#include "shell/shell.hpp"

#include <net/fetch.hpp>

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace luxium::shell {

using render::ShapeCmd;

Shell::Shell(page::Page& page, ShellConfig cfg) : page_(page), cfg_(std::move(cfg)) {
    // Clicks on <a href> come back here from the page, whatever the run mode.
    page_.on_navigate = [this](const std::string& href) { navigate_to(href, true); };
}

Shell::~Shell() {
    if (cursor_hand_) SDL_FreeCursor(cursor_hand_);
    if (texture_)  SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_)   SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Shell::ensure_surface() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (w == win_w_ && h == win_h_) return;
    win_w_ = w;
    win_h_ = h;
    console_h_ = std::max(140, win_h_ * 26 / 100);
    fb_.assign(static_cast<size_t>(win_w_) * win_h_, 0xFFFAFAF7u);
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, win_w_, win_h_);
    relayout();
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

// Signed-distance rounded-rect test: 0 inside, >0 outside (in px).
static inline float rrect_sdf(float px, float py, float x, float y, float w,
                              float h, float r) {
    const float cx = std::max(x + r - px, px - (x + w - r));
    const float cy = std::max(y + r - py, py - (y + h - r));
    const float dx = std::max(cx, 0.f), dy = std::max(cy, 0.f);
    const float d = std::sqrt(dx * dx + dy * dy) - r;
    return std::max(d, std::min(std::max(x - px, px - (x + w)),
                                std::max(y - py, py - (y + h))));
}

static void fill_shape(std::vector<uint32_t>& fb, int fb_w, int fb_h,
                       const ShapeCmd& s, float scroll, float alpha_mult) {
    const int x0 = std::max(0, static_cast<int>(s.x) - 1);
    const int y0 = std::max(0, static_cast<int>(s.y - scroll) - 1);
    const int x1 = std::min(fb_w, static_cast<int>(s.x + s.w) + 2);
    const int y1 = std::min(fb_h, static_cast<int>(s.y - scroll + s.h) + 2);
    if (x0 >= x1 || y0 >= y1) return;

    for (int y = y0; y < y1; ++y) {
        uint32_t* row = fb.data() + static_cast<size_t>(y) * fb_w;
        for (int x = x0; x < x1; ++x) {
            float d = 0;
            switch (s.kind) {
            case ShapeCmd::Kind::Rect:
                d = 0;
                break;
            case ShapeCmd::Kind::RRect:
                d = rrect_sdf(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                              s.x, s.y - scroll, s.w, s.h, s.r);
                break;
            case ShapeCmd::Kind::Circle:
                d = std::sqrt((x + 0.5f - s.x) * (x + 0.5f - s.x) +
                              (y + 0.5f - s.y - scroll) * (y + 0.5f - s.y - scroll)) -
                    s.r;
                break;
            }
            if (d > 0.5f) continue;
            uint32_t argb = s.argb;
            if (d > -0.5f) {   // the antialiased edge: alpha by coverage
                const uint32_t a = (argb >> 24) & 0xFF;
                const uint32_t cov = static_cast<uint32_t>((0.5f - d) * 255.f);
                argb = (a * cov / 255) << 24 | (argb & 0xFFFFFF);
            }
            if (((argb >> 24) & 0xFF) == 255 && alpha_mult == 1.f) {
                uint32_t& dst = row[static_cast<size_t>(x)];
                const float ca = alpha_mult;
                const uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
                const uint32_t sr = (argb >> 16) & 0xFF, sg = (argb >> 8) & 0xFF, sb = argb & 0xFF;
                dst = 0xFF000000u |
                      uint32_t(sr * ca + dr * (1 - ca)) << 16 |
                      uint32_t(sg * ca + dg * (1 - ca)) << 8 |
                      uint32_t(sb * ca + db * (1 - ca));
            } else {
                const uint32_t a = ((argb >> 24) & 0xFF);
                blend_px(row[static_cast<size_t>(x)],
                         (uint32_t(a * alpha_mult) << 24) | (argb & 0xFFFFFF));
            }
        }
    }
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
    return "<html><head><title>No se pudo cargar</title></head><body>"
           "<h1>No se pudo cargar la pagina</h1>"
           "<p><b>" + escape_html(target) + "</b></p>"
           "<p>" + escape_html(why) + "</p>"
           "<p>Prueba con otra direccion o vuelve atras con Alt+Left.</p>"
           "</body></html>";
}

void Shell::navigate_to(const std::string& href, bool push_history) {
    Resolved r = resolve(href);

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
        if (fetch_) fetch_.reset();   // one fetch at a time: the newest wins
        fetch_ = net::Fetch::start(r.target);
        if (push_history) {
            hist_.resize(hist_idx_ + 1);
            hist_.push_back(r.target);
            hist_idx_ = hist_.size() - 1;
        }
        return;   // poll_fetch() completes the load
    }

    // file
    if (!page_.load_file(r.target)) {
        page_.load_string(error_page(r.target, "no existe el archivo (o no se puede leer)"),
                          "luxium-error:" + r.target);
    }
    after_load();
    if (push_history) {
        hist_.resize(hist_idx_ + 1);
        hist_.push_back(r.target);
        hist_idx_ = hist_.size() - 1;
    }
}

void Shell::after_load() {
    scroll_ = 0;
    std::string t = "Luxium";
    if (!page_.page_title().empty()) t += " \xE2\x80\x94 " + page_.page_title();
    if (window_) SDL_SetWindowTitle(window_, t.c_str());
    relayout();
}

void Shell::poll_fetch() {
    if (!fetch_ || !fetch_->done()) return;
    const bool ok = fetch_->ok();
    const std::string body = fetch_->body();
    const std::string err  = fetch_->error();
    const std::string url  = fetch_->url();
    fetch_.reset();
    if (ok) {
        page_.load_string(body, url);
        after_load();
    } else {
        page_.load_string(error_page(url, err), "luxium-error:" + url);
        after_load();
    }
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

    font_ = render::Font::load(cfg_.font_regular, cfg_.font_bold, cfg_.font_mono);
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

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_RESIZED)
                    ensure_surface();
                break;
            case SDL_MOUSEMOTION:
                mouse_x_ = e.motion.x;
                mouse_y_ = e.motion.y;
                break;
            case SDL_MOUSEWHEEL: {
                const float lines = static_cast<float>(-e.wheel.y) * 3;
                if (mouse_y_ > win_h_ - console_h_) {
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
                if (e.button.button == SDL_BUTTON_LEFT)
                    handle_click(e.button.x, e.button.y);
                else if (e.button.button == 3) navigate_to(hist_[hist_idx_], false);      // X1 = back
                else if (e.button.button == 4 && hist_idx_ + 1 < hist_.size()) {          // X2 = forward
                    ++hist_idx_;
                    navigate_to(hist_[hist_idx_], false);
                }
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.mod & KMOD_CTRL && e.key.keysym.sym == SDLK_r) {
                    navigate_to(hist_[hist_idx_], false);   // reload
                    break;
                }
                if (e.key.keysym.mod & KMOD_CTRL && e.key.keysym.sym == SDLK_u) {
                    toggle_view_source();
                    break;
                }
                if ((e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_LEFT &&
                    hist_idx_ > 0) {
                    --hist_idx_;
                    navigate_to(hist_[hist_idx_], false);
                    break;
                }
                if ((e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_RIGHT &&
                    hist_idx_ + 1 < hist_.size()) {
                    ++hist_idx_;
                    navigate_to(hist_[hist_idx_], false);
                    break;
                }
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = true; break;
                case SDLK_PAGEUP:   scroll_ = std::max(0.f, scroll_ - page_h() * 0.8f); break;
                case SDLK_PAGEDOWN: scroll_ = std::min(std::max(0.f, content_h_ - page_h()),
                                                       scroll_ + page_h() * 0.8f); break;
                case SDLK_HOME: scroll_ = 0; break;
                case SDLK_END:  scroll_ = std::max(0.f, content_h_ - page_h()); break;
                }
                break;
            }
        }

        poll_fetch();          // network pages land here when the worker ends
        page_.tick();          // timers (page.after) fire here, frame-driven
        if (page_.dirty()) {
            relayout();
            console_scroll_ = 0;   // new output snaps the pane to the tail
        }
        repaint();

        if (texture_ && !fb_.empty()) {
            SDL_UpdateTexture(texture_, nullptr, fb_.data(), win_w_ * 4);
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        }
        SDL_RenderPresent(renderer_);

        // Debug hook: dump the real windowed loop's framebuffer (what the
        // user's screen gets) -- LUXIUM_DUMP_FRAME=/path/out.ppm
        if (const char* dump = std::getenv("LUXIUM_DUMP_FRAME")) {
            static int frames = 0;
            if (++frames == 30) {   // half a second in, past any warmup
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
        SDL_Delay(8);   // ~120fps cap; the CPU is idle at this size anyway
    }
    return 0;
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
    font_ = render::Font::load(cfg_.font_regular, cfg_.font_bold, cfg_.font_mono);
    if (!font_) return 1;
    ensure_surface();
    // clicks may have navigated to an http page: give the worker a chance
    for (int i = 0; i < 250 && fetch_ && !fetch_->done(); ++i)
        SDL_Delay(20);
    poll_fetch();
    repaint();

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

float Shell::page_h() const { return static_cast<float>(win_h_ - console_h_); }

void Shell::relayout() {
    if (win_w_ == 0 || !page_.page_root()) return;   // http pages start empty

    page_.clear_dirty();
    layout_ = render::layout(*page_.page_root(), *font_,
                             static_cast<float>(win_w_), page_.css());
    content_h_ = layout_.content_height;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, content_h_ - page_h()));
    if (std::getenv("LUXIUM_DEBUG_LAYOUT"))
        for (const auto& d : layout_.shapes)
            std::printf("shape kind=%d y=%.0f x=%.0f %.0fx%.0f\n",
                        (int)d.kind, d.y, d.x, d.w, d.h);
}

bool Shell::handle_click(int mx, int my) {
    if (my > win_h_ - console_h_) return false;   // the console is not clickable yet
    const float doc_x = static_cast<float>(mx);
    const float doc_y = static_cast<float>(my) + scroll_;
    if (const dom::Node* n = render::hit_test(layout_, doc_x, doc_y)) {
        page_.click(n);
        return true;
    }
    return false;
}

void Shell::draw_scrollbar() {
    const float page = page_h();
    if (content_h_ <= page || page <= 0) return;
    const float bar_h = std::max(36.f, page * page / content_h_);
    const float bar_y = scroll_ / (content_h_ - page) * (page - bar_h);
    // the thumb only -- no track gutter, the page edge stays clean
    const int   gx   = win_w_ - 8;
    for (float yy = bar_y; yy < bar_y + bar_h; yy += 1) {
        const int iy = static_cast<int>(yy);
        if (iy < 0 || iy >= static_cast<int>(page)) continue;
        for (int x = gx; x < gx + 5; ++x) {
            // rounded ends on the thumb
            const int ex = x - gx;
            if ((iy == static_cast<int>(bar_y) || iy == static_cast<int>(bar_y + bar_h - 1)) &&
                (ex == 0 || ex == 4))
                continue;
            fb_[static_cast<size_t>(iy) * win_w_ + x] = 0xFFC3CAD3;
        }
    }
}

void Shell::draw_console() {
    const int y0 = win_h_ - console_h_;
    // pane background + its top border
    for (int y = y0 + 1; y < win_h_; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFF171B21;
    }
    for (int x = 0; x < win_w_; ++x) fb_[static_cast<size_t>(y0) * win_w_ + x] = 0xFF2E3540;

    // header strip: "Consola" + the error count
    const int header_h = 26;
    for (int y = y0 + 1; y < y0 + 1 + header_h && y < win_h_; ++y) {
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
    for (int i = begin; i < end && y < win_h_ - lh; ++i, y += lh) {
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
                    if (py > y0 && py < win_h_ && px >= 0 && px < win_w_)
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
    if (win_w_ == 0 || !page_.page_root()) return;
    const int page_bottom = win_h_ - console_h_;

    // paper
    for (int y = 0; y < page_bottom; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFFFAFAF7;
    }

    // hover state: which button is under the cursor (document coords)
    const render::HitBox* hover = nullptr;
    if (mouse_y_ < page_bottom) {
        const float dx = static_cast<float>(mouse_x_);
        const float dy = static_cast<float>(mouse_y_) + scroll_;
        for (const auto& h : layout_.hits)
            if (h.button && dx >= h.x && dx <= h.x + h.w && dy >= h.y && dy <= h.y + h.h)
                hover = &h;
    }
    if ((hover != nullptr) != cursor_over_clickable_) {
        cursor_over_clickable_ = hover != nullptr;
        SDL_SetCursor(cursor_over_clickable_ ? cursor_hand_ : SDL_GetDefaultCursor());
    }

    // shapes first (button boxes, pills, bars), then words over them.
    for (const auto& s : layout_.shapes) fill_shape(fb_, win_w_, win_h_, s, scroll_, 1.f);
    if (hover) {   // the hovered button lightens: an overlay over the box,
                   // before the words, so the text stays crisp
        fill_shape(fb_, win_w_, win_h_,
                   {ShapeCmd::Kind::RRect, hover->x, hover->y, hover->w, hover->h, 7,
                    0x22FFFFFF},
                   scroll_, 1.f);
    }
    for (const auto& w : layout_.words) {
        if (w.y - scroll_ > page_bottom || w.y + 40 - scroll_ < 0) continue;
        render::draw_word(fb_, win_w_, win_h_, *font_, w.word, w.x, w.y - scroll_,
                          w.color);
    }

    draw_status();
    draw_scrollbar();
    draw_console();
}

// The status bar: hovered link (or fetch state) on the left, page name on
// the right.  Sits at the bottom of the page area, above the console.
void Shell::draw_status() {
    const int y0 = win_h_ - console_h_ - 20;
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
    if (text.empty()) text = page_.name();
    for (const auto& w : render::shape_words(*font_, text, false, false)) {
        if (x > win_w_ - 80) break;
        render::draw_word(fb_, win_w_, win_h_, *font_, w, x,
                          static_cast<float>(y0) + 4, color);
        x += w.width + 11.5f * 0.30f;
    }
}

} // namespace luxium::shell
