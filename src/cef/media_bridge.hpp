#pragma once
// The renderer-side half of the media bridge: it gives every page frame a
// native object, `__luxiumMedia`, backed by luxium::media::Player instances
// that live in the renderer process, and injects src/media/shim.js before the
// page's own scripts.  See docs/MEDIA.md for why the players live here and
// not in the browser process.

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_v8.h"

namespace luxium::cef::bridge {

void on_context_created(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefV8Context>);
void on_context_released(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefV8Context>);

} // namespace luxium::cef::bridge
