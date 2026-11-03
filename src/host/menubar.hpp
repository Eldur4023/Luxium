#pragma once
// Menu bar of an app-host window (set_menu).  SDL has no native menus and the page is drawn off-screen into
// our own framebuffer, so the bar and its drop-downs are drawn here, over that framebuffer, the same way the
// classic shell draws its chrome.  Docs: docs/HOST.md.
#include <SDL.h>
#include <cstdint>
#include <string>
#include <vector>

namespace luxium::render { class Font; }

namespace luxium::host {

struct MenuItem {
    std::string label, action, accel_text;
    bool sep = false, checkable = false, checked = false;
    SDL_Keycode key = 0;    // accelerator, 0 = none
    unsigned mods = 0;      // KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI subset
    std::vector<MenuItem> items;
    bool sub() const { return !items.empty(); }
};

// A leaf was activated (click, Enter or accelerator).
struct MenuFire {
    bool fired = false;
    std::string action;
    bool checkable = false, checked = false;   // checked = the NEW state
};

class MenuBar {
public:
    static constexpr int kBarH = 26;

    explicit MenuBar(const render::Font* font) : font_(font) {}

    // Replaces the menu from a JSON list (window.set_menu() spec).  False if it does not parse; an empty list
    // is valid and removes the bar.
    bool set_spec(const std::string& json);
    bool empty() const { return top_.empty(); }
    void set_dark(bool d) { dark_ = d; dirty_ = true; }

    // All return true when the event belongs to the menu (the page must not see it).
    bool on_motion(int x, int y, int win_w);
    bool on_button(int x, int y, bool down, int win_w, MenuFire& f);
    bool on_key(const SDL_Keysym& k, bool repeat, MenuFire& f);
    void on_leave() { if (hover_top_ != -1) { hover_top_ = -1; dirty_ = true; } }

    bool open() const { return !path_.empty(); }
    bool take_dirty() { bool d = dirty_; dirty_ = false; return d; }
    // Paints the bar and the open drop-downs over a win_w x win_h ARGB framebuffer.
    void draw(uint32_t* fb, int win_w, int win_h);

private:
    struct Panel { const std::vector<MenuItem>* items; int x, y, w, h; std::vector<int> rowy; };
    const render::Font* font_;
    std::vector<MenuItem> top_;
    std::vector<int> topx_, topw_;      // bar slots (computed by layout)
    std::vector<int> path_;             // [0] open top menu, [k] highlighted row of panel k-1
    int hover_top_ = -1;
    bool dark_ = false, dirty_ = true, swallow_up_ = false;

    float text_w(const std::string& s) const;
    void layout_top();
    std::vector<Panel> panels(int win_w) const;
    MenuItem* item_at(const std::vector<int>& p, size_t depth);   // panel `depth`'s highlighted item
    void activate(MenuItem& it, MenuFire& f);
    void close() { path_.clear(); dirty_ = true; }
    bool step(int dir);
};

} // namespace luxium::host
