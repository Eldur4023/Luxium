#include "net/fetch.hpp"

#include <curl/curl.h>

#include <chrono>
#include <mutex>
#include <thread>

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
};

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
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
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
        const std::string content_type = ct ? ct : "";
        char* eff = nullptr;
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
        const std::string final_url = eff ? eff : url;
        curl_easy_cleanup(c);
        curl_slist_free_all(headers);

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
