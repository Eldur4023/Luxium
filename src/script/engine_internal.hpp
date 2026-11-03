#pragma once
// Small helpers shared by the engine_*.cpp parts (not a public header).

#include <script/engine.hpp>
#include <dom/node.hpp>

#include <chrono>
#include <string>

namespace luxium::script {

inline long long mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// scheme://host[:port]  path  ?query  #hash
struct UrlParts {
    std::string scheme, authority, path, query, hash;   // query/hash without their marker
    bool absolute = false;                               // had a scheme
    std::string origin() const { return scheme.empty() ? std::string() : scheme + "://" + authority; }
};

inline UrlParts split_url(const std::string& u) {
    UrlParts p;
    std::string rest = u;
    if (auto h = rest.find('#'); h != std::string::npos) { p.hash = rest.substr(h + 1); rest.resize(h); }
    if (auto q = rest.find('?'); q != std::string::npos) { p.query = rest.substr(q + 1); rest.resize(q); }
    if (auto s = rest.find("://"); s != std::string::npos && s > 0 && s < 12) {
        p.absolute = true;
        p.scheme = rest.substr(0, s);
        rest = rest.substr(s + 3);
        auto slash = rest.find('/');
        p.authority = rest.substr(0, slash);
        p.path = slash == std::string::npos ? "/" : rest.substr(slash);
    } else {
        p.path = rest;
    }
    return p;
}

inline std::string percent_decode(const std::string& s, bool plus_is_space) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (plus_is_space && s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// encodeURIComponent
inline std::string percent_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' ||
            c == '*' || c == '\'' || c == '(' || c == ')') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

} // namespace luxium::script

// ── network + object state shared by engine_net.cpp / engine_util.cpp ───────
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace luxium::script {

struct HttpDone {
    uint64_t gen = 0, coro = 0;
    bool ok = false;
    std::string err;
    long status = 0;
    std::string url, body;
    std::vector<std::pair<std::string, std::string>> headers;   // lower-case names
};
struct SockEvent {
    uint64_t gen = 0;
    long long sock = 0;
    enum Kind { Open, Message, Close } kind = Open;
    std::string data;
    int code = 0;
};
// What worker threads post and the main loop drains.  Shared with the threads,
// so a page that went away does not leave them writing into freed memory.
struct Mailbox {
    std::mutex m;
    std::deque<HttpDone> http;
    std::deque<SockEvent> socks;
};
struct SockShared {
    std::mutex m;
    std::deque<std::string> out;
    std::atomic<bool> stop{false};
    std::atomic<int> stop_code{1000};
};
struct FormData {
    std::vector<std::pair<std::string, std::string>> fields;
    struct FileRef { std::string name, path, filename; };
    std::vector<FileRef> files;
};
struct FileInfo { std::string path, name, type; long long size = 0; };

struct Engine::Net {
    std::shared_ptr<Mailbox> mail = std::make_shared<Mailbox>();
    long long next_obj = 1;
    struct Resp { long status = 0; std::string url, body; std::vector<std::pair<std::string, std::string>> headers; };
    std::map<long long, Resp> responses;
    struct Sock {
        std::shared_ptr<SockShared> shared;
        bool open = false, closed = false;
        int close_code = 0;
        std::deque<std::string> inbox;
        std::vector<uint64_t> recv_waiters, open_waiters;
        Value on_open, on_message, on_close;
    };
    std::map<long long, Sock> socks;
    std::map<long long, FormData> forms;
    std::map<long long, FileInfo> files;
    // storage.*: the current origin's key/value file
    bool store_loaded = false;
    std::string store_path;
    Value::Dict store;
};

} // namespace luxium::script
