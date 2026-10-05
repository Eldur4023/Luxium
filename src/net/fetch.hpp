#pragma once
// One asynchronous HTTP(S) GET at a time, libcurl on a worker thread.
//
// The UI thread never blocks: it starts a fetch, keeps painting, and polls
// done() once per frame.  The result moves out with take().  TLS, redirects
// and timeouts are curl's job -- a browser that hand-rolls TLS is a vulnerability
// with a UI.

#include <memory>
#include <mutex>
#include <string>

namespace luxium::net {

class Fetch {
public:
    // Starts immediately on its own thread.  Every request says Luxium takes
    // .luxp (Accept: application/x-luxp, text/html;q=0.9): a Lux server
    // answers its compiled format, any other server its HTML.  luxp_only:
    // the caller has somewhere better for anything else (Chromium), so a
    // response that is not .luxp is cut at its first body byte -- the page
    // is not downloaded twice.
    static std::unique_ptr<Fetch> start(const std::string& url, bool luxp_only = false);

    ~Fetch();

    bool done() const;
    bool ok() const;
    bool is_luxp() const;      // the response is application/x-luxp
    bool not_luxp() const;     // luxp_only and the server answered something else
    const std::string& body() const;    // valid once done()
    const std::string& error() const;
    const std::string& url() const;

private:
    // Shared with the worker thread: dropping a Fetch mid-transfer (a newer
    // navigation wins) must not leave the thread writing into freed memory.
    struct State {
        std::mutex  m;
        std::string body, error, content_type;
        bool done = false, ok = false, not_luxp = false;
    };
    std::string            url_;
    std::shared_ptr<State> st_ = std::make_shared<State>();
};

} // namespace luxium::net
