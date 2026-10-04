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
#include <thread>

namespace luxium::net {

class Fetch {
public:
    // Starts immediately on its own thread.
    static std::unique_ptr<Fetch> start(const std::string& url);

    ~Fetch();

    bool done() const;
    bool ok() const;
    const std::string& body() const;
    const std::string& error() const;
    const std::string& url() const;

private:
    std::string url_;
    std::string body_;
    std::string error_;
    bool done_ = false;
    bool ok_   = false;
    mutable std::mutex m_;
    std::thread th_;
};

} // namespace luxium::net
