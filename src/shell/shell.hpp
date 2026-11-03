#pragma once
// The shell: an SDL2 window, an ARGB software framebuffer, and the event
// loop that drives a Page.  Software rendering on purpose (ARCHITECTURE §6).
//
// Layout of the window:
//   ┌──────────────────────────────┐
//   │  page (scrollable)           │
//   │                              │
//   ├──────────────────────────────┤
//   │  status bar (links, cargando)│  20px
//   ├──────────────────────────────┤
//   │  console pane (LuxScript)    │
//   └──────────────────────────────┘
//
// Navigation: clicks on <a href> come back through Page::on_navigate into
// navigate_to(), which resolves them (file relative, file://, http(s)://,
// #anchor), keeps history (back/forward/reload), and fetches network pages
// on a worker thread (net/fetch).

#include <page/page.hpp>
#include <render/layout.hpp>
#include <render/text.hpp>

#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;
typedef int SDL_Keycode;   // sdl.h defines it; the shell only stores keys it handles

namespace luxium::net {
class Fetch;
}

namespace luxium::shell {

constexpr int kUrlH = 38;              // the address strip's height
constexpr float kUrlTextX = 82.f;      // text origin in the field (8 + 74)

struct ShellConfig {
    int  width      = 1180;
    int  height     = 820;
    std::string font_regular = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    std::string font_bold    = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    std::string font_mono    = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
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
    // Rects (data-t) del layout propio / de la sonda de Chromium, a JSON.
    void set_boxes_out(const std::string& path) { boxes_path_ = path; }
    void set_probe_out(const std::string& path) { probe_path_ = path; }
    // --shot only: window-coordinate clicks (control bars) and a wait while media plays.
    void add_shot_click(int x, int y, unsigned at_ms) { shot_clicks_.push_back({x, y, at_ms}); }
    void set_shot_wait(unsigned ms) { shot_wait_ms_ = ms; }
    // El oraculo: Chromium (CEF off-screen) pinta la pagina en nuestro
    // framebuffer; para generar los goldens de css-tests.
    int cef_shot(const std::string& target, const std::string& ppm_path);

    // Navigates (file path, http(s) URL, #anchor, luxium:inicio).  Public
    // because main() starts URL arguments through here.
    void navigate_to(const std::string& href, bool push_history);

    // Navigates to whatever a human typed in the URL bar: files that exist,
    // domains, "luxium:inicio"...
    void navigate_address(std::string addr, bool push_history);
    void load_home();

private:
    void poll_fetch();                  // worker thread -> page, once per frame
    void after_load();
    void tick_media();                  // pump the own engine's <video>/<audio> players
    void toggle_view_source();
    bool view_source_on() const { return viewing_source_; }


    // Where an href points, once resolved against the current page.
    enum class Target { File, Http, Anchor };
    struct Resolved {
        Target     kind = Target::File;
        std::string target;    // file path, URL, or anchor id
    };

    void ensure_surface();
    int  bottom_h() const;      // 20px status strip + console pane when visible
    int  url_h() const;         // the address strip, 0 while a page is fullscreen
    void set_fullscreen(bool on);
    void update_cef_cursor();   // Chromium's cursor type -> SDL cursor
    int  cef_cursor_ = -1;      // last Chromium cursor type applied
    void toggle_console();      // F12
    void relayout();                    // DOM -> boxes (page dirty or resize)
    void repaint();                     // boxes -> framebuffer -> screen
    void draw_console();
    void draw_status();
    void draw_scrollbar();
    bool handle_click(int mx, int my, int clicks = 1);  // page coords -> hit test
    float page_h() const;

    // Navigation (M5).
    Resolved resolve(const std::string& href) const;

    page::Page&    page_;
    ShellConfig    cfg_;
    std::unique_ptr<render::Font> font_;

    SDL_Window*    window_ = nullptr;
    SDL_Renderer*  renderer_ = nullptr;
    SDL_Texture*   texture_ = nullptr;
    SDL_Cursor*    cursor_hand_ = nullptr;
    std::vector<uint32_t> fb_;         // ARGB, window-sized

    render::LayoutResult layout_;
    float  scroll_  = 0;
    float  content_h_ = 0;
    int    win_w_ = 0, win_h_ = 0;
    int    console_h_ = 180;
    bool   fullscreen_ = false;        // the page (CEF) asked for element fullscreen
    bool   console_visible_ = false;   // F12 shows/hides the console pane
    bool   console_scroll_bottom_ = true;
    float  console_scroll_ = 0;

    int    mouse_x_ = 0, mouse_y_ = 0;
    bool   cursor_over_clickable_ = false;
    const dom::Node* hover_node_ = nullptr;   // deepest element under the mouse (:hover)
    void update_hover();

    // The URL bar (top strip, always visible).
    std::string url_edit_;
    bool        url_focus_ = false;
    int         url_caret_ = 0;      // caret position in BYTES (utf-8)
    int         url_anchor_ = 0;     // selection anchor (== caret = no selection)
    float       url_scroll_ = 0;     // horizontal scroll of the field text
    bool        url_drag_ = false;   // dragging a selection with the mouse
    uint64_t    frames_ = 0;         // caret blink
    void        draw_url_bar();
    // url-bar editing (caret is byte-based, utf-8 aware)
    int  url_caret_at(int mx) const;           // window x -> byte position
    void url_insert(const std::string& utf8);  // insert at caret
    bool url_delete_selection();               // false when nothing selected
    void url_copy() const;                     // Ctrl+C in the bar
    void url_paste();                          // Ctrl+V in the bar

    // Text selection over the page's words (indices into layout_.words).
    int    sel_start_ = -1, sel_end_ = -1;   // inclusive word range, -1 = none
    bool   sel_dragging_ = false;
    int    sel_anchor_ = -1;
    void   selection_clear();    void   selection_word_at(float doc_x, float doc_y);   // double-click
    int    word_index_at(float doc_x, float doc_y) const;
    void   selection_extend(float doc_x, float doc_y);
    void   copy_selection() const;
    std::string selection_text() const;
    

    // history + network
    std::vector<std::string> hist_;
    size_t  hist_idx_ = 0;
    bool    navigating_from_history_ = false;
    std::unique_ptr<net::Fetch> fetch_;
    bool    viewing_source_ = false;
    std::string viewing_source_origin_;
    bool    engine_cef_ = false;   // la pagina actual la renderiza Chromium
    std::string boxes_path_, probe_path_;
    struct ShotClick { int x, y; unsigned at_ms; };
    std::vector<ShotClick> shot_clicks_;
    unsigned shot_wait_ms_ = 0;
    unsigned long cef_display_ = 0, cef_parent_ = 0;
    void    maybe_cef_handoff();
    bool    cef_available() const;
    void    open_in_cef(const std::string& url);
};

} // namespace luxium::shell
