// http.* (curl worker threads, resumed on the main loop), ws.* (libcurl's
// websocket API, one thread per socket) and storage.* (a JSON file per origin).
#include "script/engine.hpp"
#include "script/engine_internal.hpp"

#include <curl/curl.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace luxium::script {

namespace {

constexpr size_t kMaxBody = 256u << 20;   // a response bigger than this is cut and fails

struct HttpReq {
    std::string method, url, body;
    std::vector<std::string> headers;      // "Name: value"
    bool has_form = false;
    FormData form;
    uint64_t gen = 0, coro = 0;
};

struct Sink {
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool too_big = false;
};

size_t write_cb(char* p, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    if (s->body.size() + sz * nm > kMaxBody) { s->too_big = true; return 0; }
    s->body.append(p, sz * nm);
    return sz * nm;
}

size_t header_cb(char* p, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    std::string line(p, sz * nm);
    if (line.rfind("HTTP/", 0) == 0) { s->headers.clear(); return sz * nm; }   // a redirect's headers are dropped
    auto c = line.find(':');
    if (c == std::string::npos) return sz * nm;
    std::string k = line.substr(0, c), v = line.substr(c + 1);
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t\r\n");
    s->headers.emplace_back(k, a == std::string::npos ? "" : v.substr(a, b - a + 1));
    return sz * nm;
}

void curl_once() {
    static bool init = (std::srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())),
                        curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    (void)init;
}

void http_worker(HttpReq req, std::shared_ptr<Mailbox> mail) {
    HttpDone done;
    done.gen = req.gen;
    done.coro = req.coro;
    CURL* c = curl_easy_init();
    if (!c) {
        done.err = "http: curl could not start";
    } else {
        Sink sink;
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_slist* hdr = nullptr;
        for (const auto& h : req.headers) hdr = curl_slist_append(hdr, h.c_str());
        curl_mime* mime = nullptr;
        curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (req.has_form) {
            mime = curl_mime_init(c);
            for (const auto& [k, v] : req.form.fields) {
                curl_mimepart* part = curl_mime_addpart(mime);
                curl_mime_name(part, k.c_str());
                curl_mime_data(part, v.data(), v.size());
            }
            for (const auto& f : req.form.files) {
                curl_mimepart* part = curl_mime_addpart(mime);
                curl_mime_name(part, f.name.c_str());
                curl_mime_filedata(part, f.path.c_str());
                curl_mime_filename(part, f.filename.c_str());
            }
            curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
        } else if (req.method != "GET" && req.method != "DELETE") {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
        } else if (!req.body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
        }
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);     // a stalled transfer fails after a minute
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Luxium/0.1");
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        const CURLcode rc = curl_easy_perform(c);
        if (rc != CURLE_OK) {
            done.err = sink.too_big ? "http: response too large"
                                    : std::string("http: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        } else {
            done.ok = true;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &done.status);
            char* eff = nullptr;
            curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
            done.url = eff ? eff : req.url;
            done.body = std::move(sink.body);
            done.headers = std::move(sink.headers);
        }
        curl_slist_free_all(hdr);
        if (mime) curl_mime_free(mime);
        curl_easy_cleanup(c);
    }
    std::lock_guard<std::mutex> lk(mail->m);
    mail->http.push_back(std::move(done));
}

// ── websocket ───────────────────────────────────────────────────────────────

void post_sock(const std::shared_ptr<Mailbox>& mail, SockEvent ev) {
    std::lock_guard<std::mutex> lk(mail->m);
    mail->socks.push_back(std::move(ev));
}

// libcurl in Ubuntu ships without its (experimental) websocket support, so the
// worker speaks RFC 6455 itself on a CONNECT_ONLY connection: curl owns DNS,
// TCP and TLS, we own the handshake and the frames.
bool raw_send(CURL* c, curl_socket_t fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t sent = 0;
        CURLcode rc = curl_easy_send(c, p + off, n - off, &sent);
        if (rc == CURLE_AGAIN) { pollfd pf{fd, POLLOUT, 0}; ::poll(&pf, 1, 100); continue; }
        if (rc != CURLE_OK) return false;
        off += sent;
    }
    return true;
}

// Non-blocking read: >0 bytes, 0 = nothing yet, -1 = connection gone.
long raw_recv(CURL* c, char* buf, size_t cap) {
    size_t got = 0;
    CURLcode rc = curl_easy_recv(c, buf, cap, &got);
    if (rc == CURLE_AGAIN) return 0;
    if (rc != CURLE_OK) return -1;
    return got == 0 ? -1 : static_cast<long>(got);
}

std::string ws_frame(unsigned op, const std::string& payload) {
    std::string f;
    f += char(0x80 | op);
    const size_t n = payload.size();
    if (n < 126) f += char(0x80 | n);
    else if (n < 65536) { f += char(0x80 | 126); f += char(n >> 8); f += char(n & 0xFF); }
    else { f += char(0x80 | 127); for (int i = 7; i >= 0; --i) f += char((uint64_t(n) >> (i * 8)) & 0xFF); }
    unsigned char mask[4];
    for (auto& m : mask) m = static_cast<unsigned char>(std::rand() & 0xFF);
    f.append(reinterpret_cast<char*>(mask), 4);
    for (size_t i = 0; i < n; ++i) f += char(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]);
    return f;
}

std::string b64(const unsigned char* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
        o += i + 1 < n ? t[v >> 6 & 63] : '='; o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

void ws_worker(std::string url, std::string origin, uint64_t gen, long long id,
               std::shared_ptr<Mailbox> mail, std::shared_ptr<SockShared> sh) {
    auto emit = [&](SockEvent::Kind k, std::string data = {}, int code = 0) {
        SockEvent e;
        e.gen = gen; e.sock = id; e.kind = k; e.data = std::move(data); e.code = code;
        post_sock(mail, std::move(e));
    };
    const UrlParts up = split_url(url);
    CURL* c = curl_easy_init();
    if (!c) { emit(SockEvent::Close, "curl could not start", 1006); return; }
    char errbuf[CURL_ERROR_SIZE] = {0};
    const std::string http_url = (up.scheme == "wss" ? "https://" : "http://") + up.authority + "/";
    curl_easy_setopt(c, CURLOPT_URL, http_url.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode rc = curl_easy_perform(c);
    curl_socket_t fd = CURL_SOCKET_BAD;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &fd);
    if (rc != CURLE_OK || fd == CURL_SOCKET_BAD) {
        emit(SockEvent::Close, errbuf[0] ? errbuf : curl_easy_strerror(rc), 1006);
        curl_easy_cleanup(c);
        return;
    }

    // handshake
    unsigned char keyraw[16];
    for (auto& k : keyraw) k = static_cast<unsigned char>(std::rand() & 0xFF);
    std::string req = "GET " + up.path + (up.query.empty() ? "" : "?" + up.query) + " HTTP/1.1\r\nHost: " + up.authority +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + b64(keyraw, 16) +
        "\r\nSec-WebSocket-Version: 13\r\n" + (origin.empty() ? "" : "Origin: " + origin + "\r\n") + "\r\n";
    std::string in;   // bytes read and not yet consumed
    bool ok = raw_send(c, fd, req.data(), req.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    size_t head_end = std::string::npos;
    char buf[16384];
    while (ok && head_end == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        pollfd pf{fd, POLLIN, 0};
        ::poll(&pf, 1, 100);
        long n = raw_recv(c, buf, sizeof buf);
        if (n < 0) { ok = false; break; }
        in.append(buf, static_cast<size_t>(n));
        head_end = in.find("\r\n\r\n");
    }
    if (!ok || head_end == std::string::npos || in.compare(0, 12, "HTTP/1.1 101") != 0) {
        emit(SockEvent::Close, "websocket handshake failed: " + in.substr(0, std::min<size_t>(in.find("\r\n"), 60)), 1006);
        curl_easy_cleanup(c);
        return;
    }
    in.erase(0, head_end + 4);
    emit(SockEvent::Open);

    int close_code = 1006;
    bool done = false;
    std::string msg;
    unsigned msg_op = 0;
    while (!sh->stop && !done) {
        std::deque<std::string> out;
        { std::lock_guard<std::mutex> lk(sh->m); out.swap(sh->out); }
        for (auto& s : out) {
            const std::string f = ws_frame(0x1, s);
            if (!raw_send(c, fd, f.data(), f.size())) { done = true; break; }
        }
        pollfd pf{fd, POLLIN, 0};
        ::poll(&pf, 1, 30);
        for (;;) {
            long n = raw_recv(c, buf, sizeof buf);
            if (n < 0) { done = true; break; }
            if (n == 0) break;
            in.append(buf, static_cast<size_t>(n));
        }
        // parse complete frames
        while (!done) {
            if (in.size() < 2) break;
            const unsigned char b0 = in[0], b1 = in[1];
            size_t hl = 2, len = b1 & 0x7F;
            if (len == 126) { if (in.size() < 4) break; len = (unsigned char)in[2] << 8 | (unsigned char)in[3]; hl = 4; }
            else if (len == 127) {
                if (in.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = len << 8 | (unsigned char)in[2 + i];
                hl = 10;
            }
            if (len > kMaxBody) { done = true; break; }
            if (in.size() < hl + len) break;   // server frames are unmasked
            std::string payload = in.substr(hl, len);
            in.erase(0, hl + len);
            const unsigned op = b0 & 0x0F;
            const bool fin = b0 & 0x80;
            if (op == 0x8) {
                close_code = payload.size() >= 2 ? ((unsigned char)payload[0] << 8 | (unsigned char)payload[1]) : 1005;
                std::string reply = ws_frame(0x8, payload.substr(0, 2));
                raw_send(c, fd, reply.data(), reply.size());
                done = true;
            } else if (op == 0x9) {
                std::string pong = ws_frame(0xA, payload);
                raw_send(c, fd, pong.data(), pong.size());
            } else if (op == 0x1 || op == 0x2 || op == 0x0) {
                if (op != 0) { msg.clear(); msg_op = op; }
                msg += payload;
                if (fin) { emit(SockEvent::Message, std::move(msg)); msg.clear(); }
            }
        }
    }
    (void)msg_op;
    if (sh->stop && !done) {
        const int code = sh->stop_code;
        std::string payload{char(code >> 8), char(code & 0xFF)};
        std::string f = ws_frame(0x8, payload);
        raw_send(c, fd, f.data(), f.size());
        close_code = code;
    }
    emit(SockEvent::Close, {}, close_code);
    curl_easy_cleanup(c);
}

std::string safe_origin_name(const std::string& o) {
    std::string out;
    for (char ch : o) out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    return out.empty() ? "file" : out;
}

} // namespace

// ── lifecycle ───────────────────────────────────────────────────────────────

void Engine::net_reset() {
    if (!net_) return;
    for (auto& [id, s] : net_->socks)
        if (s.shared) s.shared->stop = true;
    net_->socks.clear();
    net_->responses.clear();
    net_->forms.clear();
    net_->files.clear();
    net_->store_loaded = false;
    net_->store = {};
    // Late answers carry the old generation and are dropped in net_tick.
}

void Engine::net_tick() {
    if (!net_) return;
    std::deque<HttpDone> http;
    std::deque<SockEvent> socks;
    {
        std::lock_guard<std::mutex> lk(net_->mail->m);
        http.swap(net_->mail->http);
        socks.swap(net_->mail->socks);
    }
    for (auto& d : http) {
        if (d.gen != gen_) continue;
        if (!d.ok) { resume_coro_error(d.coro, d.err); continue; }
        const long long id = net_->next_obj++;
        auto& r = net_->responses[id];
        r.status = d.status; r.url = std::move(d.url); r.body = std::move(d.body); r.headers = std::move(d.headers);
        while (net_->responses.size() > 512) net_->responses.erase(net_->responses.begin());
        resume_coro(d.coro, lux_script::make_object("Response", id));
    }
    for (auto& ev : socks) {
        if (ev.gen != gen_) continue;
        auto it = net_->socks.find(ev.sock);
        if (it == net_->socks.end()) continue;
        auto& s = it->second;
        auto fire = [&](const Value& fn, const char* kind) {
            if (!fn.is_func()) return;
            auto st = std::make_shared<EvState>();
            st->kind = kind;
            st->d.message = ev.data;
            st->d.close_code = ev.code;
            start_fn(fn, "a websocket handler", st);
        };
        switch (ev.kind) {
            case SockEvent::Open: {
                s.open = true;
                auto waiters = std::move(s.open_waiters);
                s.open_waiters.clear();
                for (uint64_t w : waiters) resume_coro(w, Value::boolean(true));
                fire(s.on_open, "open");
                break;
            }
            case SockEvent::Message: {
                if (!s.recv_waiters.empty()) {
                    uint64_t w = s.recv_waiters.front();
                    s.recv_waiters.erase(s.recv_waiters.begin());
                    resume_coro(w, Value::str(ev.data));
                } else if (s.on_message.is_func()) {
                    fire(s.on_message, "message");
                } else {
                    s.inbox.push_back(std::move(ev.data));
                    if (s.inbox.size() > 4096) s.inbox.pop_front();
                }
                break;
            }
            case SockEvent::Close: {
                s.open = false;
                s.closed = true;
                s.close_code = ev.code;
                if (!ev.data.empty()) page_.console(1, "websocket: " + ev.data);
                auto rw = std::move(s.recv_waiters);
                auto ow = std::move(s.open_waiters);
                s.recv_waiters.clear(); s.open_waiters.clear();
                for (uint64_t w : ow) resume_coro(w, Value::boolean(false));
                for (uint64_t w : rw) resume_coro(w, Value::null());
                fire(s.on_close, "close");
                break;
            }
        }
    }
}

// ── async entry points ──────────────────────────────────────────────────────

bool Engine::net_begin(Coro& c, const std::string& name, Args& a, Value& out, std::string& err) {
    if (name == "socket_recv" || name == "socket_wait_open") {
        std::string type;
        long long h = 0;
        if (!lux_script::host_object_of(a[0], type, h) || type != "Socket" || !net_->socks.count(h)) {
            err = "recv(): not a Socket";
            return true;
        }
        auto& s = net_->socks[h];
        if (name == "socket_recv") {
            if (!s.inbox.empty()) { out = Value::str(std::move(s.inbox.front())); s.inbox.pop_front(); return true; }
            if (s.closed) { out = Value::null(); return true; }
            s.recv_waiters.push_back(c.id);
            return false;
        }
        if (s.open || s.closed) { out = Value::boolean(s.open); return true; }
        s.open_waiters.push_back(c.id);
        return false;
    }

    static const char* kVerbs[][2] = {{"http_get", "GET"}, {"http_delete", "DELETE"}, {"http_post", "POST"},
                                      {"http_put", "PUT"}, {"http_patch", "PATCH"}};
    const char* method = nullptr;
    for (auto& v : kVerbs) if (name == v[0]) method = v[1];
    if (!method) { err = "'" + name + "' cannot be awaited here"; return true; }

    const bool has_body = std::string(method) != "GET" && std::string(method) != "DELETE";
    if (a.empty() || !a[0].is_str()) { err = "http: the URL must be a string"; return true; }

    HttpReq req;
    req.method = method;
    req.url = resolve_url(a[0].as_str());
    req.gen = gen_;
    req.coro = c.id;
    const std::string o = origin();
    if (o.empty()) { err = "http: this page has no network origin (it was opened from a file); serve it over http"; return true; }
    const UrlParts up = split_url(req.url);
    if (!up.absolute || (up.scheme != "http" && up.scheme != "https")) { err = "http: only http(s) URLs"; return true; }
    if (up.origin() != o) { err = "http: cross-origin request blocked (" + up.origin() + " is not " + o + ")"; return true; }

    size_t hdr_i = has_body ? 2 : 1;
    if (has_body) {
        const Value& b = a.size() > 1 ? a[1] : Value::null();
        std::string type;
        long long h = 0;
        if (b.is_str()) req.body = b.as_str();
        else if (b.is_null()) req.body.clear();
        else if (lux_script::host_object_of(b, type, h) && type == "Form" && net_->forms.count(h)) {
            req.has_form = true;
            req.form = net_->forms[h];
        } else if (b.is_list() || b.is_dict()) {
            req.body = b.to_json_text();
            req.headers.push_back("Content-Type: application/json");
        } else {
            req.body = b.to_string();
        }
    }
    if (a.size() > hdr_i && a[hdr_i].is_dict())
        for (const auto& [k, v] : a[hdr_i].as_dict()) req.headers.push_back(k + ": " + v.to_string());

    curl_once();
    std::thread(http_worker, std::move(req), net_->mail).detach();
    return false;
}

// ── registration ────────────────────────────────────────────────────────────

void Engine::register_net() {
    net_ = std::make_shared<Net>();

    auto resp = [this](long long h, std::string& e) -> Net::Resp* {
        auto it = net_->responses.find(h);
        if (it == net_->responses.end()) { e = "this Response expired (too many newer requests)"; return nullptr; }
        return &it->second;
    };
    reg_method("Response", "status", [resp](long long h, Args&, std::string& e) -> Value {
        auto* r = resp(h, e); return r ? Value::integer(r->status) : Value::null();
    });
    reg_method("Response", "ok", [resp](long long h, Args&, std::string& e) -> Value {
        auto* r = resp(h, e); return r ? Value::boolean(r->status >= 200 && r->status < 300) : Value::null();
    });
    reg_method("Response", "text", [resp](long long h, Args&, std::string& e) -> Value {
        auto* r = resp(h, e); return r ? Value::str(r->body) : Value::null();
    });
    reg_method("Response", "url", [resp](long long h, Args&, std::string& e) -> Value {
        auto* r = resp(h, e); return r ? Value::str(r->url) : Value::null();
    });
    reg_method("Response", "json", [resp](long long h, Args&, std::string& e) -> Value {
        auto* r = resp(h, e);
        if (!r) return Value::null();
        Value v;
        if (!Value::parse_json(r->body, v)) { e = "json(): the response body is not valid JSON"; return Value::null(); }
        return v;
    });
    reg_method("Response", "header", [resp](long long h, Args& a, std::string& e) -> Value {
        auto* r = resp(h, e);
        if (!r) return Value::null();
        std::string want = a[0].to_string();
        for (auto& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (const auto& [k, v] : r->headers) if (k == want) return Value::str(v);
        return Value::null();
    });

    // ws
    reg("ws_connect", [this](Args& a, std::string& e) -> Value {
        if (!a[0].is_str()) { e = "ws.connect() expects a URL"; return Value::null(); }
        const std::string o = origin();
        if (o.empty()) { e = "ws: this page has no network origin (it was opened from a file)"; return Value::null(); }
        UrlParts up = split_url(resolve_url(a[0].as_str()));
        if (up.scheme == "http") up.scheme = "ws"; else if (up.scheme == "https") up.scheme = "wss";
        const UrlParts po = split_url(o);
        const std::string want_scheme = po.scheme == "https" ? "wss" : "ws";
        if (up.authority != po.authority || up.scheme != want_scheme) { e = "ws: only the page's own host can be reached"; return Value::null(); }
        std::string url = up.scheme + "://" + up.authority + up.path + (up.query.empty() ? "" : "?" + up.query);
        const long long id = net_->next_obj++;
        auto& s = net_->socks[id];
        s.shared = std::make_shared<SockShared>();
        curl_once();
        std::thread(ws_worker, url, o, gen_, id, net_->mail, s.shared).detach();
        return lux_script::make_object("Socket", id);
    });
    auto sock = [this](long long h, std::string& e) -> Net::Sock* {
        auto it = net_->socks.find(h);
        if (it == net_->socks.end()) { e = "this Socket belongs to a page that is gone"; return nullptr; }
        return &it->second;
    };
    reg_method("Socket", "send", [sock](long long h, Args& a, std::string& e) -> Value {
        auto* s = sock(h, e);
        if (!s) return Value::null();
        if (s->closed) { e = "send(): the socket is closed"; return Value::null(); }
        std::lock_guard<std::mutex> lk(s->shared->m);
        s->shared->out.push_back(a[0].to_string());
        return Value::null();
    });
    reg_method("Socket", "close", [sock](long long h, Args& a, std::string& e) -> Value {
        auto* s = sock(h, e);
        if (!s) return Value::null();
        if (!a.empty() && a[0].is_int()) s->shared->stop_code = static_cast<int>(a[0].as_int());
        s->shared->stop = true;
        return Value::null();
    });
    reg_method("Socket", "is_open", [sock](long long h, Args&, std::string& e) -> Value {
        auto* s = sock(h, e); return s ? Value::boolean(s->open && !s->closed) : Value::null();
    });
    auto handler = [this, sock](const char* which) {
        return [sock, which](long long h, Args& a, std::string& e) -> Value {
            auto* s = sock(h, e);
            if (!s) return Value::null();
            if (!a[0].is_func()) { e = std::string(which) + "(fn) expects a function"; return Value::null(); }
            (std::string(which) == "on_open" ? s->on_open : std::string(which) == "on_message" ? s->on_message : s->on_close) = a[0];
            return Value::null();
        };
    };
    reg_method("Socket", "on_open", handler("on_open"));
    reg_method("Socket", "on_message", handler("on_message"));
    reg_method("Socket", "on_close", handler("on_close"));

    // storage.* -- one JSON file per origin under <profile>/storage/
    auto store = [this]() -> Value::Dict& {
        if (!net_->store_loaded) {
            net_->store_loaded = true;
            net_->store = {};
            std::string dir = page_.profile_dir();
            if (dir.empty()) {
                const char* env = std::getenv("LUXIUM_PROFILE_DIR");
                const char* home = std::getenv("HOME");
                dir = env && *env ? env : std::string(home ? home : "/tmp") + "/.local/share/luxium";
            }
            net_->store_path = dir + "/storage/" + safe_origin_name(origin()) + ".json";
            std::ifstream f(net_->store_path);
            if (f) {
                std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                Value v;
                if (Value::parse_json(text, v) && v.is_dict()) net_->store = v.as_dict();
            }
        }
        return net_->store;
    };
    auto save = [this]() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(net_->store_path).parent_path(), ec);
        const std::string tmp = net_->store_path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) { page_.console(1, "storage: cannot write " + net_->store_path); return; }
            f << Value::dict(net_->store).to_json_text();
        }
        fs::rename(tmp, net_->store_path, ec);
    };
    reg("storage_get", [store](Args& a, std::string& e) -> Value {
        if (!a[0].is_str()) { e = "storage.get() expects a key"; return Value::null(); }
        auto& d = store();
        auto it = d.find(a[0].as_str());
        return it == d.end() ? Value::null() : it->second;
    });
    reg("storage_set", [store, save](Args& a, std::string& e) -> Value {
        if (!a[0].is_str()) { e = "storage.set() expects a key"; return Value::null(); }
        store().set(std::string(a[0].as_str()), Value::str(a[1].to_string()));
        save();
        return Value::null();
    });
    reg("storage_remove", [store, save](Args& a, std::string& e) -> Value {
        if (!a[0].is_str()) { e = "storage.remove() expects a key"; return Value::null(); }
        if (store().erase(a[0].as_str())) save();
        return Value::null();
    });
    reg("storage_keys", [store](Args&, std::string&) -> Value {
        Value::List out;
        for (const auto& [k, _] : store()) out.push_back(Value::str(k));
        return Value::list(std::move(out));
    });
    reg("storage_clear", [store, save](Args&, std::string&) -> Value {
        store().clear();
        save();
        return Value::null();
    });
}

} // namespace luxium::script
