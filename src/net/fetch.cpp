#include "net/fetch.hpp"

#include "page/luxp.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace luxium::net {

namespace {
// curl_global_init is not thread-safe: one init for the process, before any
// thread exists.
CURL* curl_global_once() {
    static bool init = [] {
        return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }();
    (void)init;
    return nullptr;
}

struct Sink {
    CURL*       curl;
    std::string body;
    bool        luxp_only = false;
    bool        not_luxp  = false;
    std::string etag;        // of the final response, from its headers
};

// .luxp pages already fetched, by the URL asked for: a server that sends an ETag answers "304 Not Modified"
// to If-None-Match when the page has not changed, and the body does not travel again.  Bounded; the least
// recently used page goes first.
struct CachedPage {
    std::string etag, body, content_type, final_url;
};
constexpr size_t kCachePages = 24;
constexpr size_t kCacheBytes = 48u << 20;
std::mutex g_cache_m;
std::list<std::pair<std::string, CachedPage>> g_cache_lru;
std::unordered_map<std::string, decltype(g_cache_lru)::iterator> g_cache_idx;
size_t g_cache_bytes = 0;

bool cache_find(const std::string& url, CachedPage& out) {
    std::lock_guard<std::mutex> lk(g_cache_m);
    const auto it = g_cache_idx.find(url);
    if (it == g_cache_idx.end()) return false;
    g_cache_lru.splice(g_cache_lru.begin(), g_cache_lru, it->second);
    out = it->second->second;
    return true;
}

void cache_put(const std::string& url, CachedPage page) {
    if (page.body.size() > (kCacheBytes / 4)) return;
    std::lock_guard<std::mutex> lk(g_cache_m);
    if (const auto it = g_cache_idx.find(url); it != g_cache_idx.end()) {
        g_cache_bytes -= it->second->second.body.size();
        g_cache_lru.erase(it->second);
        g_cache_idx.erase(it);
    }
    g_cache_bytes += page.body.size();
    g_cache_lru.emplace_front(url, std::move(page));
    g_cache_idx[url] = g_cache_lru.begin();
    while (g_cache_lru.size() > kCachePages || g_cache_bytes > kCacheBytes) {
        g_cache_bytes -= g_cache_lru.back().second.body.size();
        g_cache_idx.erase(g_cache_lru.back().first);
        g_cache_lru.pop_back();
    }
}

size_t header_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    const std::string line(ptr, sz * nm);
    if (line.rfind("HTTP/", 0) == 0) { s->etag.clear(); return sz * nm; }   // a new response (a redirect's, then the final one)
    if (line.size() > 5 && std::tolower(static_cast<unsigned char>(line[0])) == 'e' &&
        std::equal(line.begin(), line.begin() + 5, "etag:", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; })) {
        size_t b = 5, e = line.size();
        while (b < e && (line[b] == ' ' || line[b] == '\t')) ++b;
        while (e > b && (line[e - 1] == '\r' || line[e - 1] == '\n' || line[e - 1] == ' ')) --e;
        s->etag = line.substr(b, e - b);
    }
    return sz * nm;
}

bool luxp_type(const char* ct) {
    return ct && std::string(ct).find("application/x-luxp") != std::string::npos;
}

size_t write_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    if (s->luxp_only && s->body.empty()) {
        // The first body byte: headers of the FINAL response (redirects
        // done) are in.  Not .luxp -> stop here, Chromium takes the URL.
        char* ct = nullptr;
        curl_easy_getinfo(s->curl, CURLINFO_CONTENT_TYPE, &ct);
        if (!luxp_type(ct)) { s->not_luxp = true; return 0; }
    }
    constexpr size_t kMax = 32u << 20;   // 32 MB: a page bigger than this is
    if (s->body.size() + sz * nm > kMax) return 0;   // not a page, it is an attack
    s->body.append(ptr, sz * nm);
    return sz * nm;
}
} // namespace

std::unique_ptr<Fetch> Fetch::start(const std::string& url, bool luxp_only) {
    curl_global_once();
    auto f = std::unique_ptr<Fetch>(new Fetch());
    f->url_ = url;
    std::thread([url, luxp_only, st = f->st_] {
        CURL* c = curl_easy_init();
        if (!c) {
            std::lock_guard<std::mutex> lk(st->m);
            st->error = "curl: no se pudo inicializar";
            st->done  = true;
            return;
        }
        Sink sink{c, {}, luxp_only};
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_slist* headers = curl_slist_append(nullptr,
            "Accept: application/x-luxp, text/html;q=0.9, */*;q=0.8");
        CachedPage held;
        const bool have = cache_find(url, held);
        if (have) headers = curl_slist_append(headers, ("If-None-Match: " + held.etag).c_str());
        // The stylesheet and program blocks this origin already sent: it leaves them out of the answer.
        const std::string blocks = luxium::page::BlockStore::global().have_header(luxium::page::url_origin(url));
        // This build can rebuild a document from a template block and the trace of its values.
        headers = curl_slist_append(headers, "X-Luxp-Caps: trace");
        if (!blocks.empty())
            headers = curl_slist_append(headers, (std::string(luxium::page::kHaveHeader) + ": " + blocks).c_str());
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Luxium/0.1");
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        char* ct = nullptr;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
        std::string content_type = ct ? ct : "";
        char* eff = nullptr;
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
        std::string final_url = eff ? eff : url;
        curl_easy_cleanup(c);
        curl_slist_free_all(headers);

        if (rc == CURLE_OK && status == 304 && have) {   // what we hold is current
            sink.body = held.body;
            content_type = held.content_type;
            final_url = held.final_url;
            status = 200;
        } else if (rc == CURLE_OK && status == 200 && !sink.not_luxp && !sink.etag.empty() && luxp_type(content_type.c_str())) {
            cache_put(url, {sink.etag, sink.body, content_type, final_url});
        } else if (have && rc == CURLE_OK && status != 304) {
            std::lock_guard<std::mutex> lk(g_cache_m);   // the page is no longer what we hold
            if (auto it = g_cache_idx.find(url); it != g_cache_idx.end()) {
                g_cache_bytes -= it->second->second.body.size();
                g_cache_lru.erase(it->second);
                g_cache_idx.erase(it);
            }
        }

        std::lock_guard<std::mutex> lk(st->m);
        st->body = std::move(sink.body);
        st->content_type = content_type;
        st->final_url = final_url;
        if (sink.not_luxp) {
            st->not_luxp = true;
        } else if (rc != CURLE_OK) {
            st->error = std::string("curl: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        } else if (status >= 400) {
            st->error = "HTTP " + std::to_string(status);
        } else {
            st->ok = true;
        }
        st->done = true;
    }).detach();
    return f;
}

Fetch::~Fetch() = default;   // the thread keeps the State alive until it ends

bool Fetch::done() const { std::lock_guard<std::mutex> lk(st_->m); return st_->done; }
bool Fetch::ok() const { std::lock_guard<std::mutex> lk(st_->m); return st_->ok; }
bool Fetch::not_luxp() const { std::lock_guard<std::mutex> lk(st_->m); return st_->not_luxp; }
std::string Fetch::block_origin() const { return luxium::page::url_origin(url_); }

bool Fetch::is_luxp() const {
    std::lock_guard<std::mutex> lk(st_->m);
    return luxp_type(st_->content_type.c_str());
}
const std::string& Fetch::body() const { std::lock_guard<std::mutex> lk(st_->m); return st_->body; }
const std::string& Fetch::error() const { std::lock_guard<std::mutex> lk(st_->m); return st_->error; }
const std::string& Fetch::url() const { return url_; }
std::string Fetch::final_url() const {
    std::lock_guard<std::mutex> lk(st_->m);
    return st_->final_url.empty() ? url_ : st_->final_url;
}

} // namespace luxium::net
