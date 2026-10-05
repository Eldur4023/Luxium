#pragma once
// The CEF backend: Chromium renders pages OFF-SCREEN (windowless) and hands
// us BGRA pixels every frame; we blit them into our framebuffer inside the
// page area.  Why not a native child window: on X11 a child must share its
// parent's visual when depths match, SDL picks visual 0x3ba while Chromium
// hardcodes the default 0x21 -- every CreateWindow ended in BadMatch.
// With OSR Chromium creates no X11 window at all.  Input (mouse, wheel,
// keys, focus) is forwarded from our SDL event loop via the CefBrowserHost
// API, so we keep full control of the chrome around the page.
//
// Our own engine keeps rendering every non-JavaScript page (file,
// luxium:inicio).  Built only when third_party/cef exists (LUXIUM_CEF);
// without it, every function here degrades to a no-op and http falls back
// to our engine+curl.

#include <cstdint>
#include <string>

namespace luxium::cef {

// Was this binary built with CEF?
bool built();

// Subprocess split: call FIRST in main, before anything else.  Returns -1 to
// continue as the browser process; otherwise the process must exit with the
// returned code.
int early(int argc, char** argv);

// Initializes Chromium (multi-threaded message loop).  display/parent are
// the X11 Display*/Window of the SDL window (used for dialogs and key
// translation).  Idempotent.
void init(unsigned long display, unsigned long parent);

// Creates (once) the offscreen browser and loads url; later calls just
// navigate.  The view size comes from set_view().
void attach(unsigned long display, unsigned long parent, const std::string& url);

void navigate(const std::string& url);
void go_back();
void go_forward();
void reload();

// Runs JavaScript in the main frame (the oracle's probe; pages of Luxium
// itself never reach here).
void run_js(const std::string& code);

// Page-area size in pixels; Chromium re-measures afterwards.
void set_view(int w, int h);

// Input forwarding.  Coordinates are VIEW-relative (0,0 = top-left of the
// page area).  mods is a OR of the EVENTFLAG_* bits (1 shift, 2 ctrl,
// 4 alt, 8 cmd, 16 caps, 32 num, 1024 repeat).
void mouse_move(int x, int y, bool leave, uint32_t mods);
void mouse_button(int x, int y, int button, bool up, int clicks, uint32_t mods);
void mouse_wheel(int x, int y, int dx, int dy, uint32_t mods);
// type: 0 = raw key down, 1 = key up, 2 = char (utf8 text).
void key(int type, uint32_t mods, uint32_t windows_vk, uint32_t native_key,
         const char* utf8);
void set_focus(bool focused);

// Latest painted frame (BGRA, tightly packed width*height*4).  Copies the
// intersection of the frame with a dst_w x dst_h area into dst using
// dst_stride bytes per row.  Returns false when nothing was painted yet.
bool copy_frame(uint8_t* dst, int dst_stride, int dst_w, int dst_h,
                int* frame_w, int* frame_h);

// Current cursor type (cef_cursor_type_t: 0 pointer, 2 hand, 3 ibeam...).
int cursor();

// Cuantos frames ha entregado OnPaint (para esperas estables del oraculo).
int paints();

// Pops the next console message Chromium emitted (console.log/warn/error).
// level: 0 info/debug, 1 warning, 2 error.  Returns false when the queue
// is empty.  Drained once per frame by the shell and shown in our console.
bool pop_console(int* level, std::string* text);

// Handoff state (written by CEF's UI thread, read by ours each frame).
std::string url();      // last known address
std::string title();    // last known title
bool loaded();

void shutdown();

} // namespace luxium::cef
