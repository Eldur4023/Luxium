#include "shell/shell.hpp"

#include <SDL.h>

#include <algorithm>
#include <utility>

#include <cstdio>

namespace luxium::shell {

using render::DrawCmd;

Shell::Shell(page::Page& page, ShellConfig cfg) : page_(page), cfg_(std::move(cfg)) {}

Shell::~Shell() {
    if (texture_)  SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_)   SDL_DestroyWindow(window_);
    SDL_Quit();
}

int Shell::screenshot(const std::string& ppm_path) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    win_w_ = cfg_.width;
    win_h_ = cfg_.height;
    console_h_ = std::max(120, win_h_ * 28 / 100);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window_ = SDL_CreateWindow("", 0, 0, win_w_, win_h_, 0);
    renderer_ = SDL_CreateRenderer(window_, -1, 0);
    font_ = render::Font::load(cfg_.font_regular, cfg_.font_bold);
    if (!font_) return 1;
    fb_.assign(static_cast<size_t>(win_w_) * win_h_, 0xFFFFFFFFu);
    relayout();
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
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);

    font_ = render::Font::load(cfg_.font_regular, cfg_.font_bold);
    if (!font_) {
        std::fprintf(stderr, "cannot load the UI fonts:\n  %s\n  %s\n",
                     cfg_.font_regular.c_str(), cfg_.font_bold.c_str());
        return 1;
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
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GetWindowSize(window_, &win_w_, &win_h_);
                    console_h_ = std::max(120, win_h_ * 28 / 100);
                    fb_.assign(static_cast<size_t>(win_w_) * win_h_, 0xFFFFFFFFu);
                    if (texture_) SDL_DestroyTexture(texture_);
                    texture_ = SDL_CreateTexture(renderer_,
                                                 SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 win_w_, win_h_);
                    scroll_ = std::min(scroll_, std::max(0.f, content_h_ - page_h()));
                    relayout();
                }
                break;
            case SDL_MOUSEWHEEL: {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                const float lines = static_cast<float>(-e.wheel.y) * 3;
                if (my > win_h_ - console_h_) {
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
                break;
            case SDL_KEYDOWN:
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

        if (page_.dirty()) {
            relayout();
            // A script may have logged: keep the pane pinned to the tail
            // unless the user scrolled it up.
            console_scroll_ = 0;
        }
        repaint();

        SDL_UpdateTexture(texture_, nullptr, fb_.data(), win_w_ * 4);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
        SDL_Delay(8);   // ~120fps cap; the CPU is idle at this size anyway
    }
    return 0;
}

float Shell::page_h() const { return static_cast<float>(win_h_ - console_h_); }

void Shell::relayout() {
    if (win_w_ == 0) return;
    page_.clear_dirty();
    layout_ = render::layout(*page_.page_root(), *font_,
                             static_cast<float>(win_w_));
    content_h_ = layout_.content_height;
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, content_h_ - page_h()));
    if (std::getenv("LUXIUM_DEBUG_LAYOUT"))
        for (const auto& d : layout_.draw)
            std::printf("cmd %s y=%.0f x=%.0f '%s'\n",
                        d.kind == render::DrawCmd::Kind::Rect ? "RECT" : "WORD",
                        d.y, d.x, d.kind == render::DrawCmd::Kind::Word ? d.word.text.c_str() : "");
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
    const float bar_h = std::max(30.f, page * page / content_h_);
    const float bar_y = scroll_ / (content_h_ - page) * (page - bar_h);
    const int   x     = win_w_ - 6;
    for (float yy = bar_y; yy < bar_y + bar_h && yy < page; yy += 1)
        for (int xx = x; xx < x + 4; ++xx)
            fb_[static_cast<size_t>(yy) * win_w_ + xx] = 0xFFB0B8C0;
}

void Shell::draw_console() {
    const int y0 = win_h_ - console_h_;
    // background
    for (int y = y0; y < win_h_; ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFF14181C;
    }
    // separator
    for (int x = 0; x < win_w_; ++x) fb_[static_cast<size_t>(y0) * win_w_ + x] = 0xFF3A4048;

    font_->set_size(13);
    const float lh = font_->line_height() + 2;
    const auto& lines = page_.console_lines();

    // Show the tail; wheel scrolling moves console_scroll_ back (0 = tail).
    int visible = static_cast<int>((console_h_ - 8) / lh);
    int end = static_cast<int>(lines.size()) - static_cast<int>(console_scroll_ / lh);
    int begin = std::max(0, end - visible);

    float y = static_cast<float>(y0) + 6;
    for (int i = begin; i < end && y < win_h_ - 4; ++i, y += lh) {
        const auto& l = lines[static_cast<size_t>(i)];
        const uint32_t color = l.level == 2 ? 0xFFFF7060
                             : l.level == 1 ? 0xFFFFD070
                                            : 0xFFB8C0C8;
        const char* prefix = l.level == 2 ? "\xE2\x9C\x95 " : l.level == 1 ? "! " : "\xC2\xBB ";
        float x = 10;
        for (const auto& w : render::shape_words(*font_, prefix + l.text, false)) {
            render::draw_word(fb_, win_w_, win_h_, *font_, w, x, y, color);
            x += w.width + 13 * 0.28f;
        }
    }
}

void Shell::repaint() {
    if (win_w_ == 0 || !page_.page_root()) return;
    // page background
    for (int y = 0; y < std::min(win_h_, win_h_ - console_h_); ++y) {
        uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
        for (int x = 0; x < win_w_; ++x) row[x] = 0xFFFCFCFA;
    }

    // rects first (button boxes, rules), then words -- see DrawCmd's note.
    for (const auto& d : layout_.draw) {
        if (d.kind != DrawCmd::Kind::Rect) continue;
        const float dx = d.x, dy = d.y - scroll_;
        const int x0 = std::max(0, static_cast<int>(dx));
        const int y0 = std::max(0, static_cast<int>(dy));
        const int x1 = std::min(win_w_, static_cast<int>(dx + d.w));
        const int y1 = std::min(win_h_ - console_h_, static_cast<int>(dy + d.h));
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = fb_.data() + static_cast<size_t>(y) * win_w_;
            for (int x = x0; x < x1; ++x) row[x] = d.argb;
        }
    }
    for (const auto& d : layout_.draw) {
        if (d.kind != DrawCmd::Kind::Word) continue;
        if (d.y - scroll_ > page_h() || d.y + d.h - scroll_ < 0) continue;
        render::draw_word(fb_, win_w_, win_h_, *font_, d.word, d.x, d.y - scroll_,
                          d.word.bold ? 0xFF101418 : 0xFF202020);
    }

    draw_scrollbar();
    draw_console();
}

} // namespace luxium::shell
