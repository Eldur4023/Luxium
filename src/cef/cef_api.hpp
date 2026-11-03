#pragma once
// The CEF backend lives in libluxium_cef.so, loaded on first use (cef_lazy.cpp):
// a process that never opens a JavaScript page never maps libcef.so (~40 MB of
// private pages just from loading it) and starts no Chromium subprocess.
// This is the table of entry points both sides agree on:
//   X(id, return type, name in cef_backend.hpp, (parameters), (arguments), default result)
// `id` only differs from `name` for overloads.  cef_plugin.cpp fills the table
// from the real implementations; cef_lazy.cpp forwards the public API through it.
#include "cef_backend.hpp"

#define LUXIUM_CEF_API(X) \
    X(set_profile_root, void, set_profile_root, (const std::string& d), (d), void()) \
    X(open_window,  Win*,  open_window,     (const OpenParams& p), (p), nullptr) \
    X(close_window, void,  close_window,    (Win* w), (w), void()) \
    X(destroy_window, void, destroy_window, (Win* w), (w), void()) \
    X(open_devtools, Win*, open_devtools,   (Win* w, const Events& e), (w, e), nullptr) \
    X(set_events,   void,  set_events,      (Win* w, const Events& e), (w, e), void()) \
    X(init,         void,  init,            (unsigned long d, unsigned long p), (d, p), void()) \
    X(attach,       void,  attach,          (unsigned long d, unsigned long p, const std::string& u), (d, p, u), void()) \
    X(navigate,     void,  navigate,        (const std::string& u), (u), void()) \
    X(go_back,      void,  go_back,         (), (), void()) \
    X(go_forward,   void,  go_forward,      (), (), void()) \
    X(reload,       void,  reload,          (), (), void()) \
    X(run_js,       void,  run_js,          (const std::string& c), (c), void()) \
    X(set_view,     void,  set_view,        (int w, int h), (w, h), void()) \
    X(mouse_move,   void,  mouse_move,      (int x, int y, bool l, uint32_t m), (x, y, l, m), void()) \
    X(mouse_button, void,  mouse_button,    (int x, int y, int b, bool u, int c, uint32_t m), (x, y, b, u, c, m), void()) \
    X(mouse_wheel,  void,  mouse_wheel,     (int x, int y, int dx, int dy, uint32_t m), (x, y, dx, dy, m), void()) \
    X(key,          void,  key,             (int t, uint32_t m, uint32_t vk, uint32_t nk, const char* u), (t, m, vk, nk, u), void()) \
    X(set_focus,    void,  set_focus,       (bool f), (f), void()) \
    X(copy_frame,   bool,  copy_frame,      (uint8_t* d, int s, int w, int h, int* fw, int* fh), (d, s, w, h, fw, fh), false) \
    X(cursor,       int,   cursor,          (), (), 0) \
    X(ime_compose,  void,  ime_compose,     (const std::string& u, int c), (u, c), void()) \
    X(ime_commit,   void,  ime_commit,      (const std::string& u), (u), void()) \
    X(edit,         void,  edit,            (char c), (c), void()) \
    X(fullscreen,   bool,  fullscreen,      (), (), false) \
    X(exit_fullscreen, void, exit_fullscreen, (), (), void()) \
    X(paints,       int,   paints,          (), (), 0) \
    X(pop_console,  bool,  pop_console,     (int* l, std::string* t), (l, t), false) \
    X(url,          std::string, url,       (), (), std::string()) \
    X(title,        std::string, title,     (), (), std::string()) \
    X(loaded,       bool,  loaded,          (), (), false) \
    X(navigate_w,   void,  navigate,        (Win* w, const std::string& u), (w, u), void()) \
    X(reload_w,     void,  reload,          (Win* w), (w), void()) \
    X(run_js_w,     void,  run_js,          (Win* w, const std::string& c), (w, c), void()) \
    X(set_view_w,   void,  set_view,        (Win* w, int a, int b), (w, a, b), void()) \
    X(mouse_move_w, void,  mouse_move,      (Win* w, int x, int y, bool l, uint32_t m), (w, x, y, l, m), void()) \
    X(mouse_button_w, void, mouse_button,   (Win* w, int x, int y, int b, bool u, int c, uint32_t m), (w, x, y, b, u, c, m), void()) \
    X(mouse_wheel_w, void, mouse_wheel,     (Win* w, int x, int y, int dx, int dy, uint32_t m), (w, x, y, dx, dy, m), void()) \
    X(key_w,        void,  key,             (Win* w, int t, uint32_t m, uint32_t vk, uint32_t nk, const char* u), (w, t, m, vk, nk, u), void()) \
    X(set_focus_w,  void,  set_focus,       (Win* w, bool f), (w, f), void()) \
    X(copy_frame_w, bool,  copy_frame,      (Win* w, uint8_t* d, int s, int a, int b, int* fw, int* fh), (w, d, s, a, b, fw, fh), false) \
    X(cursor_w,     int,   cursor,          (Win* w), (w), 0) \
    X(ime_compose_w, void, ime_compose,     (Win* w, const std::string& u, int c), (w, u, c), void()) \
    X(ime_commit_w, void,  ime_commit,      (Win* w, const std::string& u), (w, u), void()) \
    X(edit_w,       void,  edit,            (Win* w, char c), (w, c), void()) \
    X(fullscreen_w, bool,  fullscreen,      (Win* w), (w), false) \
    X(exit_fullscreen_w, void, exit_fullscreen, (Win* w), (w), void()) \
    X(paints_w,     int,   paints,          (Win* w), (w), 0) \
    X(url_w,        std::string, url,       (Win* w), (w), std::string()) \
    X(title_w,      std::string, title,     (Win* w), (w), std::string()) \
    X(is_closed,    bool,  is_closed,       (Win* w), (w), true)

namespace luxium::cef {

struct Api {
#define X(id, ret, name, params, args, dflt) ret (*id) params;
    LUXIUM_CEF_API(X)
#undef X
    int (*early)(int argc, char** argv);
    void (*flush_all)();
    void (*shutdown)();
};

} // namespace luxium::cef

extern "C" const luxium::cef::Api* luxium_cef_api();
