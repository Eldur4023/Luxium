#include "net/fetch.hpp"

#include <curl/curl.h>

#include <chrono>
#include <mutex>

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

size_t write_cb(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    constexpr size_t kMax = 32u << 20;   // 32 MB: a page bigger than this is
    if (s->size() + sz * nm > kMax) return 0;   // not a page, it is an attack
    s->append(ptr, sz * nm);
    return sz * nm;
}
} // namespace

std::unique_ptr<Fetch> Fetch::start(const std::string& url) {
    curl_global_once();
    auto f = std::unique_ptr<Fetch>(new Fetch());
    f->url_ = url;
    auto* body = &f->body_;
    auto* err  = &f->error_;
    auto* done = &f->done_;
    auto* ok   = &f->ok_;
    std::mutex* m = &f->m_;
    f->th_ = std::thread([url, body, err, done, ok, m] {
        CURL* c = curl_easy_init();
        if (!c) {
            std::lock_guard<std::mutex> lk(*m);
            *err  = "curl: no se pudo inicializar";
            *done = true;
            return;
        }
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Luxium/0.1");
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);

        std::lock_guard<std::mutex> lk(*m);
        if (rc != CURLE_OK) {
            *err = std::string("curl: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        } else if (status >= 400) {
            *err = "HTTP " + std::to_string(status);
        } else {
            *ok = true;
        }
        *done = true;
    });
    f->th_.detach();
    return f;
}

Fetch::~Fetch() = default;   // thread detached; the struct outlives it (see poll)

bool Fetch::done() const {
    std::lock_guard<std::mutex> lk(m_);
    return done_;
}
bool Fetch::ok() const { std::lock_guard<std::mutex> lk(m_); return ok_; }
const std::string& Fetch::body() const { std::lock_guard<std::mutex> lk(m_); return body_; }
const std::string& Fetch::error() const { std::lock_guard<std::mutex> lk(m_); return error_; }
const std::string& Fetch::url() const { return url_; }

} // namespace luxium::net
