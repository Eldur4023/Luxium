// Luxium app host: `luxium --host`.  ONE browser process serves every desktop
// app that asks for a window (Lux Desktop, ...): one Chromium instance, one
// chrome-less SDL window per app.  See docs/HOST.md for the protocol.
//
//   socket   $XDG_RUNTIME_DIR/luxium/host.sock   (dir 0700, socket 0600)
//   lock     $XDG_RUNTIME_DIR/luxium/host.lock   (flock held for the host's whole life)
//   protocol one JSON object per line, both directions; one connection = one window
//
// Threads: the main thread owns SDL + every window; a reader thread owns the
// socket I/O and hands complete lines over through a queue; CEF runs its own
// UI thread and only pokes us through Events (Shared state + an SDL user event).

#include "host/app_host.hpp"

#include <cef/cef_backend.hpp>
#include <cef/sdl_input.hpp>
#include <render/images.hpp>

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xlib.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luxium::host {

using Clock = std::chrono::steady_clock;

// ── paths ───────────────────────────────────────────────────────────────────
std::string runtime_dir() {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::string(x) + "/luxium";
    return "/tmp/luxium-" + std::to_string(getuid());
}
std::string socket_path() { return runtime_dir() + "/host.sock"; }
std::string lock_path()   { return runtime_dir() + "/host.lock"; }

namespace {

// ── flat JSON (what the protocol needs: one object, scalar values) ──────────
using Obj = std::map<std::string, std::string>;   // strings decoded, other scalars raw

void put_utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

bool parse_obj(const std::string& s, Obj& o) {
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; };
    auto str = [&](std::string& out) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > s.size()) return false;
                unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(out, cp);
                break;
            }
            default: out += e;
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    ws();
    if (i < s.size() && s[i] == '}') return true;
    for (;;) {
        ws();
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        ws();
        if (i < s.size() && s[i] == '"') { if (!str(v)) return false; }
        else while (i < s.size() && s[i] != ',' && s[i] != '}') v += s[i++];
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        o[k] = v;
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        return i < s.size() && s[i] == '}';
    }
}

std::string jq(const std::string& s) {   // JSON string literal
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

std::string get(const Obj& o, const char* k, const std::string& d = "") {
    auto it = o.find(k);
    return it == o.end() ? d : it->second;
}
int geti(const Obj& o, const char* k, int d) {
    auto it = o.find(k);
    return it == o.end() || it->second.empty() ? d : std::atoi(it->second.c_str());
}
bool getb(const Obj& o, const char* k, bool d) {
    auto it = o.find(k);
    return it == o.end() ? d : it->second == "true";
}

void send_line(int fd, const std::string& line) {
    if (fd < 0) return;
    std::string l = line + "\n";
    size_t off = 0;
    while (off < l.size()) {
        ssize_t n = ::send(fd, l.data() + off, l.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

// ── reader thread: accepts connections, splits lines ────────────────────────
struct In { int conn; int fd; std::string line; bool eof; };

struct Reader {
    int listen_fd = -1;
    int wake[2] = {-1, -1};
    std::mutex m;
    std::deque<In> q;
    std::thread th;
    int next_conn = 0;

    void push(In in) {
        { std::lock_guard<std::mutex> lk(m); q.push_back(std::move(in)); }
        SDL_Event e{};
        e.type = SDL_USEREVENT;
        SDL_PushEvent(&e);   // thread-safe: wakes the main loop
    }
    void loop() {
        struct C { int id, fd; std::string buf; };
        std::vector<C> cs;
        for (;;) {
            std::vector<pollfd> pf = {{listen_fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
            for (auto& c : cs) pf.push_back({c.fd, POLLIN, 0});
            if (::poll(pf.data(), pf.size(), -1) < 0) { if (errno == EINTR) continue; return; }
            if (pf[1].revents) return;
            if (pf[0].revents & POLLIN) {
                int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd >= 0) {
                    cs.push_back({++next_conn, fd, {}});
                    push({cs.back().id, fd, {}, false});   // line empty + !eof = "accepted"
                }
            }
            for (size_t i = 0; i < cs.size();) {
                bool dead = false;
                // connections accepted this round have no pollfd yet
                if (i + 2 < pf.size() && (pf[i + 2].revents & (POLLIN | POLLHUP | POLLERR))) {
                    char b[4096];
                    ssize_t n = ::read(cs[i].fd, b, sizeof b);
                    if (n <= 0) dead = true;
                    else {
                        cs[i].buf.append(b, (size_t)n);
                        size_t p;
                        while ((p = cs[i].buf.find('\n')) != std::string::npos) {
                            std::string line = cs[i].buf.substr(0, p);
                            cs[i].buf.erase(0, p + 1);
                            if (!line.empty()) push({cs[i].id, cs[i].fd, std::move(line), false});
                        }
                        if (cs[i].buf.size() > (1u << 20)) dead = true;   // absurd line
                    }
                }
                if (dead) {
                    push({cs[i].id, cs[i].fd, {}, true});   // main closes the fd
                    cs.erase(cs.begin() + (long)i);
                    pf.erase(pf.begin() + (long)i + 2);
                } else ++i;
            }
        }
    }
};

// ── windows ─────────────────────────────────────────────────────────────────
// State the CEF UI thread writes, the main thread reads.
struct Shared {
    std::mutex m;
    std::string title; bool title_dirty = false;
    std::string addr;  bool addr_dirty = false;
    std::deque<std::string> msgs;
    bool loaded = false;
    std::atomic<bool> closed{false};
};

struct AppWin {
    int conn = -1;                 // owning connection, -1 for DevTools windows
    AppWin* parent = nullptr;      // DevTools: the inspected window
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    cef::Win* cw = nullptr;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    int w = 0, h = 0;
    std::vector<uint8_t> buf;      // window-sized BGRA
    int last_paints = -1;
    bool dirty = true;
    bool dev = false, dark = false;
    bool page_fs = false, user_fs = false;
    int cursor = -1;
    unsigned long x11_display = 0;
    bool closing = false;
    Clock::time_point close_by;
    std::string app_id;
    bool loaded_sent = false;
};

struct Conn { int fd; AppWin* win = nullptr; };

std::atomic<bool> g_term{false};
void on_term(int) { g_term = true; }

class Host {
public:
    int run();

private:
    Reader rd_;
    std::map<int, Conn> conns_;
    std::vector<std::unique_ptr<AppWin>> wins_;
    bool cef_up_ = false;
    Clock::time_point idle_since_ = Clock::now();

    AppWin* by_sdl(Uint32 id) {
        for (auto& w : wins_) if (w->win && SDL_GetWindowID(w->win) == id) return w.get();
        return nullptr;
    }
    void on_line(int conn, const std::string& line);
    void cmd_open(int conn, const Obj& o);
    void resize_surface(AppWin* w);
    void begin_close(AppWin* w);
    void reap();
    void render(AppWin* w);
    void handle_sdl(const SDL_Event& e, bool& quit);
    void pump_shared(AppWin* w);
    void set_fullscreen(AppWin* w);
    void open_devtools(AppWin* w);
    cef::Events make_events(AppWin* w);
    void ev(AppWin* w, const std::string& json) { if (w->conn >= 0 && conns_.count(w->conn)) send_line(conns_[w->conn].fd, json); }
};

cef::Events Host::make_events(AppWin* w) {
    std::shared_ptr<Shared> sh = w->sh;
    cef::Events e;
    e.wake = [] { SDL_Event u{}; u.type = SDL_USEREVENT; SDL_PushEvent(&u); };
    e.on_title = [sh](const std::string& t) { std::lock_guard<std::mutex> lk(sh->m); sh->title = t; sh->title_dirty = true; };
    e.on_address = [sh](const std::string& a) { std::lock_guard<std::mutex> lk(sh->m); sh->addr = a; sh->addr_dirty = true; };
    e.on_message = [sh](const std::string& m) { std::lock_guard<std::mutex> lk(sh->m); sh->msgs.push_back(m); };
    e.on_loaded = [sh] { std::lock_guard<std::mutex> lk(sh->m); sh->loaded = true; };
    e.on_closed = [sh] { sh->closed = true; };
    return e;
}

void Host::resize_surface(AppWin* w) {
    int nw = 0, nh = 0;
    SDL_GetWindowSize(w->win, &nw, &nh);
    if (nw <= 0 || nh <= 0) return;
    if (nw == w->w && nh == w->h && w->tex) return;
    w->w = nw;
    w->h = nh;
    w->buf.assign((size_t)nw * nh * 4, 0xFF);
    if (w->tex) SDL_DestroyTexture(w->tex);
    w->tex = SDL_CreateTexture(w->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, nw, nh);
    cef::set_view(w->cw, nw, nh);
    w->dirty = true;
    if (w->conn >= 0) ev(w, "{\"ev\":\"resized\",\"width\":" + std::to_string(nw) + ",\"height\":" + std::to_string(nh) + "}");
}

void Host::cmd_open(int conn, const Obj& o) {
    Conn& c = conns_[conn];
    if (c.win) { send_line(c.fd, "{\"ev\":\"error\",\"message\":\"window already open\"}"); return; }
    const std::string app_id = get(o, "app_id", "app");
    const int width = std::clamp(geti(o, "width", 1024), 100, 10000);
    const int height = std::clamp(geti(o, "height", 768), 100, 10000);
    const bool resizable = getb(o, "resizable", true);

    auto w = std::make_unique<AppWin>();
    w->conn = conn;
    w->app_id = app_id;
    w->dev = getb(o, "devtools", false);
    w->dark = getb(o, "dark", false);
    // WM_CLASS = app id, so the shell groups/pins each app separately.
    ::setenv("SDL_VIDEO_X11_WMCLASS", app_id.c_str(), 1);
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | (resizable ? SDL_WINDOW_RESIZABLE : 0);
    w->win = SDL_CreateWindow(get(o, "title", "Luxium").c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width, height, flags);
    if (!w->win) {
        send_line(c.fd, "{\"ev\":\"error\",\"message\":" + jq(SDL_GetError()) + "}");
        return;
    }
    if (resizable) SDL_SetWindowMinimumSize(w->win, std::max(1, geti(o, "min_width", 0)), std::max(1, geti(o, "min_height", 0)));
    const std::string icon = get(o, "icon");
    if (!icon.empty())
        if (const auto* bmp = render::load_image(icon)) {   // RGB only: alpha was flattened on white
            if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(bmp->rgb.data()), bmp->w, bmp->h,
                                                                    24, bmp->w * 3, SDL_PIXELFORMAT_RGB24)) {
                SDL_SetWindowIcon(w->win, s);
                SDL_FreeSurface(s);
            }
        }
    w->ren = SDL_CreateRenderer(w->win, -1, 0);
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(w->win, &wm) || wm.subsystem != SDL_SYSWM_X11) {
        send_line(c.fd, "{\"ev\":\"error\",\"message\":\"no X11 display\"}");
        SDL_DestroyRenderer(w->ren);
        SDL_DestroyWindow(w->win);
        return;
    }
    w->x11_display = (unsigned long)wm.info.x11.display;

    if (!cef_up_) {
        cef::set_profile_root(get(o, "profile_root", std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.local/share/luxium/apps"));
        cef_up_ = true;
    }
    cef::OpenParams p;
    p.url = get(o, "url", "about:blank");
    p.profile_id = app_id;
    p.display = w->x11_display;
    p.parent = (unsigned long)wm.info.x11.window;
    p.events = make_events(w.get());
    w->cw = cef::open_window(p);
    std::signal(SIGTERM, on_term);   // CEF's init replaced the handlers
    std::signal(SIGINT, on_term);
    resize_surface(w.get());
    cef::set_focus(w->cw, true);
    c.win = w.get();
    send_line(c.fd, "{\"ev\":\"opened\",\"width\":" + std::to_string(w->w) + ",\"height\":" + std::to_string(w->h) + "}");
    wins_.push_back(std::move(w));
}

void Host::open_devtools(AppWin* par) {
    for (auto& x : wins_) if (x->parent == par && !x->closing) return;   // one inspector per window
    auto w = std::make_unique<AppWin>();
    w->parent = par;
    w->win = SDL_CreateWindow("DevTools", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 720,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w->win) return;
    w->ren = SDL_CreateRenderer(w->win, -1, 0);
    w->x11_display = par->x11_display;
    w->dev = true;
    w->cw = cef::open_devtools(par->cw, make_events(w.get()));
    if (!w->cw) { SDL_DestroyRenderer(w->ren); SDL_DestroyWindow(w->win); return; }
    resize_surface(w.get());
    cef::set_focus(w->cw, true);
    wins_.push_back(std::move(w));
}

void Host::begin_close(AppWin* w) {
    if (w->closing) return;
    w->closing = true;
    w->close_by = Clock::now() + std::chrono::seconds(3);
    SDL_HideWindow(w->win);
    if (w->conn >= 0) {
        ev(w, "{\"ev\":\"closed\"}");
        auto it = conns_.find(w->conn);
        if (it != conns_.end()) it->second.win = nullptr;   // no more events to this client
    }
    cef::close_window(w->cw);
    for (auto& x : wins_) if (x->parent == w) begin_close(x.get());
}

// Finishes windows whose browser is gone (or that took too long): frees SDL/CEF and drops the connection.
void Host::reap() {
    const auto now = Clock::now();
    for (size_t i = 0; i < wins_.size();) {
        AppWin* w = wins_[i].get();
        if (!w->closing) { ++i; continue; }
        const bool done = w->sh->closed.load();
        if (!done && now < w->close_by) {
            cef::close_window(w->cw);   // no-op once closed; covers a browser still being created
            ++i;
            continue;
        }
        if (w->conn >= 0) {
            auto it = conns_.find(w->conn);
            if (it != conns_.end()) ::shutdown(it->second.fd, SHUT_RDWR);   // reader sees EOF -> main closes fd
        }
        if (done) cef::destroy_window(w->cw);   // else leaked on purpose: CEF may still call back into it
        if (w->tex) SDL_DestroyTexture(w->tex);
        if (w->ren) SDL_DestroyRenderer(w->ren);
        if (w->win) SDL_DestroyWindow(w->win);
        wins_.erase(wins_.begin() + (long)i);
    }
}

void Host::set_fullscreen(AppWin* w) {
    SDL_SetWindowFullscreen(w->win, (w->page_fs || w->user_fs) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Host::pump_shared(AppWin* w) {
    std::string title, addr;
    bool td = false, ad = false;
    std::deque<std::string> msgs;
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lk(w->sh->m);
        td = w->sh->title_dirty; ad = w->sh->addr_dirty;
        title = w->sh->title; addr = w->sh->addr;
        w->sh->title_dirty = w->sh->addr_dirty = false;
        msgs.swap(w->sh->msgs);
        loaded = w->sh->loaded;
    }
    if (w->conn < 0) {   // DevTools window: its own title
        if (td && !title.empty()) SDL_SetWindowTitle(w->win, title.c_str());
    } else {
        // Page titles are reported, not applied: the window title is the app's (set_title).
        if (td) ev(w, "{\"ev\":\"title\",\"title\":" + jq(title) + "}");
        if (ad) ev(w, "{\"ev\":\"url\",\"url\":" + jq(addr) + "}");
        if (loaded && !w->loaded_sent) { w->loaded_sent = true; ev(w, "{\"ev\":\"loaded\"}"); }
        for (auto& m : msgs) ev(w, "{\"ev\":\"message\",\"data\":" + jq(m) + "}");
    }
    const bool fs = cef::fullscreen(w->cw);
    if (fs != w->page_fs) { w->page_fs = fs; set_fullscreen(w); }
    const int cur = cef::cursor(w->cw);
    if (cur != w->cursor) {
        w->cursor = cur;
        if (SDL_GetMouseFocus() == w->win) {
            if (cur == 36) SDL_ShowCursor(SDL_DISABLE);
            else {
                SDL_ShowCursor(SDL_ENABLE);
                static SDL_Cursor* cache[SDL_NUM_SYSTEM_CURSORS] = {};
                const SDL_SystemCursor id = cef_input::cursor_for(cur);
                if (!cache[id]) cache[id] = SDL_CreateSystemCursor(id);
                if (cache[id]) SDL_SetCursor(cache[id]);
            }
        }
    }
}

void Host::render(AppWin* w) {
    if (w->closing || !w->tex) return;
    const int p = cef::paints(w->cw);
    if (p == w->last_paints && !w->dirty) return;
    w->last_paints = p;
    w->dirty = false;
    int fw = 0, fh = 0;
    if (cef::copy_frame(w->cw, w->buf.data(), w->w * 4, w->w, w->h, &fw, &fh))
        SDL_UpdateTexture(w->tex, nullptr, w->buf.data(), w->w * 4);
    else {   // nothing painted yet: no white flash on a dark app
        if (w->dark) SDL_SetRenderDrawColor(w->ren, 0x1b, 0x1b, 0x1f, 255);
        else SDL_SetRenderDrawColor(w->ren, 255, 255, 255, 255);
        SDL_RenderClear(w->ren);
        SDL_RenderPresent(w->ren);
        return;
    }
    SDL_RenderCopy(w->ren, w->tex, nullptr, nullptr);
    SDL_RenderPresent(w->ren);
}

void Host::handle_sdl(const SDL_Event& e, bool& quit) {
    using namespace cef_input;
    switch (e.type) {
    case SDL_QUIT: quit = true; break;
    case SDL_WINDOWEVENT: {
        AppWin* w = by_sdl(e.window.windowID);
        if (!w || w->closing) break;
        switch (e.window.event) {
        case SDL_WINDOWEVENT_CLOSE: begin_close(w); break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED: resize_surface(w); break;
        case SDL_WINDOWEVENT_EXPOSED: w->dirty = true; break;
        case SDL_WINDOWEVENT_FOCUS_GAINED: cef::set_focus(w->cw, true); break;
        case SDL_WINDOWEVENT_FOCUS_LOST: cef::set_focus(w->cw, false); break;
        case SDL_WINDOWEVENT_LEAVE: cef::mouse_move(w->cw, 0, 0, true, 0); break;
        }
        break;
    }
    case SDL_MOUSEMOTION:
        if (AppWin* w = by_sdl(e.motion.windowID); w && !w->closing)
            cef::mouse_move(w->cw, e.motion.x, e.motion.y, false, cef_mods_flags(SDL_GetModState(), false));
        break;
    case SDL_MOUSEWHEEL:
        if (AppWin* w = by_sdl(e.wheel.windowID); w && !w->closing) {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            float dy = e.wheel.preciseY, dx = e.wheel.preciseX;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dy = -dy; dx = -dx; }
            cef::mouse_wheel(w->cw, mx, my, (int)(dx * 100), (int)(dy * 100), cef_mods_flags(SDL_GetModState(), false));
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (AppWin* w = by_sdl(e.button.windowID); w && !w->closing) {
            const int btn = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
            cef::mouse_button(w->cw, e.button.x, e.button.y, btn, e.type == SDL_MOUSEBUTTONUP, e.button.clicks,
                              cef_mods_flags(SDL_GetModState(), false));
        }
        break;
    case SDL_TEXTINPUT:
        if (AppWin* w = by_sdl(e.text.windowID); w && !w->closing) {
            if ((unsigned char)e.text.text[0] < 0x80) cef::key(w->cw, 2, cef_mods_flags(SDL_GetModState(), false), 0, 0, e.text.text);
            else cef::ime_commit(w->cw, e.text.text);
        }
        break;
    case SDL_TEXTEDITING:
        if (AppWin* w = by_sdl(e.edit.windowID); w && !w->closing) cef::ime_compose(w->cw, e.edit.text, e.edit.start);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        AppWin* w = by_sdl(e.key.windowID);
        if (!w || w->closing) break;
        const SDL_Keysym& k = e.key.keysym;
        const bool down = e.type == SDL_KEYDOWN;
        if (down) {
            if (w->page_fs && k.sym == SDLK_ESCAPE) { cef::exit_fullscreen(w->cw); break; }
            if (w->user_fs && k.sym == SDLK_ESCAPE) { w->user_fs = false; set_fullscreen(w); break; }
            if (w->dev && !w->parent && (k.sym == SDLK_F12 || ((k.mod & KMOD_CTRL) && (k.mod & KMOD_SHIFT) && k.sym == SDLK_i))) {
                open_devtools(w);
                break;
            }
            if (w->dev && (k.sym == SDLK_F5 || ((k.mod & KMOD_CTRL) && k.sym == SDLK_r))) { cef::reload(w->cw); break; }
            if ((k.mod & KMOD_CTRL) && !(k.mod & (KMOD_ALT | KMOD_SHIFT)) &&
                (k.sym == SDLK_c || k.sym == SDLK_x || k.sym == SDLK_v || k.sym == SDLK_a)) {
                cef::edit(w->cw, (char)k.sym);
                break;
            }
        }
        const uint32_t vk = cef_vk(k.sym);
        if (vk == 0) break;
        uint32_t native = 0;
        if (const uint32_t xs = cef_x_keysym(k.sym); xs && w->x11_display)
            native = XKeysymToKeycode((Display*)w->x11_display, (KeySym)xs);
        cef::key(w->cw, down ? 0 : 1, cef_mods_flags(k.mod, down && e.key.repeat), vk, native, nullptr);
        break;
    }
    default: break;
    }
}

void Host::on_line(int conn, const std::string& line) {
    auto ci = conns_.find(conn);
    if (ci == conns_.end()) return;
    Obj o;
    if (!parse_obj(line, o)) { send_line(ci->second.fd, "{\"ev\":\"error\",\"message\":\"bad json\"}"); return; }
    const std::string cmd = get(o, "cmd");
    if (cmd == "open") { cmd_open(conn, o); return; }
    if (cmd == "ping") { send_line(ci->second.fd, "{\"ev\":\"pong\"}"); return; }
    AppWin* w = ci->second.win;
    if (!w || w->closing) return;
    if (cmd == "navigate") cef::navigate(w->cw, get(o, "url"));
    else if (cmd == "reload") cef::reload(w->cw);
    else if (cmd == "set_title") SDL_SetWindowTitle(w->win, get(o, "title").c_str());
    else if (cmd == "resize") SDL_SetWindowSize(w->win, std::clamp(geti(o, "width", w->w), 100, 10000), std::clamp(geti(o, "height", w->h), 100, 10000));
    else if (cmd == "eval") cef::run_js(w->cw, get(o, "code"));
    else if (cmd == "post_message")   // delivered to the page as a 'luxium' CustomEvent whose detail is the string
        cef::run_js(w->cw, "window.dispatchEvent(new CustomEvent('luxium',{detail:" + jq(get(o, "data")) + "}))");
    else if (cmd == "devtools") open_devtools(w);
    else if (cmd == "close") begin_close(w);
    else if (cmd == "minimize") SDL_MinimizeWindow(w->win);
    else if (cmd == "maximize") SDL_MaximizeWindow(w->win);
    else if (cmd == "restore") { w->user_fs = false; set_fullscreen(w); SDL_RestoreWindow(w->win); }
    else if (cmd == "fullscreen") { w->user_fs = getb(o, "on", true); set_fullscreen(w); }
    else if (cmd == "always_on_top") SDL_SetWindowAlwaysOnTop(w->win, getb(o, "on", true) ? SDL_TRUE : SDL_FALSE);
    else if (cmd == "clipboard_write") SDL_SetClipboardText(get(o, "text").c_str());
    else if (cmd == "clipboard_read") {
        char* t = SDL_GetClipboardText();
        send_line(ci->second.fd, "{\"ev\":\"result\",\"id\":" + get(o, "id", "0") + ",\"value\":" + jq(t ? t : "") + "}");
        if (t) SDL_free(t);
    }
}

int Host::run() {
    const std::string dir = runtime_dir();
    ::mkdir(dir.c_str(), 0700);
    ::chmod(dir.c_str(), 0700);

    // Lifetime lock: a host that is still shutting down keeps it, so its successor waits instead of
    // fighting it for the profile.  Bounded wait: a stuck host must not stack up waiters forever.
    int lock_fd = ::open(lock_path().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) { std::perror("luxium --host: lock"); return 1; }
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (Clock::now() > deadline) return 0;   // another host is alive: clients connect to it
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // We own the lock, so any socket file is stale.
    ::unlink(socket_path().c_str());
    rd_.listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, socket_path().c_str(), sizeof sa.sun_path - 1);
    const mode_t old_umask = ::umask(0177);
    const int br = ::bind(rd_.listen_fd, (sockaddr*)&sa, sizeof sa);
    ::umask(old_umask);
    if (br != 0 || ::listen(rd_.listen_fd, 16) != 0) { std::perror("luxium --host: socket"); return 1; }
    ::chmod(socket_path().c_str(), 0600);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "luxium --host: SDL_Init: %s\n", SDL_GetError());
        ::unlink(socket_path().c_str());
        return 1;
    }
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if (::pipe2(rd_.wake, O_CLOEXEC) != 0) return 1;
    std::signal(SIGTERM, on_term);
    std::signal(SIGINT, on_term);
    std::signal(SIGPIPE, SIG_IGN);
    rd_.th = std::thread([this] { rd_.loop(); });

    const int grace_s = std::getenv("LUXIUM_HOST_GRACE") ? std::atoi(std::getenv("LUXIUM_HOST_GRACE")) : 3;
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50)) {
            do handle_sdl(e, quit); while (SDL_PollEvent(&e));
        }
        if (g_term) quit = true;
        // protocol input
        std::deque<In> q;
        { std::lock_guard<std::mutex> lk(rd_.m); q.swap(rd_.q); }
        for (auto& in : q) {
            if (in.eof) {
                auto it = conns_.find(in.conn);
                if (it != conns_.end()) {
                    if (AppWin* w = it->second.win) {   // connection drop => window closes
                        w->conn = -1;
                        begin_close(w);
                    }
                    ::close(it->second.fd);
                    conns_.erase(it);
                } else ::close(in.fd);
            } else if (in.line.empty()) {
                conns_[in.conn] = Conn{in.fd, nullptr};
            } else on_line(in.conn, in.line);
        }
        for (auto& w : std::vector<AppWin*>([&] { std::vector<AppWin*> v; for (auto& x : wins_) v.push_back(x.get()); return v; }())) {
            if (!w->closing) { pump_shared(w); render(w); }
        }
        reap();
        if (!wins_.empty() || !conns_.empty()) idle_since_ = Clock::now();
        else if (Clock::now() - idle_since_ > std::chrono::seconds(grace_s)) quit = true;
    }

    // Shutdown: refuse new clients first, then close what is left.
    char b = 0;
    (void)!::write(rd_.wake[1], &b, 1);
    rd_.th.join();
    ::close(rd_.listen_fd);
    ::unlink(socket_path().c_str());
    for (auto& w : wins_) begin_close(w.get());
    for (int i = 0; i < 60 && !wins_.empty(); ++i) { reap(); SDL_PumpEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    if (cef_up_) cef::shutdown();
    SDL_Quit();
    return 0;   // lock_fd closes with the process
}

} // namespace

int run() { return Host().run(); }

} // namespace luxium::host
