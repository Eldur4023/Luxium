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
#include "host/menubar.hpp"

#include <page/page.hpp>
#include <shell/shell.hpp>
#include <render/images.hpp>
#include <render/text.hpp>

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
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
static std::string pid_path() { return runtime_dir() + "/host.pid"; }

namespace {

// ── liveness state, shared with the reader and watchdog threads ─────────────
std::atomic<bool> g_term{false};
std::atomic<int> g_windows{0};             // live (non-closing) windows, kept by the main loop
std::atomic<long long> g_beat_ms{0};       // main-loop heartbeat
std::atomic<long long> g_deadline_ms{0};   // hard exit time once shutdown starts (0 = none)
std::string g_build;                       // "<size>-<mtime>" of this executable, fixed at startup

long long mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}
// Arms the watchdog (first caller wins). Async-signal-safe: lock-free atomics and clock_gettime only.
void arm_deadline(long long ms) {
    long long z = 0;
    g_deadline_ms.compare_exchange_strong(z, mono_ms() + ms);
}
void on_term(int) { g_term = true; arm_deadline(3000); }

// stat of /proc/self/exe follows the link to the inode even after an installer replaced the file, so a host
// outliving its binary still reports the build it really runs. The client stats the same way.
std::string build_id() {
    struct stat st;
    if (::stat("/proc/self/exe", &st) != 0) return "unknown";
    return std::to_string((long long)st.st_size) + "-" + std::to_string((long long)st.st_mtime);
}

// SIGKILLs every descendant (CEF zygote, GPU, renderers, utility): they must not outlive the host.
void kill_descendants() {
    std::map<int, int> parent;   // pid -> ppid
    if (DIR* d = ::opendir("/proc")) {
        while (dirent* e = ::readdir(d)) {
            const int pid = std::atoi(e->d_name);
            if (pid <= 0) continue;
            char path[64], buf[1024];
            std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            buf[n] = 0;
            const char* rp = std::strrchr(buf, ')');   // comm may contain spaces/parens
            int ppid = 0;
            char st;
            if (rp && std::sscanf(rp + 1, " %c %d", &st, &ppid) == 2) parent[pid] = ppid;
        }
        ::closedir(d);
    }
    std::vector<int> todo = {(int)::getpid()};
    while (!todo.empty()) {
        const int p = todo.back();
        todo.pop_back();
        for (auto& [pid, pp] : parent)
            if (pp == p && pid != (int)::getpid()) { ::kill(pid, SIGKILL); todo.push_back(pid); }
    }
}

void cleanup_files() {
    ::unlink(socket_path().c_str());
    ::unlink(pid_path().c_str());
}

// Once shutdown is armed (signal, quit, idle exit) this thread guarantees the process ends: a wedged CEF or
// SDL must not keep the socket alive for hours.
void watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const long long d = g_deadline_ms.load();
        if (d && mono_ms() > d) {
            std::fprintf(stderr, "luxium --host: shutdown timed out, forcing exit\n");
            kill_descendants();
            cleanup_files();
            ::_exit(0);
        }
    }
}

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
    // hello / ping / quit are answered here, on the reader thread: they work while the main loop is busy
    // rendering or wedged in CEF, which is what makes them a liveness probe. True = handled.
    bool fast(const std::string& line, int fd, size_t nconn) {
        Obj o;
        if (!parse_obj(line, o)) return false;
        const std::string cmd = get(o, "cmd");
        if (cmd == "ping") { send_line(fd, "{\"ev\":\"pong\"}"); return true; }
        if (cmd == "hello") {
            send_line(fd, "{\"ev\":\"hello\",\"proto\":2,\"build\":" + jq(g_build) + ",\"pid\":" + std::to_string(::getpid()) +
                          ",\"windows\":" + std::to_string(g_windows.load()) + ",\"beat_ms\":" + std::to_string(mono_ms() - g_beat_ms.load()) + "}");
            return true;
        }
        if (cmd == "quit") {   // only an idle host leaves: the asker may be the only connection
            if (g_windows.load() > 0 || nconn > 1) { send_line(fd, "{\"ev\":\"busy\",\"windows\":" + std::to_string(g_windows.load()) + "}"); return true; }
            g_term = true;
            arm_deadline(3000);
            send_line(fd, "{\"ev\":\"bye\"}");
            SDL_Event e{};
            e.type = SDL_USEREVENT;
            SDL_PushEvent(&e);
            return true;
        }
        return false;
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
                            if (!line.empty() && !fast(line, cs[i].fd, cs.size())) push({cs[i].id, cs[i].fd, std::move(line), false});
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
    std::deque<std::string> msgs, errs;
    bool loaded = false;
    std::atomic<bool> closed{false};
};

// A window rendered by Luxium's own engine: a Page and the Shell that paints it into the host's SDL window.
// No Chromium involved; `wants_web` is set when the page turned out to need JavaScript (engine "auto").
struct Own {
    page::Page page;                       // declared first: destroyed after the shell that points at it
    std::unique_ptr<shell::Shell> shell;
    bool wants_web = false;
};

struct AppWin {
    std::unique_ptr<Own> own;      // own-engine window (no cw); else a Chromium window
    std::string open_url, profile_root;
    std::string last_title;
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
    unsigned long x11_display = 0, x11_window = 0;
    std::unique_ptr<MenuBar> menu;   // set_menu; the page view sits below its bar
    int view_bar = -1;               // bar height the page view was last sized for
    bool closing = false;
    Clock::time_point close_by;
    std::string app_id;
    bool loaded_sent = false;
};

struct Conn { int fd; AppWin* win = nullptr; };

class Host {
public:
    int run();

private:
    Reader rd_;
    std::map<int, Conn> conns_;
    std::vector<std::unique_ptr<AppWin>> wins_;
    bool cef_up_ = false;
    Clock::time_point idle_since_ = Clock::now();

    std::unique_ptr<render::Font> font_;   // menu bar text (main thread only)

    static int bar_h(const AppWin* w) { return w->menu && !w->menu->empty() ? MenuBar::kBarH : 0; }
    void fire_menu(AppWin* w, const MenuFire& f);
    void cmd_set_menu(AppWin* w, const Obj& o);
    AppWin* by_sdl(Uint32 id) {
        for (auto& w : wins_) if (w->win && SDL_GetWindowID(w->win) == id) return w.get();
        return nullptr;
    }
    void on_line(int conn, const std::string& line);
    void cmd_open(int conn, const Obj& o);
    void start_own(AppWin* w, bool auto_engine);
    bool start_web(AppWin* w);   // false: Chromium could not start (the caller falls back to the own engine)
    void handle_own(AppWin* w, const SDL_Event& e);
    AppWin* event_win(const SDL_Event& e);
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
    e.on_error = [sh](const std::string& m) { std::lock_guard<std::mutex> lk(sh->m); sh->errs.push_back(m); };
    e.on_loaded = [sh] { std::lock_guard<std::mutex> lk(sh->m); sh->loaded = true; };
    e.on_closed = [sh] { sh->closed = true; };
    return e;
}

void Host::resize_surface(AppWin* w) {
    int nw = 0, nh = 0;
    SDL_GetWindowSize(w->win, &nw, &nh);
    if (nw <= 0 || nh <= 0) return;
    const int bar = bar_h(w);
    if (nw == w->w && nh == w->h && (w->own || w->tex) && bar == w->view_bar) return;
    w->view_bar = bar;
    w->w = nw;
    w->h = nh;
    if (w->own) {   // the Shell keeps its own framebuffer; the bar is a top inset it leaves free
        w->own->shell->set_top_inset(bar);
        w->own->shell->invalidate();
    } else {
        w->buf.assign((size_t)nw * nh * 4, 0xFF);
        if (w->tex) SDL_DestroyTexture(w->tex);
        w->tex = SDL_CreateTexture(w->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, nw, nh);
        cef::set_view(w->cw, nw, std::max(1, nh - bar));
    }
    w->dirty = true;
    if (w->conn >= 0) ev(w, "{\"ev\":\"resized\",\"width\":" + std::to_string(nw) + ",\"height\":" + std::to_string(nh) + "}");
}

// Dark title bar: the _GTK_THEME_VARIANT hint (honoured by GNOME/Mutter and GTK-aware window managers; the
// host always runs on X11, XWayland included, so this is the only mechanism needed).
static void set_x11_dark(const AppWin* w) {
    if (!w->x11_display || !w->x11_window) return;
    Display* d = (Display*)w->x11_display;
    const Atom prop = XInternAtom(d, "_GTK_THEME_VARIANT", False);
    if (w->dark) XChangeProperty(d, (Window)w->x11_window, prop, XInternAtom(d, "UTF8_STRING", False), 8, PropModeReplace,
                                 (const unsigned char*)"dark", 4);
    else XDeleteProperty(d, (Window)w->x11_window, prop);
    XFlush(d);
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
        send_line(c.fd, "{\"ev\":\"error\",\"fatal\":true,\"message\":" + jq(SDL_GetError()) + "}");
        return;
    }
    if (resizable) SDL_SetWindowMinimumSize(w->win, std::max(1, geti(o, "min_width", 0)), std::max(1, geti(o, "min_height", 0)));
    const std::string icon = get(o, "icon");
    if (!icon.empty()) {
        // RGBA when the PNG has alpha (SDL turns the surface into an ARGB _NET_WM_ICON), else the RGB bitmap.
        SDL_Surface* sf = nullptr;
        auto rgba = render::load_image_rgba(icon);
        if (rgba && !rgba->rgba.empty())
            sf = SDL_CreateRGBSurfaceWithFormatFrom(rgba->rgba.data(), rgba->w, rgba->h, 32, rgba->w * 4, SDL_PIXELFORMAT_RGBA32);
        else if (const auto* bmp = render::load_image(icon))
            sf = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(bmp->rgb.data()), bmp->w, bmp->h, 24, bmp->w * 3, SDL_PIXELFORMAT_RGB24);
        if (sf) { SDL_SetWindowIcon(w->win, sf); SDL_FreeSurface(sf); }
    }
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(w->win, &wm) || wm.subsystem != SDL_SYSWM_X11) {
        send_line(c.fd, "{\"ev\":\"error\",\"fatal\":true,\"message\":\"no X11 display\"}");
        SDL_DestroyWindow(w->win);
        return;
    }
    w->x11_display = (unsigned long)wm.info.x11.display;
    w->x11_window = (unsigned long)wm.info.x11.window;
    set_x11_dark(w.get());
    {   // SDL 2.30 ignores SDL_VIDEO_X11_WMCLASS here: set the class hint ourselves so the shell matches the
        // window to its .desktop file (StartupWMClass). `wm_class` defaults to the app id.
        std::string cls = get(o, "wm_class", app_id);
        XClassHint ch{cls.data(), cls.data()};
        XSetClassHint((Display*)w->x11_display, (Window)w->x11_window, &ch);
        XFlush((Display*)w->x11_display);
    }

    w->open_url = get(o, "url", "about:blank");
    w->profile_root = get(o, "profile_root", std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.local/share/luxium/apps");
    // engine: "luxium" = own engine always, "web" = Chromium, "auto" (default) = own engine, and Chromium
    // only if the page turns out to run JavaScript (decided by the Page after it loads).
    const std::string engine = get(o, "engine", "auto");
    if (engine == "web") {
        if (!start_web(w.get())) {
            send_line(c.fd, "{\"ev\":\"error\",\"message\":\"Chromium could not start; using the own engine (no JavaScript)\"}");
            start_own(w.get(), false);
        }
    } else start_own(w.get(), engine != "luxium");
    resize_surface(w.get());
    if (w->cw) cef::set_focus(w->cw, true);
    c.win = w.get();
    send_line(c.fd, "{\"ev\":\"opened\",\"width\":" + std::to_string(w->w) + ",\"height\":" + std::to_string(w->h) + "}");
    wins_.push_back(std::move(w));
}

void Host::start_own(AppWin* w, bool auto_engine) {
    w->own = std::make_unique<Own>();
    Own* own = w->own.get();
    shell::ShellConfig cfg;
    cfg.app = true;
    cfg.devtools = w->dev;
    own->page.set_profile_dir(w->profile_root + "/" + w->app_id);   // storage.* per app
    own->shell = std::make_unique<shell::Shell>(own->page, cfg);
    own->shell->attach(w->win);
    own->shell->set_overlay([this, w](uint32_t* fb, int ww, int hh) {
        if (w->menu && bar_h(w)) w->menu->draw(fb, ww, hh);
    });
    if (auto_engine) own->shell->set_web_handler([own](const std::string&) { own->wants_web = true; });
    own->shell->navigate_to(w->open_url, true);
}

// Chromium window on the already-created SDL window (the first call starts Chromium; nothing before does).
bool Host::start_web(AppWin* w) {
    if (!cef_up_) {
        cef::set_profile_root(w->profile_root);
        cef_up_ = true;
    }
    cef::OpenParams p;
    p.url = w->open_url;
    p.profile_id = w->app_id;
    p.display = w->x11_display;
    p.parent = (unsigned long)w->x11_window;
    p.events = make_events(w);
    w->cw = cef::open_window(p);
    std::signal(SIGTERM, on_term);   // CEF's init replaced the handlers
    std::signal(SIGINT, on_term);
    if (!w->cw) return false;
    w->ren = SDL_CreateRenderer(w->win, -1, 0);   // only now: the own engine paints through the window surface
    return true;
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
    if (w->cw) cef::close_window(w->cw);
    else w->sh->closed = true;   // own-engine window: nothing asynchronous to wait for
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
            if (w->cw) cef::close_window(w->cw);   // no-op once closed; covers a browser still being created
            ++i;
            continue;
        }
        if (w->conn >= 0) {
            auto it = conns_.find(w->conn);
            if (it != conns_.end()) ::shutdown(it->second.fd, SHUT_RDWR);   // reader sees EOF -> main closes fd
        }
        if (w->cw && done) cef::destroy_window(w->cw);   // else leaked on purpose: CEF may still call back into it
        w->own.reset();   // the Shell frees its renderer before the window goes
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
    if (w->own) {   // own engine: read the page directly
        Own& o = *w->own;
        const std::string t = o.shell->page_title();
        if (t != w->last_title) { w->last_title = t; ev(w, "{\"ev\":\"title\",\"title\":" + jq(t) + "}"); }
        if (!w->loaded_sent && o.shell->has_page()) {
            w->loaded_sent = true;
            ev(w, "{\"ev\":\"url\",\"url\":" + jq(w->open_url) + "}");
            ev(w, "{\"ev\":\"loaded\"}");
        }
        return;
    }
    std::string title, addr;
    bool td = false, ad = false;
    std::deque<std::string> msgs, errs;
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lk(w->sh->m);
        td = w->sh->title_dirty; ad = w->sh->addr_dirty;
        title = w->sh->title; addr = w->sh->addr;
        w->sh->title_dirty = w->sh->addr_dirty = false;
        msgs.swap(w->sh->msgs);
        errs.swap(w->sh->errs);
        loaded = w->sh->loaded;
    }
    if (w->conn < 0) {   // DevTools window: its own title
        if (td && !title.empty()) SDL_SetWindowTitle(w->win, title.c_str());
    } else {
        // Page titles are reported, not applied: the window title is the app's (set_title).
        if (td) ev(w, "{\"ev\":\"title\",\"title\":" + jq(title) + "}");
        if (ad) ev(w, "{\"ev\":\"url\",\"url\":" + jq(addr) + "}");
        if (loaded && !w->loaded_sent) { w->loaded_sent = true; ev(w, "{\"ev\":\"loaded\"}"); }
        for (auto& m : errs) ev(w, "{\"ev\":\"error\",\"message\":" + jq(m) + "}");
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
    if (w->own) {
        if (w->menu && w->menu->take_dirty()) w->own->shell->invalidate();
        w->own->shell->frame();
        return;
    }
    if (w->closing || !w->tex) return;
    const int p = cef::paints(w->cw);
    if (w->menu && w->menu->take_dirty()) w->dirty = true;
    if (p == w->last_paints && !w->dirty) return;
    w->last_paints = p;
    w->dirty = false;
    int fw = 0, fh = 0;
    const int bar = bar_h(w);
    if (cef::copy_frame(w->cw, w->buf.data() + (size_t)bar * w->w * 4, w->w * 4, w->w, std::max(1, w->h - bar), &fw, &fh)) {
        if (bar) w->menu->draw((uint32_t*)w->buf.data(), w->w, w->h);
        SDL_UpdateTexture(w->tex, nullptr, w->buf.data(), w->w * 4);
    } else {   // nothing painted yet: no white flash on a dark app
        if (w->dark) SDL_SetRenderDrawColor(w->ren, 0x1b, 0x1b, 0x1f, 255);
        else SDL_SetRenderDrawColor(w->ren, 255, 255, 255, 255);
        SDL_RenderClear(w->ren);
        SDL_RenderPresent(w->ren);
        return;
    }
    SDL_RenderCopy(w->ren, w->tex, nullptr, nullptr);
    SDL_RenderPresent(w->ren);
}

AppWin* Host::event_win(const SDL_Event& e) {
    switch (e.type) {
    case SDL_WINDOWEVENT: return by_sdl(e.window.windowID);
    case SDL_MOUSEMOTION: return by_sdl(e.motion.windowID);
    case SDL_MOUSEWHEEL: return by_sdl(e.wheel.windowID);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: return by_sdl(e.button.windowID);
    case SDL_TEXTINPUT: return by_sdl(e.text.windowID);
    case SDL_TEXTEDITING: return by_sdl(e.edit.windowID);
    case SDL_KEYDOWN:
    case SDL_KEYUP: return by_sdl(e.key.windowID);
    default: return nullptr;
    }
}

// Input for an own-engine window: the menu bar sees it first, then the Shell.
void Host::handle_own(AppWin* w, const SDL_Event& e) {
    shell::Shell& sh = *w->own->shell;
    switch (e.type) {
    case SDL_WINDOWEVENT:
        switch (e.window.event) {
        case SDL_WINDOWEVENT_CLOSE: begin_close(w); return;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED: resize_surface(w); break;
        case SDL_WINDOWEVENT_LEAVE: if (w->menu) w->menu->on_leave(); break;
        }
        break;
    case SDL_MOUSEMOTION:
        if (w->menu && w->menu->on_motion(e.motion.x, e.motion.y, w->w)) return;
        break;
    case SDL_MOUSEWHEEL: {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        if (w->menu && (w->menu->open() || my < bar_h(w))) return;
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (w->menu) {
            MenuFire f;
            const bool used = w->menu->on_button(e.button.x, e.button.y, e.type == SDL_MOUSEBUTTONDOWN, w->w, f);
            fire_menu(w, f);
            if (used) return;
        }
        break;
    case SDL_TEXTINPUT:
        if (w->menu && w->menu->open()) return;
        break;
    case SDL_KEYDOWN:
        if (w->menu) {
            MenuFire f;
            const bool used = w->menu->on_key(e.key.keysym, e.key.repeat != 0, f);
            fire_menu(w, f);
            if (used) return;
        }
        if (w->user_fs && e.key.keysym.sym == SDLK_ESCAPE) { w->user_fs = false; set_fullscreen(w); return; }
        if (w->dev && (e.key.keysym.sym == SDLK_F5 || ((e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym == SDLK_r))) { sh.reload(); return; }
        break;
    default: break;
    }
    sh.handle_event(e);
}

void Host::handle_sdl(const SDL_Event& e, bool& quit) {
    using namespace cef_input;
    if (AppWin* ow = event_win(e); ow && ow->own && !ow->closing) { handle_own(ow, e); return; }
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
        case SDL_WINDOWEVENT_LEAVE:
            if (w->menu) w->menu->on_leave();
            cef::mouse_move(w->cw, 0, 0, true, 0);
            break;
        }
        break;
    }
    case SDL_MOUSEMOTION:
        if (AppWin* w = by_sdl(e.motion.windowID); w && !w->closing) {
            if (w->menu && w->menu->on_motion(e.motion.x, e.motion.y, w->w)) break;
            cef::mouse_move(w->cw, e.motion.x, e.motion.y - bar_h(w), false, cef_mods_flags(SDL_GetModState(), false));
        }
        break;
    case SDL_MOUSEWHEEL:
        if (AppWin* w = by_sdl(e.wheel.windowID); w && !w->closing) {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            if (w->menu && (w->menu->open() || my < bar_h(w))) break;
            my -= bar_h(w);
            float dy = e.wheel.preciseY, dx = e.wheel.preciseX;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dy = -dy; dx = -dx; }
            cef::mouse_wheel(w->cw, mx, my, (int)(dx * 100), (int)(dy * 100), cef_mods_flags(SDL_GetModState(), false));
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (AppWin* w = by_sdl(e.button.windowID); w && !w->closing) {
            const int btn = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
            if (w->menu) {
                MenuFire f;
                const bool used = w->menu->on_button(e.button.x, e.button.y, e.type == SDL_MOUSEBUTTONDOWN, w->w, f);
                fire_menu(w, f);
                if (used) break;
            }
            cef::mouse_button(w->cw, e.button.x, e.button.y - bar_h(w), btn, e.type == SDL_MOUSEBUTTONUP, e.button.clicks,
                              cef_mods_flags(SDL_GetModState(), false));
        }
        break;
    case SDL_TEXTINPUT:
        if (AppWin* w = by_sdl(e.text.windowID); w && !w->closing && !(w->menu && w->menu->open())) {
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
            // Menu accelerators fire before the page sees the key (and the open menu owns the keyboard).
            if (w->menu) {
                MenuFire f;
                const bool used = w->menu->on_key(k, e.key.repeat != 0, f);
                fire_menu(w, f);
                if (used) break;
            }
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

// Same contract as the WebKitGTK backend: a leaf activation reaches the client, which POSTs the item's action.
void Host::fire_menu(AppWin* w, const MenuFire& f) {
    if (!f.fired) return;
    ev(w, "{\"ev\":\"menu\",\"action\":" + jq(f.action) + (f.checkable ? std::string(",\"checked\":") + (f.checked ? "true" : "false") : "") + "}");
}

void Host::cmd_set_menu(AppWin* w, const Obj& o) {
    if (!font_) font_ = render::Font::load_dejavu("/usr/share/fonts/truetype/dejavu");
    if (!font_) { ev(w, "{\"ev\":\"error\",\"message\":\"menu: DejaVu fonts not found\"}"); return; }
    if (!w->menu) w->menu = std::make_unique<MenuBar>(font_.get());
    w->menu->set_dark(w->dark);
    if (!w->menu->set_spec(get(o, "spec", "[]"))) ev(w, "{\"ev\":\"error\",\"message\":\"menu: bad spec\"}");
    resize_surface(w);   // the page view shrinks or grows by the bar
    w->dirty = true;
}

void Host::on_line(int conn, const std::string& line) {
    auto ci = conns_.find(conn);
    if (ci == conns_.end()) return;
    Obj o;
    if (!parse_obj(line, o)) { send_line(ci->second.fd, "{\"ev\":\"error\",\"message\":\"bad json\"}"); return; }
    const std::string cmd = get(o, "cmd");
    if (cmd == "open") { cmd_open(conn, o); return; }
    if (cmd == "ping") { send_line(ci->second.fd, "{\"ev\":\"pong\"}"); return; }
    if (cmd == "_wedge" && std::getenv("LUXIUM_HOST_TEST")) for (;;) ::pause();   // tests only: simulates a render loop stuck in CEF
    AppWin* w = ci->second.win;
    if (!w || w->closing) return;
    if (w->own && (cmd == "eval" || cmd == "post_message" || cmd == "devtools")) {
        send_line(ci->second.fd, "{\"ev\":\"error\",\"message\":\"" + cmd + " is not available on own-engine windows\"}");
        return;
    }
    if (cmd == "navigate") { if (w->own) w->own->shell->navigate_to(get(o, "url"), true); else cef::navigate(w->cw, get(o, "url")); }
    else if (cmd == "reload") { if (w->own) w->own->shell->reload(); else cef::reload(w->cw); }
    else if (cmd == "set_title") SDL_SetWindowTitle(w->win, get(o, "title").c_str());
    else if (cmd == "resize") SDL_SetWindowSize(w->win, std::clamp(geti(o, "width", w->w), 100, 10000), std::clamp(geti(o, "height", w->h), 100, 10000));
    else if (cmd == "eval") cef::run_js(w->cw, get(o, "code"));
    else if (cmd == "post_message")   // delivered to the page as a 'luxium' CustomEvent whose detail is the string
        cef::run_js(w->cw, "window.dispatchEvent(new CustomEvent('luxium',{detail:" + jq(get(o, "data")) + "}))");
    else if (cmd == "devtools") open_devtools(w);
    else if (cmd == "set_menu") cmd_set_menu(w, o);
    else if (cmd == "set_dark") { w->dark = getb(o, "on", true); set_x11_dark(w); if (w->menu) w->menu->set_dark(w->dark); w->dirty = true; }
    else if (cmd == "show") { SDL_ShowWindow(w->win); SDL_RaiseWindow(w->win); }
    else if (cmd == "hide") SDL_HideWindow(w->win);
    else if (cmd == "toggle") {   // tray click: hide a visible window, show (and raise) a hidden one
        if (SDL_GetWindowFlags(w->win) & SDL_WINDOW_HIDDEN) { SDL_ShowWindow(w->win); SDL_RaiseWindow(w->win); }
        else SDL_HideWindow(w->win);
    }
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

    g_build = build_id();
    g_beat_ms = mono_ms();
    std::thread(watchdog).detach();
    if (FILE* pf = std::fopen(pid_path().c_str(), "w")) { std::fprintf(pf, "%d\n", (int)::getpid()); std::fclose(pf); }

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
        cleanup_files();
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
        g_beat_ms = mono_ms();
        SDL_Event e;
        bool any_own = false;
        for (auto& x : wins_) if (x->own) any_own = true;
        if (SDL_WaitEventTimeout(&e, any_own ? 16 : 50)) {
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
            if (w->own && w->own->wants_web && !w->closing) {   // engine "auto": the page runs JavaScript, hand the window to Chromium
                w->own.reset();
                w->w = 0;
                w->loaded_sent = false;
                if (start_web(w)) {
                    resize_surface(w);
                    cef::set_focus(w->cw, true);
                } else {   // Chromium is unavailable: keep showing the page without JavaScript, tell the app
                    ev(w, "{\"ev\":\"error\",\"message\":\"Chromium could not start; the page needs JavaScript\"}");
                    start_own(w, false);
                    resize_surface(w);
                }
            }
        }
        reap();
        { int n = 0; for (auto& x : wins_) if (!x->closing) ++n; g_windows = n; }
        if (!wins_.empty() || !conns_.empty()) idle_since_ = Clock::now();
        else if (Clock::now() - idle_since_ > std::chrono::seconds(grace_s)) quit = true;
    }

    // Shutdown: refuse new clients first, then close what is left. The watchdog ends the process if this wedges.
    arm_deadline(6000);
    char b = 0;
    (void)!::write(rd_.wake[1], &b, 1);
    rd_.th.join();
    ::close(rd_.listen_fd);
    cleanup_files();
    for (auto& w : wins_) begin_close(w.get());
    for (int i = 0; i < 60 && !wins_.empty(); ++i) { reap(); SDL_PumpEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    if (cef_up_) cef::shutdown();
    kill_descendants();
    SDL_Quit();
    return 0;   // lock_fd closes with the process
}

} // namespace

int run() { return Host().run(); }

} // namespace luxium::host
