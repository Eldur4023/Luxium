#include "shell/shell.hpp"

#include <net/fetch.hpp>
#include <cef/cef_backend.hpp>
#include <cef/sdl_input.hpp>
#include <style/dump.hpp>
#include <paint/raster.hpp>
#include <render/images.hpp>
#include <media/own_media.hpp>
#include "shell/media_glue.hpp"

#ifdef LUXIUM_CEF
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#endif

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

namespace luxium::shell {

// SIGTERM/SIGINT become a normal quit, so CEF shuts down and flushes the profile.
static std::atomic<bool> g_term{false};
static void on_term(int) { g_term = true; }

using luxium::render::ImageBitmap;
using namespace luxium::cef_input;


Shell::Shell(page::Page& page, ShellConfig cfg) : page_(page), cfg_(std::move(cfg)) {
    app_ = cfg_.app;
    // Clicks on <a href> come back here from the page, whatever the run mode.
    page_.on_navigate = [this](const std::string& href) { navigate_to(href, true); };
    page_.on_media_click = [](const dom::Node* n) { media::own::toggle(n); };
    media_api_ = std::make_unique<OwnMediaApi>(this);
    wire_page_hooks();
}

// What LuxScript reaches through Page::hooks(): the pieces only the window knows.
void Shell::wire_page_hooks() {
    page_.set_media(media_api_.get());
    auto& h = page_.hooks();
    h.clipboard_set = [](const std::string& s) { SDL_SetClipboardText(s.c_str()); };
    h.clipboard_get = [] {
        char* t = SDL_GetClipboardText();
        std::string s = t ? t : "";
        if (t) SDL_free(t);
        return s;
    };
    h.node_at = [this](float x, float y) {
        return const_cast<dom::Node*>(render::hit_test(layout_, x, y));
    };
    h.page_scroll_y  = [this] { return scroll_; };
    h.page_scroll_to = [this](float y) { scroll_ = std::max(0.f, std::min(y, std::max(0.f, content_h_ - page_h()))); };
    h.fullscreen = [this](dom::Node* n) { fullscreen_el_ = n; set_fullscreen(n != nullptr); page_.dispatch_window("fullscreenchange"); };
    h.fullscreen_element = [this] { return fullscreen_ ? fullscreen_el_ : nullptr; };
    h.reload = [this] { navigate_to(hist_[hist_idx_], false); };
    h.history_back_outside = [this] { if (hist_idx_ > 0) { --hist_idx_; navigate_to(hist_[hist_idx_], false); } };
    h.page_visible = [this] { return window_ && !(SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)); };
    // history.push/replace: the address bar follows; the browser's own history keeps one entry per page load.
    page_.on_location = [this](const std::string& url, bool) { if (hist_idx_ < hist_.size()) hist_[hist_idx_] = url; };
    wire_form_hooks();
}

// DOM key names for the keys scripts care about; printable keys use their text.
bool Shell::forward_key_to_page(const SDL_Event& e, bool down) {
    const SDL_Keycode k = e.key.keysym.sym;
    EventData d;
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: d.key = "Enter"; break;
    case SDLK_ESCAPE: d.key = "Escape"; break;
    case SDLK_TAB: d.key = "Tab"; break;
    case SDLK_BACKSPACE: d.key = "Backspace"; break;
    case SDLK_DELETE: d.key = "Delete"; break;
    case SDLK_LEFT: d.key = "ArrowLeft"; break;
    case SDLK_RIGHT: d.key = "ArrowRight"; break;
    case SDLK_UP: d.key = "ArrowUp"; break;
    case SDLK_DOWN: d.key = "ArrowDown"; break;
    case SDLK_HOME: d.key = "Home"; break;
    case SDLK_END: d.key = "End"; break;
    case SDLK_PAGEUP: d.key = "PageUp"; break;
    case SDLK_PAGEDOWN: d.key = "PageDown"; break;
    case SDLK_SPACE: d.key = " "; d.code = "Space"; break;
    default:
        if (k >= SDLK_F1 && k <= SDLK_F12) d.key = "F" + std::to_string(k - SDLK_F1 + 1);
        else if (k >= 32 && k < 127) d.key = std::string(1, static_cast<char>(
                     (e.key.keysym.mod & KMOD_SHIFT) && k >= 'a' && k <= 'z' ? k - 32 : k));
        else return false;
    }
    if (d.code.empty()) d.code = d.key.size() == 1 && std::isalpha(static_cast<unsigned char>(d.key[0]))
                                     ? "Key" + std::string(1, static_cast<char>(std::toupper(d.key[0]))) : d.key;
    const uint16_t m = e.key.keysym.mod;
    d.ctrl = m & KMOD_CTRL; d.shift = m & KMOD_SHIFT; d.alt = m & KMOD_ALT; d.meta = m & KMOD_GUI;
    d.repeat = e.key.repeat != 0;
    return page_.key_event(down, d);
}

Shell::~Shell() {
    media::own::Scope media_scope(this);
    media::own::reset();
    if (cursor_hand_) SDL_FreeCursor(cursor_hand_);
    if (cursor_text_) SDL_FreeCursor(cursor_text_);
    if (cursor_no_) SDL_FreeCursor(cursor_no_);
    if (text_input_on_) SDL_StopTextInput();
    if (texture_)  SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_ && !owns_window_ && !renderer_) SDL_DestroyWindowSurface(window_);   // the host may give the window to a renderer next
    if (window_ && owns_window_) SDL_DestroyWindow(window_);
    if (owns_window_) SDL_Quit();
}

void Shell::ensure_surface() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return;
    const int ch = console_visible_ ? std::max(140, h * 26 / 100) : 0;
    if (w == win_w_ && h == win_h_ && ch == console_h_) return;
    win_w_ = w;
    win_h_ = h;
    console_h_ = ch;
#ifdef LUXIUM_CEF
    if (engine_cef_)
        cef::set_view(win_w_, win_h_ - bottom_h() - url_h());
#endif
    need_paint_ = true;
    fb_.assign(static_cast<size_t>(win_w_) * win_h_, 0xFFFAFAF7u);
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = renderer_ ? SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, win_w_, win_h_)
                         : nullptr;
    relayout();
}

// 20px de tira de estado siempre; el panel de consola solo cuando esta
// visible (F12).  La pagina ocupa todo lo demas.
int Shell::bottom_h() const {
    if (fullscreen_) return 0;
    return status_h() + (console_visible_ ? console_h_ : 0);
}

int Shell::url_h() const { return fullscreen_ ? 0 : app_ ? top_inset_ : kUrlH; }

// The host window around an own-engine app page: SDL, the window and its events belong to the host.
void Shell::attach(SDL_Window* window) {
    window_ = window;
    owns_window_ = false;
    // No SDL_Renderer: it drags in libGL + Mesa (~40 MB of private pages per process) even in software mode.
    // frame() copies the framebuffer into the window surface instead (X11 shared memory).
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    cursor_hand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    font_ = render::Font::load_dejavu("/usr/share/fonts/truetype/dejavu");
    ensure_surface();
}

void Shell::set_top_inset(int px) {
    if (px == top_inset_) return;
    top_inset_ = px;
    win_w_ = 0;   // rebuild the surface for the new page area
    ensure_surface();
}

// The page took fullscreen (a video player, a web app): the window follows and
// the chrome around the page disappears; the view is the whole window.
void Shell::set_fullscreen(bool on) {
    if (on == fullscreen_) return;
    fullscreen_ = on;
    if (window_) SDL_SetWindowFullscreen(window_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    win_w_ = 0;      // force ensure_surface() to rebuild everything for the new size
    ensure_surface();
}

void Shell::toggle_console() {
    console_visible_ = !console_visible_;
    ensure_surface();   // reasigna fb, textura, vista CEF y re-layout
}

// ── raster primitives (all coordinates clipped to the framebuffer) ──────────

static inline void blend_px(uint32_t& dst, uint32_t argb) {
    const uint32_t a  = (argb >> 24) & 0xFF;
    const uint32_t sr = (argb >> 16) & 0xFF, sg = (argb >> 8) & 0xFF, sb = argb & 0xFF;
    const uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    const uint32_t ia = 255 - a;
    dst = 0xFF000000u |
          ((sr * a + dr * ia) / 255) << 16 |
          ((sg * a + dg * ia) / 255) << 8 |
          ((sb * a + db * ia) / 255);
}

// ── shell ────────────────────────────────────────────────────────────────────

// ── navigation (M5) ─────────────────────────────────────────────────────────

static std::string escape_html(const std::string& in) {
    std::string out;
    for (char ch : in) {
        switch (ch) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        default:  out += ch;
        }
    }
    return out;
}

// Path normalization for file targets: "a/./b", "a/x/../b" -> "a/b".
// A RELATIVE path stays relative -- prepending '/' made every local link
// point at the filesystem root, and the page quietly never changed.
static std::string normalize_path(const std::string& p) {
    const bool absolute = !p.empty() && p[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t nl = p.find('/', i);
        size_t e = (nl == std::string::npos) ? p.size() : nl;
        std::string seg = p.substr(i, e - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    std::string out;
    for (const auto& s : parts) { out += '/'; out += s; }
    if (absolute) return out.empty() ? "/" : out;
    return out.empty() ? "." : out.substr(1);
}

Shell::Resolved Shell::resolve(const std::string& href) const {
    Resolved r;
    r.target = href;
    const std::string& cur = page_.name();
    const bool cur_http = cur.rfind("http://", 0) == 0 || cur.rfind("https://", 0) == 0;

    if (!href.empty() && href[0] == '#') {
        r.kind = Target::Anchor;
        r.target = href.substr(1);
        return r;
    }
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) {
        r.kind = Target::Http;
        return r;
    }
    if (href.rfind("file://", 0) == 0) {
        r.kind = Target::File;
        r.target = normalize_path(href.substr(7));
        return r;
    }
    if (!href.empty() && href[0] == '/') {
        r.kind = Target::File;
        r.target = normalize_path(href);
        return r;
    }
    // relative: against the current page, whatever its scheme is
    size_t slash = cur.find_last_of('/');
    std::string base = (slash == std::string::npos) ? "" : cur.substr(0, slash + 1);
    if (cur_http) {
        r.kind = Target::Http;
        r.target = base + href;
        return r;
    }
    r.kind = Target::File;
    r.target = normalize_path(base + href);
    return r;
}

static std::string error_page(const std::string& target, const std::string& why) {
    return "<html><head><title>No se pudo cargar</title><style>body{font-family:Arial,Helvetica,sans-serif}</style></head><body>"
           "<h1>No se pudo cargar la pagina</h1>"
           "<p><b>" + escape_html(target) + "</b></p>"
           "<p>" + escape_html(why) + "</p>"
           "<p>Prueba con otra direccion o vuelve atras con Alt+Left.</p>"
           "</body></html>";
}

void Shell::navigate_to(const std::string& href, bool push_history) {
    Resolved r = resolve(href);
    if (r.kind != Target::Anchor) page_.dispatch_window("pagehide");   // last chance to report progress

    if (r.kind == Target::Anchor) {
        // scroll to the element with that id
        for (const auto& h : layout_.hits)
            if (h.node->is_element() && h.node->attr_or("id") == r.target) {
                scroll_ = std::max(0.f, h.y - 30.f);
                return;
            }
        page_.console(1, "ancla no encontrada: #" + r.target);
        return;
    }

    if (r.kind == Target::Http) {
        // Always asked as .luxp first (Accept): a Lux server answers its
        // compiled page and our engine shows it.  Anything else goes to
        // Chromium when it is built -- the fetch is then cut at the first
        // byte (luxp_only), so the page is not downloaded twice.
        if (fetch_) fetch_.reset();   // one fetch at a time: the newest wins
        fetch_ = net::Fetch::start(r.target, cef_available());
        if (push_history) {
            hist_.resize(hist_idx_ + 1);
            hist_.push_back(r.target);
            hist_idx_ = hist_.size() - 1;
        }
        return;   // poll_fetch() completes the load
    }

    // file
    engine_cef_ = false;   // el motor propio vuelve a pintar (papel + page)
    if (!page_.load_file(r.target)) {
        page_.load_string(error_page(r.target, "no existe el archivo (o no se puede leer)"),
                          "luxium-error:" + r.target);
    }
    // El juicio lux/js para paginas locales: si piden JavaScript y hay CEF,
    // se las pasa (file:// tambien es mundo Chromium).
    maybe_cef_handoff();
    after_load();
    if (push_history) {
        hist_.resize(hist_idx_ + 1);
        hist_.push_back(r.target);
        hist_idx_ = hist_.size() - 1;
    }
}

void Shell::tick_media() {
    if (media::own::tick() & 2) relayout();   // a video learned its size
}

void Shell::after_load() {
    { media::own::Scope media_scope(this); media::own::reset(); }   // players of the old document stop here
    hover_node_ = nullptr;   // the old DOM is gone
    scroll_ = 0;
    std::string t = "Luxium";
    if (!page_.page_title().empty()) t += " \xE2\x80\x94 " + page_.page_title();
    if (window_ && !app_) SDL_SetWindowTitle(window_, t.c_str());
    need_paint_ = true;
    relayout();
}

// A network image finished: lay the page out again so it shows up.
void Shell::poll_images() {
    const uint64_t g = render::image_generation();
    if (g == image_gen_) return;
    image_gen_ = g;
    page_.page_mutated();
}

void Shell::poll_fetch() {
    if (!fetch_ || !fetch_->done()) return;
    const bool ok = fetch_->ok();
    const bool luxp = fetch_->is_luxp();
    const bool not_luxp = fetch_->not_luxp();
    const std::string body = fetch_->body();
    const std::string err  = fetch_->error();
    // Relative URLs (http.get("/api/.."), <img>, <video>) resolve against where the page really came from:
    // a 302 to another origin (Lux Desktop apps do this in remote-server mode) moves the base with it.
    const std::string url  = fetch_->final_url();
    fetch_.reset();
    if (not_luxp) {   // the web's HTML/JS: Chromium's job
        open_in_cef(url);
        return;
    }
    if (ok && luxp) {
        page_.load_luxp(body, url);   // verified section by section; never handed to Chromium
        after_load();
        return;
    }
    if (!ok) {
        page_.load_string(error_page(url, err), "luxium-error:" + url);
        after_load();
        return;
    }
    page_.load_string(body, url);   // nuestro motor la juzga al cargar
    after_load();
#ifdef LUXIUM_CEF
    // La pagina pide JavaScript?  CEF la descarga y renderiza con todo.
    maybe_cef_handoff();
#endif
}

void Shell::toggle_view_source() {
    if (!viewing_source_) {
        viewing_source_origin_ = page_.name();
        const std::string& src = page_.source();
        page_.load_string(
            "<html><head><title>fuente: " + escape_html(viewing_source_origin_) +
            "</title></head><body><h2>fuente de " + escape_html(viewing_source_origin_) +
            "</h2><pre>" + escape_html(src) + "</pre></body></html>",
            "view-source:" + viewing_source_origin_);
        after_load();
        viewing_source_ = true;
    } else {
        viewing_source_ = false;
        navigate_to(viewing_source_origin_, false);
    }
}

// ── home + address bar ──────────────────────────────────────────────────────

constexpr const char* kHomeHTML = R"html(<!doctype html>
<html>
<head><title>Luxium</title>
<style>
body { text-align: center; font-family: Arial, Helvetica, sans-serif; }
h1 { font-size: 44px; margin-top: 40px; }
.sub { color: #8A919B; font-size: 15px; }
h2 { font-size: 18px; }
.a { font-size: 16px; }
.caja {
    background-color: #EFF3F9; margin: 24px 60px; padding: 16px 24px;
    text-align: left;
}
code { font-size: 13px; }
</style>
</head>
<body>
    <h1>Luxium</h1>
    <p class="sub">Un navegador que renderiza HTML + CSS + <b>Lux Script</b>, en lugar de JavaScript.</p>

    <div class="caja">
        <p><b>Escribe una URI en la barra de arriba</b> y pulsa Enter:</p>
        <p><code>http://example.com</code> &middot; una pagina web &middot;
           <code>/ruta/al/archivo.html</code> &middot; un archivo local &middot;
           <code>carpeta/pagina.html</code> &middot; relativo al directorio actual</p>
    </div>

    <h2>Ejemplos de este repositorio (si ejecutas desde la raiz)</h2>
    <p class="a">
        <a href="examples/css.html">prueba de CSS</a> &middot;
        <a href="examples/todo.html">tareas (DOM + timers)</a> &middot;
        <a href="examples/counter.html">contador</a> &middot;
        <a href="examples/script-demo.html">demo LuxScript</a>
    </p>

    <p class="sub">Ctrl+U ve el fuente &middot; Alt+Left vuelve &middot; Ctrl+R recarga</p>
</body>
</html>)html";

void Shell::load_home() {
    page_.load_string(kHomeHTML, "luxium:inicio");
    after_load();
}

void Shell::navigate_address(std::string addr, bool push_history) {
    // trim
    while (!addr.empty() && (addr.front() == ' ' || addr.front() == '\t')) addr.erase(addr.begin());
    while (!addr.empty() && (addr.back() == ' ' || addr.back() == '\t')) addr.pop_back();
    if (addr.empty()) return;

    if (addr == "luxium:inicio" || addr == "luxium:home") {
        if (push_history) {
            hist_.resize(hist_idx_ + 1);
            hist_.push_back("luxium:inicio");
            hist_idx_ = hist_.size() - 1;
        }
        load_home();
        return;
    }
    // Files that exist win (relative to the CWD and to the current page):
    const std::string& cur = page_.name();
    size_t slash = cur.find_last_of('/');
    std::string base = (slash == std::string::npos || cur.rfind("luxium:", 0) == 0)
                           ? std::string()
                           : cur.substr(0, slash + 1);
    if (std::filesystem::exists(base + addr)) {
        navigate_to(base + addr, push_history);
        return;
    }
    if (std::filesystem::exists(addr)) {
        navigate_to(addr, push_history);
        return;
    }
    // Domain-looking input gets the https prefix (localhost speaks http).
    const bool domainish = addr.find(' ') == std::string::npos &&
                           (addr.rfind("localhost", 0) == 0 ||
                            addr.rfind("127.0.0.1", 0) == 0 ||
                            addr.find('.') != std::string::npos);
    if (domainish) {
        const std::string scheme =
            (addr.rfind("localhost", 0) == 0 || addr.rfind("127.0.0.1", 0) == 0)
                ? "http://" : "https://";
        navigate_to(scheme + addr, push_history);
        return;
    }
    navigate_to(addr, push_history);   // will show the error page: honest
}

// ── selection + clipboard ───────────────────────────────────────────────────

void Shell::selection_clear() {
    sel_start_ = sel_end_ = -1;
    sel_dragging_ = false;
}

int Shell::word_index_at(float doc_x, float doc_y) const {
    // The word whose line contains doc_y and whose span contains doc_x;
    // else the nearest word on that line; else -1.
    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < static_cast<int>(layout_.words.size()); ++i) {
        const auto& w = layout_.words[i];
        if (doc_y < w.y - 2 || doc_y > w.y + w.h + 2) continue;
        float d = 0;
        if (doc_x < w.x)            d = w.x - doc_x;
        else if (doc_x > w.x + w.w) d = doc_x - (w.x + w.w);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

void Shell::selection_word_at(float doc_x, float doc_y) {
    int i = word_index_at(doc_x, doc_y);
    if (i < 0) { selection_clear(); return; }
    sel_start_ = sel_end_ = i;
}

void Shell::selection_extend(float doc_x, float doc_y) {
    int i = word_index_at(doc_x, doc_y);
    if (i < 0) return;
    sel_end_ = i;
}

std::string Shell::selection_text() const {
    if (sel_start_ < 0 || sel_end_ < 0) return "";
    const int a = std::min(sel_start_, sel_end_);
    const int b = std::max(sel_start_, sel_end_);
    std::string out;
    float last_y = -1e9f;
    for (int i = a; i <= b && i < static_cast<int>(layout_.words.size()); ++i) {
        const auto& w = layout_.words[static_cast<size_t>(i)];
        if (last_y > -1e8f && std::fabs(w.y - last_y) > 2.f) out += '\n';
        else if (!out.empty()) out += ' ';
        out += w.text;
        last_y = w.y;
    }
    return out;
}

void Shell::copy_selection() const {
    const std::string t = selection_text();
    if (!t.empty()) {
        SDL_SetClipboardText(t.c_str());
        // feedback in the status area would be nicer; the console is honest
    }
}

// Chromium's cursor type for the page area -> an SDL system cursor.
void Shell::update_cef_cursor() {
#ifdef LUXIUM_CEF
    constexpr int kNone = 36;
    const int t = cef::cursor();
    if (t == cef_cursor_) return;
    cef_cursor_ = t;
    if (std::getenv("LUXIUM_DEBUG_CEF")) std::fprintf(stderr, "[cursor] type %d\n", t);
    if (t == kNone) { SDL_ShowCursor(SDL_DISABLE); return; }
    SDL_ShowCursor(SDL_ENABLE);
    const SDL_SystemCursor id = cef_input::cursor_for(t);
    static SDL_Cursor* cache[SDL_NUM_SYSTEM_CURSORS] = {};
    if (!cache[id]) cache[id] = SDL_CreateSystemCursor(id);
    if (cache[id]) SDL_SetCursor(cache[id]);
#endif
}

// El juicio de la casa: si la pagina que acaba de cargar pide JavaScript y
// hay CEF, se la pasa a Chromium (que la descarga y renderiza con todo).
// Sin display X11 (modo --shot, dummy) no hay handoff: el motor propio la
// deja en blanco y lo dice por consola.
bool Shell::cef_available() const {
#ifdef LUXIUM_CEF
    return cef::built() && cef_display_;
#else
    return false;
#endif
}

void Shell::open_in_cef(const std::string& url) {
#ifdef LUXIUM_CEF
    cef::init(cef_display_, cef_parent_);   // first JavaScript page: Chromium starts now, not before
    std::signal(SIGTERM, on_term);          // CEF's init replaced the handlers
    std::signal(SIGINT, on_term);
    cef::attach(cef_display_, cef_parent_, url);
    cef::set_view(win_w_, win_h_ - bottom_h() - url_h());
    cef::set_focus(true);
    engine_cef_ = true;
    scroll_ = 0;
#else
    (void)url;
#endif
}

void Shell::maybe_cef_handoff() {
    if (!page_.used_javascript()) return;
    if (app_) {   // the host decides what to do with a JavaScript page (open it in Chromium)
        if (web_handler_) web_handler_(page_.name());
        return;
    }
    if (!cef_available()) return;
    std::string target = page_.name();
    if (target.rfind("http", 0) != 0) {
        std::error_code ec;
        auto abs = std::filesystem::absolute(target, ec);
        target = "file://" + (ec ? std::string("/") + target : abs.string());
    }
    open_in_cef(target);
}

// ── the URL bar itself ──────────────────────────────────────────────────────

// ── url-bar editing ─────────────────────────────────────────────────────────
// Byte boundaries (utf-8): the caret lives in BYTES but only ever moves
// between characters, so a backspace can never split a sequence.
static int u8_next(const std::string& s, int i) {
    if (i >= static_cast<int>(s.size())) return static_cast<int>(s.size());
    ++i;
    while (i < static_cast<int>(s.size()) && (s[i] & 0xC0) == 0x80) ++i;
    return i;
}
static int u8_prev(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && (s[i] & 0xC0) == 0x80) --i;
    return i;
}
// Ctrl+Left/Right: skip spaces, then the word (spaces = separators).
static int url_word_left(const std::string& s, int i) {
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) i = u8_prev(s, i);
    while (i > 0 && !(s[i - 1] == ' ' || s[i - 1] == '\t')) i = u8_prev(s, i);
    return i;
}
static int url_word_right(const std::string& s, int i) {
    const int n = static_cast<int>(s.size());
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i = u8_next(s, i);
    while (i < n && !(s[i] == ' ' || s[i] == '\t')) i = u8_next(s, i);
    return i;
}

// Window x -> byte position, by shaping the field text and testing each
// glyph's half (left half = caret before it, right half = after it).
int Shell::url_caret_at(int mx) const {
    if (url_edit_.empty()) return 0;
    font_->set_size(14);
    const float local = static_cast<float>(mx) - kUrlTextX + url_scroll_;
    if (local <= 0) return 0;
    const auto run = font_->shape(url_edit_, false, false);
    const int n = static_cast<int>(run.positions.size());
    const int len = static_cast<int>(url_edit_.size());
    for (int gi = 0; gi < n; ++gi) {
        const auto& p = run.positions[gi];
        const float x1 = (gi + 1 < n) ? run.positions[gi + 1].x : run.width;
        if (local < p.x) break;
        if (local < p.x + (x1 - p.x) * 0.5f) return static_cast<int>(p.cluster);
        int end = len;   // right half: the caret goes AFTER this cluster
        for (int g2 = gi + 1; g2 < n; ++g2)
            if (static_cast<int>(run.positions[g2].cluster) > static_cast<int>(p.cluster)) {
                end = static_cast<int>(run.positions[g2].cluster);
                break;
            }
        if (local < x1) return end;
    }
    return len;
}

bool Shell::url_delete_selection() {
    const int a = std::min(url_anchor_, url_caret_);
    const int b = std::max(url_anchor_, url_caret_);
    if (a >= b) return false;
    url_edit_.erase(a, b - a);
    url_caret_ = url_anchor_ = a;
    return true;
}

void Shell::url_insert(const std::string& utf8) {
    std::string t;
    t.reserve(utf8.size());
    for (char c : utf8)
        if (c != '\r' && c != '\n' && c != '\t') t += c;
    if (t.size() > 1) {   // a paste: drop the stray spaces around it
        const size_t b = t.find_first_not_of(' ');
        const size_t e = t.find_last_not_of(' ');
        t = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
    }
    if (t.empty()) return;
    url_delete_selection();
    url_caret_ = std::clamp(url_caret_, 0, static_cast<int>(url_edit_.size()));
    url_edit_.insert(url_edit_.begin() + url_caret_, t.begin(), t.end());
    url_caret_ += static_cast<int>(t.size());
    url_anchor_ = url_caret_;
}

void Shell::url_copy() const {
    const int a = std::min(url_anchor_, url_caret_);
    const int b = std::max(url_anchor_, url_caret_);
    if (a < b) SDL_SetClipboardText(url_edit_.substr(a, b - a).c_str());
}

void Shell::url_paste() {
    char* t = SDL_GetClipboardText();
    if (t) {
        if (*t) url_insert(t);
        SDL_free(t);
    }
}

void Shell::draw_url_bar() {
    constexpr int H = kUrlH;   // drawn only outside fullscreen
    // field background + focus ring
    const uint32_t bg      = url_focus_ ? 0xFFFFFFFFu : 0xFFF1F2F4;
    const uint32_t ring    = url_focus_ ? 0xFF2F6FEDu : 0xFFD9DCDF;
    for (int y = 0; y < H; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFFE8EAED;
    }
    const int fx = 8, fy = 6, fw = win_w_ - 16, fh = H - 12;
    for (int y = fy; y < fy + fh; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = fx; x < fx + fw; ++x) row[x] = bg;
    }
    for (int x = fx; x < fx + fw; ++x) {
        fb_[static_cast<size_t>(fy) * win_w_ + x] = ring;
        fb_[static_cast<size_t>(fy + fh - 1) * win_w_ + x] = ring;
    }
    for (int y = fy; y < fy + fh; ++y) {
        fb_[static_cast<size_t>(y) * win_w_ + fx] = ring;
        fb_[static_cast<size_t>(y) * win_w_ + fx + fw - 1] = ring;
    }

    // label -- only when it adds information (a luxium: address already
    // says it)
    const std::string shown_addr = url_focus_ ? url_edit_ : page_.location();
    font_->set_size(12.5f);
    if (shown_addr.rfind("luxium:", 0) != 0) {
        float lx = fx + 10;
        for (const auto& w : render::shape_words(*font_, "luxium:", true, false)) {
            render::draw_word(fb_, win_w_, win_h_, *font_, w, lx,
                              fy + (fh - font_->line_height()) / 2.f, 0xFF8A919B);
            lx += w.width + 12.5f * 0.3f;
        }
    }

    // the address: editing text, or the current page
    font_->set_size(14);
    const float tx = kUrlTextX;
    const float ty = fy + (fh - font_->line_height()) / 2.f;
    const float avail = fx + fw - tx - 12;

    if (url_focus_) {
        // ── edición de verdad: caret en cualquier sitio, selección con
        //    Arrastrar con el ratón y scroll horizontal cuando no cabe ──
        const int len = static_cast<int>(url_edit_.size());
        url_caret_  = std::clamp(url_caret_, 0, len);
        url_anchor_ = std::clamp(url_anchor_, 0, len);

        auto width_upto = [&](int byte) -> float {
            if (byte <= 0) return 0.f;
            return font_->shape(std::string_view(url_edit_).substr(0, byte),
                                false, false).width;
        };
        const float full_w = url_edit_.empty() ? 0.f
                            : font_->shape(url_edit_, false, false).width;
        const float caret_local = width_upto(url_caret_);
        // el caret siempre visible dentro del campo
        if (caret_local - url_scroll_ > avail - 2)
            url_scroll_ = caret_local - avail + 2;
        if (caret_local < url_scroll_) url_scroll_ = caret_local;
        url_scroll_ = std::clamp(url_scroll_, 0.f,
                                 std::max(0.f, full_w - avail + 2));

        // subcadena visible: glifos enteros dentro de [scroll, scroll+avail]
        std::string sub;
        float sub_x = tx;
        if (!url_edit_.empty()) {
            const auto run = font_->shape(url_edit_, false, false);
            const int n = static_cast<int>(run.positions.size());
            int b0 = 0, b1 = len;
            float px0 = 0.f;
            for (int gi = 0; gi < n; ++gi) {
                const auto& p = run.positions[gi];
                if (p.x >= url_scroll_) { b0 = static_cast<int>(p.cluster); px0 = p.x; break; }
            }
            for (int gi = 0; gi < n; ++gi) {
                const auto& p = run.positions[gi];
                const float x1 = (gi + 1 < n) ? run.positions[gi + 1].x : run.width;
                if (x1 > url_scroll_ + avail) { b1 = static_cast<int>(p.cluster); break; }
            }
            if (b1 < b0) b1 = b0;
            sub  = url_edit_.substr(b0, b1 - b0);
            sub_x = tx + px0 - url_scroll_;
        }

        // selección (bajo el texto)
        if (url_anchor_ != url_caret_) {
            const int sa = std::min(url_anchor_, url_caret_);
            const int sb = std::max(url_anchor_, url_caret_);
            int px_a = static_cast<int>(tx + width_upto(sa) - url_scroll_);
            int px_b = static_cast<int>(tx + width_upto(sb) - url_scroll_);
            px_a = std::max(px_a, static_cast<int>(tx));
            px_b = std::min(px_b, static_cast<int>(tx + avail));
            for (int yy = fy + 3; yy < fy + fh - 3; ++yy)
                for (int xx = px_a; xx < px_b; ++xx)
                    if (xx >= 0 && xx < win_w_)
                        blend_px(fb_[static_cast<size_t>(yy) * win_w_ + xx],
                                 0x662F6FEDu);
        }

        if (!sub.empty()) {
            render::Word w;
            w.text   = sub;
            w.shaped = font_->shape(sub, false, false);
            w.width  = w.shaped.width;
            w.size   = 14;
            w.ascent = font_->ascent();
            render::draw_word(fb_, win_w_, win_h_, *font_, w, sub_x, ty,
                              0xFF232936);
        }

        // caret (parpadea)
        const float cx = tx + caret_local - url_scroll_;
        if (cx >= tx - 1 && cx <= tx + avail && (frames_ / 30) % 2 == 0)
            for (int yy = fy + 5; yy < fy + fh - 5; ++yy)
                for (int xx = static_cast<int>(cx); xx < static_cast<int>(cx) + 2; ++xx)
                    if (xx >= 0 && xx < win_w_)
                        fb_[static_cast<size_t>(yy) * win_w_ + xx] = 0xFF2F6FED;
    } else {
        std::string visible = engine_cef_ ? cef::url() : page_.location();
        if (visible.empty()) visible = "escribe una uri...";
        // keep the tail that fits (utf-8 sequences dropped whole from the front)
        while (visible.size() > 4 &&
               font_->shape(visible, false, false).width > avail) {
            size_t n = 1;
            while (n < visible.size() && (visible[n] & 0xC0) == 0x80) ++n;
            visible.erase(0, n);
        }
        render::Word shown_word;
        shown_word.text   = visible;
        shown_word.shaped = font_->shape(visible, false, false);
        shown_word.width  = shown_word.shaped.width;
        shown_word.size   = 14;
        shown_word.ascent = font_->ascent();
        render::draw_word(fb_, win_w_, win_h_, *font_, shown_word, tx, ty,
                          0xFF5A6270);
    }
}

#ifdef LUXIUM_CEF
// type: 0 = raw key down, 1 = key up.  native = hardware keycode X11.
static void forward_cef_key(const SDL_Keysym& k, int type, unsigned long display,
                            bool repeat) {
    const uint32_t vk = cef_vk(k.sym);
    if (vk == 0) return;                 // tecla desconocida: no estorbar
    uint32_t native = 0;
    const uint32_t xs = cef_x_keysym(k.sym);
    if (xs && display)
        native = XKeysymToKeycode((Display*)display, (KeySym)xs);
    cef::key(type, cef_mods_flags(k.mod, repeat), vk, native, nullptr);
}
#endif // LUXIUM_CEF


int Shell::run() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window_ = SDL_CreateWindow(cfg_.title.c_str(), SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, cfg_.width, cfg_.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    renderer_ = SDL_CreateRenderer(window_, -1, 0);
    cursor_hand_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);

    font_ = render::Font::load_dejavu("/usr/share/fonts/truetype/dejavu");
    if (!font_) {
        std::fprintf(stderr, "cannot load the UI fonts:\n  %s\n  %s\n  %s\n",
                     cfg_.font_regular.c_str(), cfg_.font_bold.c_str(),
                     cfg_.font_mono.c_str());
        return 1;
    }

    // Do not wait for a SIZE_CHANGED event that may never come: measure the
    // window as it is right now.  (A window whose framebuffer is never
    // allocated is a black window, and no SDL error says why.)
    ensure_surface();

    if (hist_.empty()) {
        hist_.push_back(page_.name());
        hist_idx_ = 0;
    }

#ifdef LUXIUM_CEF
    // Chromium se inicializa con la ventana ya viva: su hijo X11 cuelga de
    // la nuestra.
    {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (SDL_GetWindowWMInfo(window_, &wm)) {
            cef_display_ = (unsigned long)wm.info.x11.display;
            cef_parent_  = (unsigned long)wm.info.x11.window;
        }
    }
    maybe_cef_handoff();   // la pagina inicial tambien se juzga (lux vs js)
#endif

    bool quit = false;
    while (!quit) {
        if (g_term) quit = true;
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (handle_event(e)) quit = true;
        frame();
        SDL_Delay(8);   // ~120fps cap; the CPU is idle at this size anyway
    }
#ifdef LUXIUM_CEF
    cef::shutdown();
#endif
    return 0;
}

// One window event.  Returns true when the user asked to quit.
bool Shell::handle_event(const SDL_Event& e) {
    bool quit = false;
    need_paint_ = true;
    if (!engine_cef_ && page_input(e)) return false;   // a form control / text input took it
            switch (e.type) {
            case SDL_QUIT:
                page_.dispatch_window("pagehide");
                quit = true;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED || e.window.event == SDL_WINDOWEVENT_HIDDEN ||
                    e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_SHOWN)
                    page_.dispatch_window("visibilitychange");
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_RESIZED)
                    ensure_surface();
#ifdef LUXIUM_CEF
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    cef::set_focus(engine_cef_ && !url_focus_);
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    cef::set_focus(false);
#endif
                break;
            case SDL_MOUSEMOTION:
                mouse_x_ = e.motion.x;
                mouse_y_ = e.motion.y;
                // arrastrar dentro de la barra: la selección crece
                if (url_drag_ && url_focus_ &&
                    (e.motion.state & SDL_BUTTON_LMASK))
                    url_caret_ = url_caret_at(e.motion.x);
                if (!engine_cef_) update_hover();
#ifdef LUXIUM_CEF
                if (engine_cef_) {
                    const bool inside =
                        e.motion.x >= 0 && e.motion.x < win_w_ &&
                        e.motion.y >= url_h() &&
                        e.motion.y < win_h_ - bottom_h();
                    cef::mouse_move(e.motion.x, e.motion.y - url_h(), !inside,
                                    cef_mods_flags(SDL_GetModState(), false));
                }
#endif
                break;
            case SDL_TEXTINPUT:
                if (url_focus_) url_insert(e.text.text);
#ifdef LUXIUM_CEF
                else if (engine_cef_) {
                    // plain ASCII is a CHAR key event; anything else (accents, CJK, emoji)
                    // goes through the input-method path so it is not cut to 16 bits
                    if (static_cast<unsigned char>(e.text.text[0]) < 0x80)
                        cef::key(2, cef_mods_flags(SDL_GetModState(), false), 0, 0, e.text.text);
                    else
                        cef::ime_commit(e.text.text);
                }
#endif
                break;
            case SDL_TEXTEDITING:   // IME preedit; an empty string cancels it
#ifdef LUXIUM_CEF
                if (engine_cef_ && !url_focus_) cef::ime_compose(e.edit.text, e.edit.start);
#endif
                break;
            case SDL_MOUSEWHEEL: {
#ifdef LUXIUM_CEF
                if (engine_cef_ && !url_focus_ && mouse_y_ >= url_h() &&
                    mouse_y_ < win_h_ - bottom_h()) {
                    float dy = e.wheel.preciseY;
                    float dx = e.wheel.preciseX;
                    if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                        dy = -dy;
                        dx = -dx;
                    }
                    cef::mouse_wheel(mouse_x_, mouse_y_ - url_h(),
                                     static_cast<int>(dx * 100),
                                     static_cast<int>(dy * 100),
                                     cef_mods_flags(SDL_GetModState(), false));
                    break;
                }
#endif
                const float lines = static_cast<float>(-e.wheel.y) * 3;
                if (!engine_cef_ && !popup_.node &&
                    wheel_scroll(static_cast<float>(e.wheel.x) * 54.f * ((SDL_GetModState() & KMOD_SHIFT) && e.wheel.x == 0 ? 0 : 1) +
                                 ((SDL_GetModState() & KMOD_SHIFT) && e.wheel.x == 0 ? lines * 18.f : 0.f),
                                 (SDL_GetModState() & KMOD_SHIFT) && e.wheel.x == 0 ? 0.f : lines * 18.f)) break;
                if (console_visible_ && mouse_y_ >= win_h_ - bottom_h()) {
                    // console pane scrolls its own history
                    console_scroll_ = std::clamp(console_scroll_ + lines * 18.f,
                                                 0.f, 1e9f);
                } else {
                    scroll_ = std::clamp(scroll_ + lines * 18.f, 0.f,
                                         std::max(0.f, content_h_ - page_h()));
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
#ifdef LUXIUM_CEF
                if (engine_cef_ && e.button.y >= url_h() &&
                    e.button.y < win_h_ - bottom_h()) {
                    // click dentro del area de pagina: el chrome no la toca
                    url_focus_ = false;
                    url_drag_  = false;
                    const int btn = e.button.button == SDL_BUTTON_LEFT ? 0
                                  : e.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
                    cef::mouse_button(e.button.x, e.button.y - url_h(), btn, false,
                                      e.button.clicks,
                                      cef_mods_flags(SDL_GetModState(), false));
                    break;
                }
#endif
                if (e.button.button == SDL_BUTTON_LEFT)
                    handle_click(e.button.x, e.button.y, e.button.clicks);
                else if (e.button.button == 3 && !engine_cef_ && page_.history_go(-1)) {}   // X1 = back inside the page
                else if (e.button.button == 4 && !engine_cef_ && page_.history_go(1)) {}    // X2 = forward inside the page
                else if (e.button.button == 3) navigate_to(hist_[hist_idx_], false);      // X1 = back
                else if (e.button.button == 4 && hist_idx_ + 1 < hist_.size()) {          // X2 = forward
                    ++hist_idx_;
                    navigate_to(hist_[hist_idx_], false);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) url_drag_ = false;
#ifdef LUXIUM_CEF
                if (engine_cef_ && e.button.y >= url_h() &&
                    e.button.y < win_h_ - bottom_h()) {
                    const int btn = e.button.button == SDL_BUTTON_LEFT ? 0
                                  : e.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
                    cef::mouse_button(e.button.x, e.button.y - url_h(), btn, true,
                                      e.button.clicks,
                                      cef_mods_flags(SDL_GetModState(), false));
                }
#endif
                break;
            case SDL_KEYDOWN:
#ifdef LUXIUM_CEF
                if (fullscreen_ && e.key.keysym.sym == SDLK_ESCAPE) {
                    cef::exit_fullscreen();
                    break;
                }
#endif
                if (e.key.keysym.sym == SDLK_F12 && (!app_ || cfg_.devtools)) {   // consola: mostrar/ocultar
                    toggle_console();
                    break;
                }
                if (url_focus_) {
                    const uint16_t mods = e.key.keysym.mod;
                    const bool ctrl  = (mods & KMOD_CTRL) != 0;
                    const bool shift = (mods & KMOD_SHIFT) != 0;
                    const int  len   = static_cast<int>(url_edit_.size());
                    switch (e.key.keysym.sym) {
                    case SDLK_RETURN: case SDLK_KP_ENTER: {
                        const std::string addr = url_edit_;
                        url_focus_ = false;
                        navigate_address(addr, true);
                        break;
                    }
                    case SDLK_ESCAPE:
                        url_focus_ = false;
                        break;
                    case SDLK_BACKSPACE:
                        if (!url_delete_selection()) {
                            const int p = u8_prev(url_edit_, url_caret_);
                            if (p < url_caret_) {
                                url_edit_.erase(p, url_caret_ - p);
                                url_caret_ = url_anchor_ = p;
                            }
                        }
                        break;
                    case SDLK_DELETE:
                        if (!url_delete_selection()) {
                            const int p = u8_next(url_edit_, url_caret_);
                            if (p > url_caret_) {
                                url_edit_.erase(url_caret_, p - url_caret_);
                                url_anchor_ = url_caret_;
                            }
                        }
                        break;
                    case SDLK_LEFT: case SDLK_RIGHT: {
                        const bool left = e.key.keysym.sym == SDLK_LEFT;
                        int target;
                        if (ctrl)
                            target = left ? url_word_left(url_edit_, url_caret_)
                                          : url_word_right(url_edit_, url_caret_);
                        else
                            target = left ? u8_prev(url_edit_, url_caret_)
                                          : u8_next(url_edit_, url_caret_);
                        // sin shift: una selección existente colapsa a su borde
                        if (!shift && url_anchor_ != url_caret_)
                            target = left ? std::min(url_anchor_, url_caret_)
                                          : std::max(url_anchor_, url_caret_);
                        url_caret_ = std::clamp(target, 0, len);
                        if (!shift) url_anchor_ = url_caret_;
                        break;
                    }
                    case SDLK_HOME:
                        url_caret_ = 0;
                        if (!shift) url_anchor_ = url_caret_;
                        break;
                    case SDLK_END:
                        url_caret_ = len;
                        if (!shift) url_anchor_ = url_caret_;
                        break;
                    default:
                        if (ctrl) {
                            switch (e.key.keysym.sym) {
                            case SDLK_a:   // seleccionar todo
                            case SDLK_l:   // Ctrl+L: la dirección queda marcada
                                url_caret_  = len;
                                url_anchor_ = 0;
                                break;
                            case SDLK_c: url_copy(); break;
                            case SDLK_x: url_copy(); url_delete_selection(); break;
                            case SDLK_v: url_paste(); break;
                            default: break;
                            }
                        }
                        break;
                    }
                    break;   // focused: the page does not see keys
                }
                if (!app_ && e.key.keysym.mod & KMOD_CTRL && e.key.keysym.sym == SDLK_l) {
                    url_focus_ = true;
                    std::string shown;
#ifdef LUXIUM_CEF
                    if (engine_cef_ && !cef::url().empty()) shown = cef::url();
#endif
                    if (shown.empty())
                        shown = page_.location().rfind("luxium:", 0) == 0 ? "" : page_.location();
                    url_edit_  = shown;
                    url_caret_  = static_cast<int>(url_edit_.size());
                    url_anchor_ = 0;   // como Chrome: la dirección queda marcada
                    url_scroll_ = 0;
                    break;
                }
                if (e.key.keysym.mod & KMOD_CTRL && e.key.keysym.sym == SDLK_r) {
                    if (engine_cef_) cef::reload();
                    else navigate_to(hist_[hist_idx_], false);
                    break;
                }
                if (!app_ && e.key.keysym.mod & KMOD_CTRL && e.key.keysym.sym == SDLK_u) {
                    toggle_view_source();
                    break;
                }
                if (!engine_cef_ && (e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_LEFT &&
                    page_.history_go(-1)) break;
                if (!engine_cef_ && (e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_RIGHT &&
                    page_.history_go(1)) break;
                if ((e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_LEFT &&
                    hist_idx_ > 0) {
                    --hist_idx_;
#ifdef LUXIUM_CEF
                    if (engine_cef_) cef::go_back();
                    else
#endif
                    navigate_to(hist_[hist_idx_], false);
                    break;
                }
                if ((e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_RIGHT &&
                    hist_idx_ + 1 < hist_.size()) {
                    ++hist_idx_;
#ifdef LUXIUM_CEF
                    if (engine_cef_) cef::go_forward();
                    else
#endif
                    navigate_to(hist_[hist_idx_], false);
                    break;
                }
#ifdef LUXIUM_CEF
                if (engine_cef_) {
                    if ((e.key.keysym.mod & KMOD_CTRL) && !(e.key.keysym.mod & (KMOD_ALT | KMOD_SHIFT))) {
                        const int sc = e.key.keysym.sym;
                        if (sc == SDLK_c || sc == SDLK_x || sc == SDLK_v || sc == SDLK_a) {
                            cef::edit(static_cast<char>(sc));
                            break;
                        }
                    }
                    // Todo lo que el chrome no se quedo (letras, Esc, flechas,
                    // Ctrl+C/V...) va a Chromium como evento nativo.
                    forward_cef_key(e.key.keysym, 0, cef_display_, e.key.repeat);
                    break;
                }
#endif
                if (!key_dispatched_ && forward_key_to_page(e, true)) break;   // a script handled it (event.prevent())
                key_dispatched_ = false;
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: if (!app_) quit = true; break;
                case SDLK_PAGEUP:   scroll_ = std::max(0.f, scroll_ - page_h() * 0.8f); break;
                case SDLK_PAGEDOWN: scroll_ = std::min(std::max(0.f, content_h_ - page_h()),
                                                       scroll_ + page_h() * 0.8f); break;
                case SDLK_HOME: scroll_ = 0; break;
                case SDLK_END:  scroll_ = std::max(0.f, content_h_ - page_h()); break;
                }
                break;
            case SDL_KEYUP:
#ifdef LUXIUM_CEF
                if (engine_cef_ && !url_focus_) {
                    forward_cef_key(e.key.keysym, 1, cef_display_, false);
                    break;
                }
#endif
                if (!url_focus_) forward_key_to_page(e, false);
                break;
            }
    return quit;
}

// One iteration of everything that is not input: fetches, timers, layout, paint, present.
void Shell::frame() {
        poll_fetch();          // network pages land here when the worker ends
#ifdef LUXIUM_CEF
        if (engine_cef_) {
            // console.log/warn/error de Chromium -> nuestro panel
            int lv = 0;
            std::string tx;
            while (cef::pop_console(&lv, &tx)) {
                page_.console(lv, tx);
                console_scroll_ = 0;   // el panel salta a la ultima linea
            }
            set_fullscreen(cef::fullscreen());
            update_cef_cursor();
            // el titulo de la ventana sigue a la pagina de Chromium
            static std::string last_title;
            const std::string t = cef::title();
            if (!t.empty() && t != last_title) {
                last_title = t;
                SDL_SetWindowTitle(window_, ("Luxium \xE2\x80\x94 " + t).c_str());
            }
        }
#endif
        poll_images();
        page_.tick();          // timers (page.after) fire here, frame-driven
        form_frame();          // text input on/off, caret, file chooser result
        if (anim_active_) page_.page_mutated();   // a transition is running: re-lay out with the next sample
        tick_media();
        if (page_.dirty()) {
            relayout();
            console_scroll_ = 0;   // new output snaps the pane to the tail
            need_paint_ = true;
        }
        // App windows sit idle between events: repaint only when something changed (video overlays animate).
        if (app_ && !need_paint_ && layout_.media.empty() && !std::getenv("LUXIUM_DUMP_FRAME")) return;
        need_paint_ = false;
        repaint();
        if (overlay_ && !fb_.empty()) overlay_(fb_.data(), win_w_, win_h_);

        if (!renderer_) {   // app window: blit into the window surface
            SDL_Surface* sf = fb_.empty() ? nullptr : SDL_GetWindowSurface(window_);
            if (sf && sf->w == win_w_ && sf->h == win_h_ && sf->format->BytesPerPixel == 4) {
                SDL_LockSurface(sf);
                for (int y = 0; y < win_h_; ++y)
                    std::memcpy(static_cast<uint8_t*>(sf->pixels) + static_cast<size_t>(y) * sf->pitch,
                                fb_.data() + static_cast<size_t>(y) * win_w_, static_cast<size_t>(win_w_) * 4);
                SDL_UnlockSurface(sf);
                SDL_UpdateWindowSurface(window_);
            }
        } else {
            if (texture_ && !fb_.empty()) {
                SDL_UpdateTexture(texture_, nullptr, fb_.data(), win_w_ * 4);
                SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
            }
            SDL_RenderPresent(renderer_);
        }

        // Debug hook: dump the real windowed loop's framebuffer (what the
        // user's screen gets) -- LUXIUM_DUMP_FRAME=/path/out.ppm.  Dumps
        // itself at 0.25s and then every ~3s; LUXIUM_DUMP_TRIGGER names a
        // file that, when it exists, forces a dump and is consumed --
        // that makes interactive tests deterministic.
        if (const char* dump = std::getenv("LUXIUM_DUMP_FRAME")) {
            static int frames = 0;
            bool want = (++frames == 30 || frames % 360 == 0);
            if (!want)
                if (const char* trig = std::getenv("LUXIUM_DUMP_TRIGGER"))
                    if (FILE* tf = std::fopen(trig, "rb")) {
                        std::fclose(tf);
                        std::remove(trig);
                        want = true;
                    }
            if (want) {
                FILE* f = std::fopen(dump, "wb");
                if (f) {
                    std::fprintf(f, "P6\n%d %d\n255\n", win_w_, win_h_);
                    for (size_t i = 0; i < fb_.size(); ++i) {
                        uint8_t rgb[3] = {uint8_t(fb_[i] >> 16), uint8_t(fb_[i] >> 8), uint8_t(fb_[i])};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                    std::printf("dumped frame %d to %s\n", frames, dump);
                }
            }
        }
}

// [{"t":"name","x":..,"y":..,"w":..,"h":..},...]  (mismo formato que la sonda)
static void write_boxes_json(const std::string& path,
                             const std::vector<render::BoxRect>& boxes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fputc('[', f);
    for (size_t i = 0; i < boxes.size(); ++i)
        std::fprintf(f, "%s{\"t\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f}",
                     i ? "," : "", boxes[i].name.c_str(), boxes[i].x, boxes[i].y,
                     boxes[i].w, boxes[i].h);
    std::fputs("]\n", f);
    std::fclose(f);
}

int Shell::screenshot(const std::string& ppm_path) {
    // The SAME startup the windowed run does, on the dummy driver: if this
    // path renders, the windowed path renders -- they share ensure_surface(),
    // relayout and repaint now.  (They did not, and the window was black
    // while --shot looked fine: two code paths is how that happens.)
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window_ = SDL_CreateWindow(cfg_.title.c_str(), SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, cfg_.width, cfg_.height, 0);
    renderer_ = SDL_CreateRenderer(window_, -1, 0);
    font_ = render::Font::load_dejavu("/usr/share/fonts/truetype/dejavu");
    if (!font_) return 1;
    ensure_surface();
    // Let media play: players decode on their own threads, we only pump them.
    // Clicks fire at their own time (ms after start), the shot after the last.
    const uint32_t t0 = SDL_GetTicks();
    unsigned end = shot_wait_ms_;
    for (const auto& c : shot_clicks_) end = std::max(end, c.at_ms);
    for (const auto& ev : shot_events_) end = std::max(end, ev.at_ms + 50);
    std::stable_sort(shot_events_.begin(), shot_events_.end(), [](const auto& a, const auto& b) { return a.at_ms < b.at_ms; });
    size_t next_ev = 0;
    size_t next = 0;
    std::sort(shot_clicks_.begin(), shot_clicks_.end(), [](const auto& a, const auto& b) { return a.at_ms < b.at_ms; });
    while (SDL_GetTicks() - t0 < end || next < shot_clicks_.size()) {
        while (next < shot_clicks_.size() && SDL_GetTicks() - t0 >= shot_clicks_[next].at_ms) {
            handle_click(shot_clicks_[next].x, shot_clicks_[next].y);
            ++next;
        }
        while (next_ev < shot_events_.size() && SDL_GetTicks() - t0 >= shot_events_[next_ev].at_ms)
            run_shot_event(shot_events_[next_ev++]);
        poll_fetch();   // an http page lands here, and its scripts run in the frames that follow
        poll_images();
        page_.tick();
        form_frame();
        if (anim_active_) page_.page_mutated();
        if (page_.dirty()) relayout();
        tick_media();
        repaint();   // overlays read the clock when drawn
        SDL_Delay(10);
    }
    // clicks may have navigated to an http page: give the worker a chance
    for (int i = 0; i < 250 && fetch_ && !fetch_->done(); ++i)
        SDL_Delay(20);
    poll_fetch();
    repaint();
    if (!boxes_path_.empty()) write_boxes_json(boxes_path_, layout_.boxes);

    FILE* f = std::fopen(ppm_path.c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "P6\n%d %d\n255\n", win_w_, win_h_);
    for (size_t i = 0; i < fb_.size(); ++i) {
        uint8_t rgb[3] = {uint8_t(fb_[i] >> 16), uint8_t(fb_[i] >> 8), uint8_t(fb_[i])};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("wrote %s (%dx%d)\n", ppm_path.c_str(), win_w_, win_h_);
    return 0;
}

bool Shell::load_shot_script(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        ShotEvent ev;
        char kind[32] = {0}, rest[900] = {0};
        unsigned ms = 0;
        if (std::sscanf(line, "%u %31s %899[^\n]", &ms, kind, rest) < 2) continue;
        ev.at_ms = ms; ev.kind = kind; ev.s = rest;
        std::sscanf(rest, "%d %d %d", &ev.a, &ev.b, &ev.c);
        shot_events_.push_back(std::move(ev));
    }
    std::fclose(f);
    return true;
}

void Shell::run_shot_event(const ShotEvent& ev) {
    SDL_Event e{};
    if (ev.kind == "click" || ev.kind == "down" || ev.kind == "up") {
        e.button.x = ev.a; e.button.y = ev.b; e.button.button = SDL_BUTTON_LEFT; e.button.clicks = ev.c > 0 ? ev.c : 1;
        if (ev.kind != "up") { e.type = SDL_MOUSEBUTTONDOWN; e.button.state = SDL_PRESSED; handle_event(e); }
        if (ev.kind != "down") { e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED; handle_event(e); }
    } else if (ev.kind == "move") {
        e.type = SDL_MOUSEMOTION; e.motion.x = ev.a; e.motion.y = ev.b; e.motion.state = ev.c ? SDL_BUTTON_LMASK : 0;
        handle_event(e);
    } else if (ev.kind == "wheel") {
        mouse_x_ = ev.a; mouse_y_ = ev.b;
        e.type = SDL_MOUSEWHEEL; e.wheel.y = ev.c; e.wheel.preciseY = float(ev.c);
        handle_event(e);
    } else if (ev.kind == "text") {
        e.type = SDL_TEXTINPUT;
        std::snprintf(e.text.text, sizeof e.text.text, "%s", ev.s.c_str());
        handle_event(e);
    } else if (ev.kind == "edit") {   // IME preedit
        e.type = SDL_TEXTEDITING;
        std::snprintf(e.edit.text, sizeof e.edit.text, "%s", ev.s.c_str());
        handle_event(e);
    } else if (ev.kind == "key") {
        char name[64] = {0};
        std::sscanf(ev.s.c_str(), "%63s", name);
        SDL_Keycode k = SDL_GetKeyFromName(name);
        e.key.keysym.sym = k;
        if (ev.s.find("shift") != std::string::npos) e.key.keysym.mod |= KMOD_LSHIFT;
        if (ev.s.find("ctrl") != std::string::npos) e.key.keysym.mod |= KMOD_LCTRL;
        if (ev.s.find("alt") != std::string::npos) e.key.keysym.mod |= KMOD_LALT;
        e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED; handle_event(e);
        e.type = SDL_KEYUP; e.key.state = SDL_RELEASED; handle_event(e);
    }
}

// El ORACULO (docs/plan-css.md fase 0): la pagina la pinta Chromium en
// off-screen y volcamos NUESTRO framebuffer, que dentro del area de pagina
// son pixeles exactos de Chromium.  Ventana real pero OCULTA: OSR no la
// necesita para pintar, solo quiere el display X11 vivo.  Con esto se
// generan los goldens de css-tests (docs/plan-css-tests.md).
int Shell::cef_shot(const std::string& target, const std::string& ppm_path) {
#ifdef LUXIUM_CEF
    if (!cef::built()) {
        std::fprintf(stderr, "cefshot: binario sin CEF\n");
        return 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window_ = SDL_CreateWindow(cfg_.title.c_str(), SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, cfg_.width, cfg_.height,
                               SDL_WINDOW_HIDDEN);
    renderer_ = SDL_CreateRenderer(window_, -1, 0);
    font_ = render::Font::load_dejavu("/usr/share/fonts/truetype/dejavu");
    if (!font_) return 1;
    ensure_surface();

    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!window_ || !SDL_GetWindowWMInfo(window_, &wm)) {
        std::fprintf(stderr, "cefshot: se necesita una sesion X11 real\n");
        return 1;
    }
    cef_display_ = (unsigned long)wm.info.x11.display;
    cef_parent_  = (unsigned long)wm.info.x11.window;
    cef::init(cef_display_, cef_parent_);

    std::string url = target;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        std::error_code ec;
        auto abs = std::filesystem::absolute(target, ec);
        url = std::string("file://") + (ec ? std::string("/") + target
                                           : abs.string());
    }
    // el juicio NO aplica aqui: el oraculo es Chromium SIEMPRE
    cef::attach(cef_display_, cef_parent_, url);
    cef::set_view(win_w_, win_h_ - bottom_h() - url_h());
    cef::set_focus(true);
    engine_cef_ = true;

    // estable = sin frames nuevos de OnPaint durante 600ms (imagenes,
    // fuentes y layout tardan); techo de 20s
    int last_paints = -1, stable_ms = 0, total_ms = 0;
    while (stable_ms < 600 && total_ms < 20000) {
        int lv = 0;
        std::string tx;
        while (cef::pop_console(&lv, &tx)) page_.console(lv, tx);
        page_.tick();
        repaint();
        if (texture_ && !fb_.empty()) {
            SDL_UpdateTexture(texture_, nullptr, fb_.data(), win_w_ * 4);
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        }
        SDL_RenderPresent(renderer_);
        const int p = cef::paints();
        if (p == last_paints) stable_ms += 8;
        else { stable_ms = 0; last_paints = p; }
        total_ms += 8;
        SDL_Delay(8);
    }
    if (cef::paints() == 0) {
        std::fprintf(stderr, "cefshot: Chromium no pinto nada en 20s\n");
        cef::shutdown();
        return 1;
    }
    repaint();   // ultimo blit con el frame final

    if (!probe_path_.empty()) {
        // La sonda: rect (border-box, coords de documento) de cada [data-t].
        // display "inline" va marcado: sus fragmentos no son una caja.
        std::string props = "[";
        for (const auto& n : style::probe_properties()) props += "'" + n + "',";
        props += "]";
        cef::run_js(
            "(()=>{const P=" + props + ";const o=[];for(const e of document.querySelectorAll('[data-t]')){"
            "const r=e.getBoundingClientRect();const g=getComputedStyle(e);const cs={};"
            "for(const p of P)cs[p]=g.getPropertyValue(p);"
            "o.push({t:e.getAttribute('data-t'),x:+(r.x+scrollX).toFixed(2),"
            "y:+(r.y+scrollY).toFixed(2),w:+r.width.toFixed(2),h:+r.height.toFixed(2),"
            "d:g.display,cs:cs});}"
            "console.log('LUXPROBE'+JSON.stringify(o));})()");
        for (int waited = 0; waited < 5000; waited += 10) {
            int lv = 0;
            std::string tx;
            bool got = false;
            while (cef::pop_console(&lv, &tx))
                if (tx.rfind("LUXPROBE", 0) == 0) {
                    if (FILE* pf = std::fopen(probe_path_.c_str(), "wb")) {
                        std::fputs(tx.c_str() + 8, pf);
                        std::fputc('\n', pf);
                        std::fclose(pf);
                    }
                    got = true;
                }
            if (got) break;
            SDL_Delay(10);
        }
    }

    FILE* f = std::fopen(ppm_path.c_str(), "wb");
    if (!f) { cef::shutdown(); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", win_w_, win_h_);
    for (size_t i = 0; i < fb_.size(); ++i) {
        uint8_t rgb[3] = {uint8_t(fb_[i] >> 16), uint8_t(fb_[i] >> 8), uint8_t(fb_[i])};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("wrote %s (%dx%d, %d paints)\n", ppm_path.c_str(),
                win_w_, win_h_, cef::paints());
    cef::shutdown();
    return 0;
#else
    (void)target;
    (void)ppm_path;
    std::fprintf(stderr, "cefshot: binario sin CEF\n");
    return 1;
#endif
}

float Shell::page_h() const { return static_cast<float>(win_h_ - bottom_h() - url_h()); }

void Shell::relayout() {
    media::own::Scope media_scope(this);   // this window's players only (sweep)
    if (win_w_ == 0 || !page_.page_root()) return;   // http pages start empty

    // The hovered node may have been remove()d: its Hover chain is cut, so
    // clear the bit document-wide and let the next mouse move set it again.
    if (hover_node_) {
        const dom::Node* top = hover_node_;
        while (top->parent) top = top->parent;
        if (top != page_.page_root()) {
            std::vector<dom::Node*> st{page_.page_root()};
            while (!st.empty()) {
                dom::Node* n = st.back();
                st.pop_back();
                n->state &= static_cast<unsigned char>(~dom::Node::Hover);
                for (auto& c : n->children) st.push_back(c.get());
            }
            hover_node_ = nullptr;
        }
    }

    const Uint64 t_layout0 = SDL_GetPerformanceCounter();
    page_.clear_dirty();
    layout_ = render::layout(*page_.page_root(), *font_,
                             static_cast<float>(win_w_), page_.rules(),
                             page_.base_dir(),
                             static_cast<float>(url_h()),
                             static_cast<float>(win_h_ - bottom_h()),
                             [this](const dom::Node& el, style::ComputedStyle& st, int pseudo) { return style_hook(el, st, pseudo); },
                             layout_.gutter);
    // forget animation state of elements that are gone; keep the frame loop alive while a tween runs
    ++anim_frame_;
    anim_active_ = false;
    for (auto it = anims_.begin(); it != anims_.end();) {
        if (it->second.seen + 1 < anim_frame_) it = anims_.erase(it);
        else { anim_active_ = anim_active_ || it->second.active || it->second.kf_active; ++it; }
    }
    if (std::getenv("LUXIUM_TIMING"))
        std::fprintf(stderr, "relayout %.2f ms (%zu boxes hit, %zu items)\n",
                     double(SDL_GetPerformanceCounter() - t_layout0) * 1000.0 / double(SDL_GetPerformanceFrequency()), layout_.areas.size(), layout_.list.size());
    content_h_ = layout_.content_height;
    page_.free_removed();   // the new layout no longer points into removed nodes
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, content_h_ - page_h()));
    if (std::getenv("LUXIUM_DEBUG_LAYOUT"))
        for (const auto& d : layout_.boxes)
            std::printf("box %s x=%.1f y=%.1f %.1fx%.1f\n", d.name.c_str(), d.x, d.y, d.w, d.h);
}

bool Shell::handle_click(int mx, int my, int clicks) {
    if (app_ && my < url_h()) return false;
    if (my < url_h()) {   // the address strip: caret placement + selection
        if (!url_focus_) {
            url_focus_  = true;
            std::string shown;
#ifdef LUXIUM_CEF
            if (engine_cef_ && !cef::url().empty()) shown = cef::url();
#endif
            if (shown.empty())
                shown = page_.location().rfind("luxium:", 0) == 0 ? "" : page_.location();
            url_edit_   = shown;
            url_caret_  = static_cast<int>(url_edit_.size());
            url_anchor_ = url_caret_;
            url_scroll_ = 0;
        }
        const int pos = url_caret_at(mx);
        if (clicks >= 3) {                     // triple: todo el texto
            url_anchor_ = 0;
            url_caret_  = static_cast<int>(url_edit_.size());
        } else if (clicks == 2) {              // doble: la palabra
            const int wl = url_word_left(url_edit_, pos);
            url_anchor_ = wl;
            url_caret_  = url_word_right(url_edit_, wl);
        } else if (!(SDL_GetModState() & KMOD_SHIFT)) {   // simple: caret
            url_caret_ = pos;
            url_anchor_ = pos;
        } else {                               // shift+click: extiende
            url_caret_ = pos;
        }
        url_drag_ = true;
        return true;
    }
    if (my >= win_h_ - bottom_h()) return false;   // status strip + console
    url_focus_ = false;
    url_drag_  = false;
    const float doc_x = static_cast<float>(mx);
    const float doc_y = static_cast<float>(my - url_h()) + scroll_;
    for (auto it = layout_.media.rbegin(); it != layout_.media.rend(); ++it)   // control bars
        if (render::media_click(*it, doc_x, it->fixed ? static_cast<float>(my - url_h()) : doc_y)) return true;
    if (const dom::Node* n = render::hit_test(layout_, doc_x, doc_y)) {
        page_.click(n, doc_x, doc_y);
        if (clicks == 2) page_.dispatch("dblclick", n);
        return true;
    }
    return false;
}

// :hover -- the element under the mouse and its ancestors carry the Hover
// state; a style change re-runs layout (only pages that mention :hover).
void Shell::update_hover() {
    if (!page_.page_root() || (!page_.uses_hover() && !page_.wants_event("mousemove") && !page_.wants_event("mouseenter") && !page_.wants_event("mouseleave"))) return;
    const dom::Node* target = nullptr;
    if (mouse_y_ >= url_h() && mouse_y_ < win_h_ - bottom_h() && !url_focus_) {
        const float dx = static_cast<float>(mouse_x_);
        const float dy = static_cast<float>(mouse_y_ - url_h()) + scroll_;
        for (const auto& a : layout_.areas)
            if (dx >= a.x && dx < a.x + a.w && dy >= a.y && dy < a.y + a.h) target = a.node;
    }
    if (target && page_.wants_event("mousemove")) {   // pointer movement as events
        EventData mv;
        mv.x = static_cast<float>(mouse_x_);
        mv.y = static_cast<float>(mouse_y_ - url_h()) + scroll_;
        page_.dispatch("mousemove", target, mv);
    }
    if (target == hover_node_) return;
    if (page_.wants_event("mouseenter") || page_.wants_event("mouseleave")) {
        auto chain = [](const dom::Node* n) { std::vector<const dom::Node*> v; for (; n; n = n->parent) v.push_back(n); return v; };
        const auto old_c = chain(hover_node_), new_c = chain(target);
        for (const dom::Node* n : old_c)
            if (std::find(new_c.begin(), new_c.end(), n) == new_c.end() && n->is_element()) {
                page_.dispatch("mouseleave", n);
            }
        for (auto it = new_c.rbegin(); it != new_c.rend(); ++it)
            if (std::find(old_c.begin(), old_c.end(), *it) == old_c.end() && (*it)->is_element()) {
                page_.dispatch("mouseenter", *it);
            }
    }
    auto set = [](const dom::Node* n, bool on) {
        for (; n; n = n->parent) {
            auto* m = const_cast<dom::Node*>(n);
            if (on) m->state |= dom::Node::Hover; else m->state &= static_cast<unsigned char>(~dom::Node::Hover);
        }
    };
    set(hover_node_, false);
    set(target, true);
    hover_node_ = target;
    if (page_.uses_hover()) relayout();
}

void Shell::draw_scrollbar() {
    // Chromium's classic Linux scrollbar, measured from its output: a 15px
    // track (#fcfcfc), 5-row arrows, a 9px-wide rounded thumb inset 3px.
    const float page = page_h();
    if (content_h_ <= page || page <= 0) return;
    const int x0 = win_w_ - 15, top = url_h(), h = static_cast<int>(page);
    const bool dark = page_.dark_scheme();
    auto put = [&](int x, int y, uint32_t c) {
        if (x < 0 || x >= win_w_ || y < top || y >= top + h) return;
        fb_[static_cast<size_t>(y) * win_w_ + x] = c;
    };
    for (int y = top; y < top + h; ++y)
        for (int x = x0; x < win_w_; ++x) put(x, y, dark ? 0xFF2C2C2C : 0xFFFCFCFC);
    const uint32_t k = dark ? 0xFF9F9F9F : 0xFF8B8B8B;
    for (int i = 0; i < 5; ++i) {   // up arrow: apex at row 6, base at row 10
        for (int dx = -i; dx <= i; ++dx) put(x0 + 7 + dx, top + 6 + i, k);
        for (int dx = -(4 - i); dx <= 4 - i; ++dx) put(x0 + 7 + dx, top + h - 12 + i, k);   // down arrow
    }
    const float track = page - 30.f;
    const float thumb = std::max(24.f, track * page / content_h_);
    const float off = scroll_ / (content_h_ - page) * (track - thumb);
    paint::DisplayList dl;
    paint::Item it; it.kind = paint::Item::Kind::Rect;
    it.x = static_cast<float>(x0 + 3); it.w = 9;
    it.y = std::round(15.f + 3.f + off); it.h = std::round(thumb - 6.f);
    it.radii = {4.5f, 4.5f, 4.5f, 4.5f}; it.color = k;
    dl.push_back(it);
    paint::Canvas cv;
    cv.px = std::span<uint32_t>(fb_.data(), fb_.size());
    cv.w = win_w_; cv.h = win_h_;
    cv.clip = {x0, top, win_w_, top + h};
    cv.ox = 0; cv.oy = static_cast<float>(top);
    paint::rasterize(dl, cv, *font_);
}

void Shell::draw_console() {
    if (!console_visible_) return;
    const int y0 = win_h_ - bottom_h();
    const int y_end = win_h_ - status_h();   // la tira de estado sigue debajo
    // pane background + its top border
    for (int y = y0 + 1; y < y_end; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFF171B21;
    }
    for (int x = 0; x < win_w_; ++x) fb_[static_cast<size_t>(y0) * win_w_ + x] = 0xFF2E3540;

    // header strip: "Consola" + the error count
    const int header_h = 26;
    for (int y = y0 + 1; y < y0 + 1 + header_h && y < y_end; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFF1E232B;
    }
    font_->set_size(11.5f);
    int errors = 0, warns = 0;
    for (const auto& l : page_.console_lines()) {
        if (l.level == 2) ++errors;
        if (l.level == 1) ++warns;
    }
    std::string header = "Consola";
    if (errors)   header += "  \xC2\xB7  " + std::to_string(errors) + " error" + (errors > 1 ? "es" : "");
    if (warns)    header += "  \xC2\xB7  " + std::to_string(warns) + " avisos";
    {
        float x = 12;
        for (const auto& w : render::shape_words(*font_, header, true, false)) {
            render::draw_word(fb_, win_w_, win_h_, *font_, w, x,
                              static_cast<float>(y0) + 7, errors ? 0xFFFF8A7A : 0xFF8B94A3);
            x += w.width + 4;
        }
    }

    // lines: mono, level dot on the left
    font_->set_size(12.5f);
    const float lh = font_->line_height() + 3;
    const float body_y = static_cast<float>(y0 + 1 + header_h) + 6;
    const auto& lines = page_.console_lines();

    int visible = static_cast<int>((console_h_ - header_h - 12) / lh);
    int end = static_cast<int>(lines.size()) - static_cast<int>(console_scroll_ / lh);
    int begin = std::max(0, end - visible);

    float y = body_y;
    for (int i = begin; i < end && y < y_end - lh; ++i, y += lh) {
        const auto& l = lines[static_cast<size_t>(i)];
        const uint32_t dot   = l.level == 2 ? 0xFFFF6B5E
                             : l.level == 1 ? 0xFFFFC24B
                                            : 0xFF5FA1F7;
        const uint32_t color = l.level == 2 ? 0xFFFF9C91
                             : l.level == 1 ? 0xFFFFD79A
                                            : 0xFFAEB6C2;
        // the dot
        const float cy = y + font_->line_height() * 0.5f;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (dx * dx + dy * dy <= 5) {
                    const int px = 14 + dx, py = static_cast<int>(cy) + dy;
                    if (py > y0 && py < y_end && px >= 0 && px < win_w_)
                        fb_[static_cast<size_t>(py) * win_w_ + px] = dot;
                }
        float x = 24;
        for (const auto& w : render::shape_words(*font_, l.text, false, true)) {
            render::draw_word(fb_, win_w_, win_h_, *font_, w, x, y, color);
            x += w.width + 12.5f * 0.30f;
        }
    }
}

void Shell::repaint() {
    const Uint64 t_paint0 = SDL_GetPerformanceCounter();
    struct PaintTimer { Uint64 t0; ~PaintTimer() { if (std::getenv("LUXIUM_TIMING")) std::fprintf(stderr, "repaint %.2f ms\n", double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency())); } } paint_timer{t_paint0};
    if (win_w_ == 0 || engine_cef_) {   // CEF pinta la pagina; nosotros el chrome
#ifdef LUXIUM_CEF
        if (engine_cef_ && !fb_.empty()) {
            // OSR: los pixeles BGRA de Chromium entran en el framebuffer
            // (mismo layout ARGB little-endian) solo en el area de pagina.
            int fw = 0, fh = 0;
            uint8_t* page = reinterpret_cast<uint8_t*>(fb_.data()) +
                            static_cast<size_t>(url_h()) * win_w_ * 4;
            const int ph = win_h_ - bottom_h() - url_h();
            if (ph > 0)
                cef::copy_frame(page, win_w_ * 4, win_w_, ph, &fw, &fh);
        }
#endif
        if (!fullscreen_ && !app_) {
            draw_url_bar();
            draw_status();
            draw_console();
        }
        ++frames_;
        return;
    }
    if (!page_.page_root()) {
        if (app_) std::fill(fb_.begin(), fb_.end(), 0xFFFFFFFFu);   // nothing loaded yet
        else draw_url_bar();
        return;
    }
    const int page_top    = url_h();
    const int page_bottom = win_h_ - bottom_h();

    // paper
    for (int y = page_top; y < page_bottom; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = layout_.canvas_bg;
    }

    // hover state: which button is under the cursor (document coords)
    const render::HitBox* hover = nullptr;
    if (mouse_y_ >= page_top && mouse_y_ < page_bottom && !url_focus_) {
        const float dx = static_cast<float>(mouse_x_);
        const float dy = static_cast<float>(mouse_y_ - page_top) + scroll_;
        for (const auto& h : layout_.hits)
            if (h.button && dx >= h.x && dx <= h.x + h.w && dy >= h.y && dy <= h.y + h.h)
                hover = &h;
    }
    cursor_over_clickable_ = hover != nullptr;
    update_cursor_shape();
    if (page_.uses_hover()) hover = nullptr;   // the page styles :hover itself: no lightening overlay on top

    // the page: display list in document coordinates, then fixed boxes in viewport coordinates
    {
        paint::Canvas cv;
        cv.px = std::span<uint32_t>(fb_.data(), fb_.size());
        cv.w = win_w_; cv.h = win_h_;
        cv.clip = {0, page_top, win_w_, page_bottom};
        cv.ox = 0; cv.oy = static_cast<float>(page_top) - scroll_;
        render::media_patch(layout_);
        const Uint64 tr0 = SDL_GetPerformanceCounter();
        paint::rasterize(layout_.list, cv, *font_);
        if (std::getenv("LUXIUM_TIMING")) std::fprintf(stderr, "  raster list %.2f ms\n", double(SDL_GetPerformanceCounter() - tr0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
        paint::rasterize(render::media_overlay(layout_, false, *font_), cv, *font_);
        apply_sticky();
        for (size_t i = 0; i < layout_.sticky.size(); ++i) {
            const float saved_oy = cv.oy;
            cv.oy += sticky_dy_[i];
            paint::rasterize(layout_.sticky[i].list, cv, *font_);
            cv.oy = saved_oy;
        }
        if (hover) {   // the hovered button lightens: an overlay over its box
            paint::DisplayList ov;
            paint::Item it; it.kind = paint::Item::Kind::Rect;
            it.x = hover->x; it.y = hover->y; it.w = hover->w; it.h = hover->h;
            it.radii = {7, 7, 7, 7}; it.color = 0x22FFFFFF;
            ov.push_back(std::move(it));
            paint::rasterize(ov, cv, *font_);
        }
        cv.oy = static_cast<float>(page_top);
        paint::rasterize(layout_.fixed, cv, *font_);
        paint::rasterize(render::media_overlay(layout_, true, *font_), cv, *font_);
    }
    draw_select_popup();

    if (!app_) draw_status();
    draw_scrollbar();
    draw_console();
    if (!app_) draw_url_bar();
    ++frames_;
}

// The status bar: hovered link (or fetch state) on the left, page name on
// the right.  Sits at the bottom of the page area, above the console.
void Shell::draw_status() {
    const int y0 = win_h_ - 20;   // tira fina pegada al borde inferior
    if (y0 < 0) return;
    for (int y = y0; y < y0 + 20; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFFEEF0F2;
    }
    for (int x = 0; x < win_w_; ++x) fb_[static_cast<size_t>(y0) * win_w_ + x] = 0xFFD8DBDF;

    font_->set_size(11.5f);
    float x = 8;
    std::string text;
    uint32_t color = 0xFF5A6270;
    if (fetch_) {
        text = "cargando " + fetch_->url() + " ...";
        color = 0xFF2F6FED;
    } else if (mouse_y_ < y0) {
        const float dx = static_cast<float>(mouse_x_);
        const float dy = static_cast<float>(mouse_y_) + scroll_;
        if (const dom::Node* n = render::hit_test(layout_, dx, dy)) {
            for (const dom::Node* a = n; a; a = a->parent) {
                if (!a->is_element()) continue;
                std::string href = a->attr_or("href");
                if (!href.empty()) { text = "enlace: " + href; break; }
            }
        }
    }
    if (text.empty()) text = engine_cef_ ? cef::url() : page_.location();
    for (const auto& w : render::shape_words(*font_, text, false, false)) {
        if (x > win_w_ - 80) break;
        render::draw_word(fb_, win_w_, win_h_, *font_, w, x,
                          static_cast<float>(y0) + 4, color);
        x += w.width + 11.5f * 0.30f;
    }
}

} // namespace luxium::shell
