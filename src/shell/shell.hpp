#pragma once
// The shell: an SDL2 window, an ARGB software framebuffer, and the event
// loop that drives a Page.  Software rendering on purpose (ARCHITECTURE §6):
// the demo repaints a few hundred words at 60fps worst case, and every pixel
// is one we understand.
//
// Layout of the window:
//   ┌──────────────────────────────┐
//   │  page (scrollable)           │
//   │                              │
//   ├──────────────────────────────┤
//   │  console pane (LuxScript)    │  30% of the height, min 140px
//   └──────────────────────────────┘

#include <page/page.hpp>
#include <render/layout.hpp>
#include <render/text.hpp>

#include <memory>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace luxium::shell {

struct ShellConfig {
    int  width      = 1100;
    int  height     = 780;
    std::string font_regular = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    std::string font_bold    = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    std::string title        = "Luxium";
};

class Shell {
public:
    explicit Shell(page::Page& page, ShellConfig cfg = {});
    ~Shell();

    // Opens the window and runs until closed.  Returns the process exit code.
    int run();

    // One frame without a display (SDL_VIDEODRIVER=dummy), then the
    // framebuffer goes to a PPM.  This is how the render is tested without
    // stealing anyone's screen.
    int screenshot(const std::string& ppm_path);

private:
    void ensure_surface();
    void relayout();                    // DOM -> boxes (page dirty or resize)
    void repaint();                     // boxes -> framebuffer -> screen
    void draw_console();
    float page_h() const;
    void draw_scrollbar();
    bool handle_click(int mx, int my);  // page coords -> hit test -> page

    page::Page&    page_;
    ShellConfig    cfg_;
    std::unique_ptr<render::Font> font_;

    SDL_Window*    window_ = nullptr;
    SDL_Renderer*  renderer_ = nullptr;
    SDL_Texture*   texture_ = nullptr;
    std::vector<uint32_t> fb_;         // ARGB, window-sized

    render::LayoutResult layout_;
    float  scroll_  = 0;
    float  content_h_ = 0;
    int    win_w_ = 0, win_h_ = 0;
    int    console_h_ = 180;
    bool   console_scroll_bottom_ = true;
    float  console_scroll_ = 0;
};

} // namespace luxium::shell
