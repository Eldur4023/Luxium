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

#include <functional>
#include <memory>
#include <unordered_map>
#include <SDL_events.h>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;
struct SDL_KeyboardEvent; struct SDL_MouseButtonEvent;
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
    // App-window mode (the app host, src/host): no address strip, status strip
    // or handoff to Chromium by itself; the caller owns SDL and the window.
    bool app      = false;
    bool devtools = false;   // app mode: F12 shows the console pane
};

class OwnMediaApi;

// The style values a transition can animate.
struct AnimVals {
    float opacity = 1;
    style::Len tx, ty;
    uint32_t bg = 0, color = 0, border[4] = {0, 0, 0, 0};
};

class Shell {
public:
    explicit Shell(page::Page& page, ShellConfig cfg = {});
    ~Shell();

    // ── App-window mode (cfg.app) ──────────────────────────────────────────
    // The host owns SDL and one SDL window per Shell; it feeds events and calls
    // frame().  Several Shells live in one process.
    void attach(SDL_Window* window);
    bool handle_event(const SDL_Event& e);   // true: the user asked to quit
    void frame();                            // fetches, timers, layout, paint, present
    void set_top_inset(int px);              // rows above the page the host paints (menu bar)
    void invalidate() { need_paint_ = true; }
    // Painted after the page, before the frame is presented (the menu bar and its drop-downs).
    void set_overlay(std::function<void(uint32_t* argb, int w, int h)> f) { overlay_ = std::move(f); }
    // The page asked for JavaScript (a document only Chromium can run): called with the URL.
    void set_web_handler(std::function<void(const std::string& url)> f) { web_handler_ = std::move(f); }
    bool has_page() const { return page_.page_root() != nullptr; }
    std::string page_title() const { return page_.page_title(); }
    const std::string& page_name() const { return page_.name(); }
    void reload() { navigate_to(hist_.empty() ? page_.name() : hist_[hist_idx_], false); }

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
    // --shot only: a script of synthetic input, one step per line:
    //   <ms> click X Y | down X Y | up X Y | move X Y | text STR | key NAME [shift] [ctrl] [alt] | wheel X Y DY
    // (window coordinates; NAME is an SDL key name such as Return, Tab, Left, Backspace, a).
    bool load_shot_script(const std::string& path);
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
    std::unique_ptr<OwnMediaApi> media_api_;   // <video> API for LuxScript (media_glue.cpp)
    dom::Node* fullscreen_el_ = nullptr;       // document.fullscreen(e)
    void wire_page_hooks();                    // clipboard, fullscreen, scroll, reload for scripts
    bool forward_key_to_page(const SDL_Event& e, bool down);   // true: a script prevented it
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
    struct ShotEvent { unsigned at_ms; std::string kind; int a = 0, b = 0, c = 0; std::string s; };
    std::vector<ShotEvent> shot_events_;
    void run_shot_event(const ShotEvent& ev);
    unsigned long cef_display_ = 0, cef_parent_ = 0;
    void    maybe_cef_handoff();
    bool    cef_available() const;
    bool    app_ = false, owns_window_ = true;
    int     top_inset_ = 0;
    bool    need_paint_ = true;
    std::function<void(uint32_t*, int, int)> overlay_;
    std::function<void(const std::string&)>  web_handler_;
    int     status_h() const { return app_ ? 0 : 20; }
    void    open_in_cef(const std::string& url);

    // ── forms and text input in the own engine (shell_forms.cpp) ──
    bool    page_input(const SDL_Event& e);          // true: the event was for a page control
    bool    page_key_down(const SDL_KeyboardEvent& k);
    bool    page_mouse_down(const SDL_MouseButtonEvent& b);
    void    page_mouse_move(int x, int y, bool left_down);
    void    form_frame();                             // text-input on/off, caret blink/scroll, file dialog result
    const render::HitBox* control_hit(const dom::Node* n) const;
    int     control_caret_at(const render::HitBox& h, float doc_x) const;
    float   control_text_width(const render::HitBox& h, const std::string& shown, int byte) const;
    void    open_select_popup(const render::HitBox& h);
    void    close_select_popup() { popup_.node = nullptr; need_paint_ = true; }
    void    draw_select_popup();
    bool    popup_input_key(int sym);
    void    open_file_dialog(dom::Node* input);
    void    update_cursor_shape();
    struct Popup {
        dom::Node* node = nullptr;
        float x = 0, y = 0, w = 0;                    // document coords of the list's top-left
        std::vector<std::string> items;
        int hover = 0;
        float px = 13.333f; int face = 0;
        float item_h = 20;
    } popup_;
    struct FileJob;                                   // zenity running on a worker thread
    std::shared_ptr<FileJob> file_job_;
    bool    wheel_scroll(float dx, float dy);          // scroll the innermost scrollable box under the mouse; false: the page scrolls
    bool    scrollbar_press(float doc_x, float doc_y); // thumb drag / track page-jump of an overflow box
    struct SbDrag { dom::Node* node = nullptr; float mouse0 = 0, scroll0 = 0, track = 0, thumb = 0, max = 0; } sb_drag_;
    void    fire_scroll(dom::Node* n);
    void    apply_sticky();                            // shift sticky groups (and their hit boxes) for the current scroll
    std::vector<float> sticky_dy_;
    // transitions (shell_anim.cpp): the style hook compares each element's target style with the previous layout's
    // and, when `transition` covers the change, feeds interpolated values back until the tween ends.
    using Vals = AnimVals;
    struct NodeAnim {
        bool has_target = false, active = false;
        Vals target, from;
        float t0 = 0, dur = 0, delay = 0, bez[4] = {0.25f, 0.1f, 0.25f, 1.f};
        unsigned groups = 0;
        uint64_t seen = 0;
    };
    std::unordered_map<const dom::Node*, NodeAnim> anims_;
    uint64_t anim_frame_ = 0;
    bool    anim_active_ = false;
    void    style_hook(const dom::Node& el, style::ComputedStyle& s);
    void    wire_form_hooks();                         // ShellHooks: rect, scroll, focus, live values, files
    const render::HitBox* box_of(const dom::Node* n) const;
    bool    key_dispatched_ = false;                   // page_key_down already delivered this keydown to scripts
    bool    text_input_on_ = false;
    bool    form_drag_ = false;                       // selecting text in a control with the mouse
    uint32_t caret_tick_ = 0;
    int     cursor_kind_ = 0;
    SDL_Cursor* cursor_text_ = nullptr; SDL_Cursor* cursor_no_ = nullptr; SDL_Cursor* cursor_move_ = nullptr;
};

} // namespace luxium::shell
