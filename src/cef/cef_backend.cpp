// CEF backend, real implementation (windowless / OSR).  See cef_backend.hpp.
//
// Chromium renders off-screen: it creates NO X11 window (a native child is
// impossible here -- SDL's window uses visual 0x3ba, Chromium insists on
// the default 0x21, and equal-depth windows must share the parent's
// visual, so every CreateWindow ended in BadMatch).  Instead OnPaint
// delivers BGRA pixels every frame and we blit them into our framebuffer;
// input comes from our SDL loop through the CefBrowserHost API.

#include "cef_backend.hpp"

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_process_handler.h"

#include "media_bridge.hpp"

#include <X11/Xlib.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <unistd.h>
#include <vector>

namespace luxium::cef {

// ── handoff state (CEF UI thread <-> our frame loop) ────────────────────────
namespace {
std::mutex              g_mutex;
std::string             g_url, g_title;
bool                    g_loaded = false;
CefRefPtr<CefBrowser>   g_browser;
unsigned long           g_display = 0, g_parent = 0;
std::atomic<int>        g_view_w{1}, g_view_h{1};
std::atomic<int>        g_cursor_type{0};
std::atomic<int>        g_paints{0};

std::mutex              g_paint_mutex;      // guards g_pixels/g_pw/g_ph
std::vector<uint8_t>    g_pixels;           // BGRA, g_pw * g_ph * 4
int                     g_pw = 0, g_ph = 0;

std::deque<std::pair<int, std::string>> g_console;   // under g_mutex

// traza OSR (LUXIUM_DEBUG_CEF=1): cuenta GetViewRect / OnPaint / copy_frame
static bool dbg() {
    static const bool d = std::getenv("LUXIUM_DEBUG_CEF") != nullptr;
    return d;
}
static void dbgf(const char* fmt, ...) {
    if (!dbg()) return;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[osr] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

uint32_t mods_of(bool shift, bool ctrl, bool alt, bool cmd,
                 bool caps, bool num, bool repeat) {
    uint32_t m = 0;
    if (shift) m |= 1;         // EVENTFLAG_SHIFT_DOWN
    if (ctrl)  m |= 2;         // EVENTFLAG_CONTROL_DOWN
    if (alt)   m |= 4;         // EVENTFLAG_ALT_DOWN
    if (cmd)   m |= 8;         // EVENTFLAG_COMMAND_DOWN
    if (caps)  m |= 16;        // EVENTFLAG_CAPS_LOCK_ON
    if (num)   m |= 32;        // EVENTFLAG_NUM_LOCK_ON
    if (repeat) m |= 1024;     // EVENTFLAG_IS_REPEAT
    return m;
}

class LuxClient : public CefClient,
                  public CefLifeSpanHandler,
                  public CefDisplayHandler,
                  public CefLoadHandler,
                  public CefRenderHandler {
public:
    // CefClient
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler()  override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler()     override { return this; }
    CefRefPtr<CefRenderHandler>   GetRenderHandler()   override { return this; }

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
        dbgf("OnAfterCreated pid=%d", (int)b->GetIdentifier());
        std::lock_guard<std::mutex> lk(g_mutex);
        g_browser = b;
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_browser = nullptr;
    }

    // CefDisplayHandler
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                         const CefString& url) override {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_url = url;
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_title = title;
    }
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                        cef_cursor_type_t type, const CefCursorInfo&) override {
        g_cursor_type = (int)type;
        return true;
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                          const CefString& message, const CefString&,
                          int) override {
        int lv = 0;
        if (level == LOGSEVERITY_WARNING)     lv = 1;
        else if (level >= LOGSEVERITY_ERROR)  lv = 2;
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_console.size() < 200)
            g_console.emplace_back(lv, message.ToString());
        return true;   // ya lo mostramos nosotros en nuestro panel
    }

    // CefLoadHandler
    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                     TransitionType) override {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_loaded = false;
    }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int) override {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_loaded = true;
    }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        const int w = std::max(1, g_view_w.load());
        const int h = std::max(1, g_view_h.load());
        {   // traza rara (una de cada 300) para ver el tamano pedido
            static std::atomic<int> n{0};
            int k = n++;
            if (k < 5 || k % 300 == 0)
                dbgf("GetViewRect #%d -> %dx%d", k, w, h);
        }
        rect = CefRect(0, 0, w, h);
    }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                 const RectList&, const void* buffer,
                 int width, int height) override {
        {   // traza: las primeras 5 + una cada 300
            static std::atomic<int> n{0};
            int k = n++;
            if (k < 5 || k % 300 == 0)
                dbgf("OnPaint #%d type=%d %dx%d", k, (int)type, width, height);
        }
        if (type != PET_VIEW || width <= 0 || height <= 0) return;
        std::lock_guard<std::mutex> lk(g_paint_mutex);
        g_pixels.resize((size_t)width * height * 4);
        std::memcpy(g_pixels.data(), buffer, g_pixels.size());
        g_pw = width;
        g_ph = height;
        ++g_paints;
    }

private:
    IMPLEMENT_REFCOUNTING(LuxClient);
};

class LuxApp : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    // Every page frame gets the media shim and its native engine bridge.
    void OnContextCreated(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, CefRefPtr<CefV8Context> c) override {
        bridge::on_context_created(b, f, c);
    }
    void OnContextReleased(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, CefRefPtr<CefV8Context> c) override {
        bridge::on_context_released(b, f, c);
    }

    // GPU policy: LUXIUM_GPU=0 forces software rendering (no GPU at all, no
    // WebGL); anything else lets Chromium use the GPU, falling back to
    // SwiftShader.  LUXIUM_CEF_FLAGS="--a --b=c" appends raw switches.
    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> cl) override {
        const char* gpu = std::getenv("LUXIUM_GPU");
        if (gpu && gpu[0] == '0') {
            cl->AppendSwitch("disable-gpu");
            cl->AppendSwitch("disable-gpu-compositing");
        }
        if (const char* extra = std::getenv("LUXIUM_CEF_FLAGS")) {
            std::string s(extra);
            for (size_t i = 0; i < s.size();) {
                size_t j = s.find(' ', i);
                if (j == std::string::npos) j = s.size();
                std::string tok = s.substr(i, j - i);
                i = j + 1;
                if (tok.rfind("--", 0) != 0) continue;
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) cl->AppendSwitch(tok.substr(2));
                else cl->AppendSwitchWithValue(tok.substr(2, eq - 2), tok.substr(eq + 1));
            }
        }
    }
    IMPLEMENT_REFCOUNTING(LuxApp);
};

CefRefPtr<LuxApp>& app() {
    static CefRefPtr<LuxApp> a = new LuxApp();
    return a;
}
CefRefPtr<LuxClient>& client() {
    static CefRefPtr<LuxClient> cl = new LuxClient();
    return cl;
}

CefRefPtr<CefBrowser> browser() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_browser;
}

CefMouseEvent mouse_event(int x, int y, uint32_t mods) {
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    ev.modifiers = mods;
    return ev;
}

// Decodes the first codepoint of a UTF-8 string (0 on failure).
char32_t first_cp(const char* s) {
    if (!s || !*s) return 0;
    const unsigned char* p = (const unsigned char*)s;
    if (p[0] < 0x80) return p[0];
    int extra = 0;
    char32_t cp = 0;
    if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else return 0xFFFD;
    for (int i = 1; i <= extra && p[i]; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    return cp;
}

} // namespace

bool built() { return true; }

static int g_argc = 0;
static char** g_argv = nullptr;

int early(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    CefMainArgs args(argc, argv);
    return CefExecuteProcess(args, app(), nullptr);   // -1 = browser process
}

void init(unsigned long display, unsigned long parent) {
    static bool done = false;
    if (done) return;
    done = true;
    g_display = display;
    g_parent  = parent;

    CefMainArgs args(g_argc, g_argv);
    CefSettings settings;
    settings.multi_threaded_message_loop = true;   // CEF's own UI thread
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;  // OSR: no X windows of its own
    char exe[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        CefString(&settings.browser_subprocess_path) = std::string(exe);
    }
    // resources sit next to the binary (copied by CMake)
    std::string dir(exe);
    size_t sl = dir.find_last_of('/');
    dir = (sl == std::string::npos) ? std::string(".") : dir.substr(0, sl);
    CefString(&settings.resources_dir_path) = dir;
    CefString(&settings.locales_dir_path)   = dir + "/locales";
    CefString(&settings.log_file)           = dir + "/cef.log";
    settings.log_severity                   = LOGSEVERITY_INFO;
    if (const char* port = std::getenv("LUXIUM_CDP_PORT"))   // DevTools protocol, for tests/automation
        settings.remote_debugging_port = std::atoi(port);

    CefInitialize(args, settings, app(), nullptr);
}

void attach(unsigned long display, unsigned long parent, const std::string& url) {
    init(display, parent);
    if (!browser()) {
        CefWindowInfo info;
        info.SetAsWindowless((::Window)parent);   // parent: dialogs/IME only
        CefBrowserSettings bs;
        bs.background_color = 0xFFFFFFFF;         // opaco: blanco antes del 1er paint
        bs.windowless_frame_rate = 60;            // video and animations: 30 is the default
        CefBrowserHost::CreateBrowser(info, client(), url, bs, nullptr, nullptr);
        return;
    }
    navigate(url);
}

void navigate(const std::string& url) {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetMainFrame()->LoadURL(CefString(url));
}

void go_back()    { CefRefPtr<CefBrowser> b = browser(); if (b) b->GoBack(); }
void go_forward() { CefRefPtr<CefBrowser> b = browser(); if (b) b->GoForward(); }
void reload()     { CefRefPtr<CefBrowser> b = browser(); if (b) b->Reload(); }

void run_js(const std::string& code) {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetMainFrame()->ExecuteJavaScript(CefString(code), CefString("luxium://probe"), 0);
}

void set_view(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    // dos exchanges SEPARADOS: con && cortocircuitaria y la altura no se
    // actualizaria nunca (g_view_h se quedaria en 1 -> pintado de 1px).
    const int ow = g_view_w.exchange(w);
    const int oh = g_view_h.exchange(h);
    const bool same = (ow == w && oh == h);
    CefRefPtr<CefBrowser> b = browser();
    dbgf("set_view %dx%d same=%d browser=%d", w, h, (int)same, b ? 1 : 0);
    if (same) return;
    if (b) b->GetHost()->WasResized();
}

// ── input forwarding (our SDL loop -> Chromium) ─────────────────────────────

void mouse_move(int x, int y, bool leave, uint32_t mods) {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetHost()->SendMouseMoveEvent(mouse_event(x, y, mods), leave);
}

void mouse_button(int x, int y, int button, bool up, int clicks, uint32_t mods) {
    CefRefPtr<CefBrowser> b = browser();
    if (!b) return;
    b->GetHost()->SendMouseClickEvent(mouse_event(x, y, mods),
                                      (cef_mouse_button_type_t)button, up,
                                      std::max(1, clicks));
}

void mouse_wheel(int x, int y, int dx, int dy, uint32_t mods) {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetHost()->SendMouseWheelEvent(mouse_event(x, y, mods), dx, dy);
}

void key(int type, uint32_t mods, uint32_t windows_vk, uint32_t native_key,
         const char* utf8) {
    CefRefPtr<CefBrowser> b = browser();
    if (!b) return;
    CefKeyEvent e;
    e.modifiers = mods;
    e.native_key_code = native_key;
    switch (type) {
    case 0:  e.type = KEYEVENT_RAWKEYDOWN; e.windows_key_code = windows_vk; break;
    case 1:  e.type = KEYEVENT_KEYUP;      e.windows_key_code = windows_vk; break;
    default: {
        e.type = KEYEVENT_CHAR;
        char32_t cp = first_cp(utf8);
        e.character = (char16_t)cp;
        e.windows_key_code = windows_vk ? windows_vk : (uint32_t)cp;
        break;
    }
    }
    b->GetHost()->SendKeyEvent(e);
}

void set_focus(bool focused) {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetHost()->SetFocus(focused);
}

// ── frames (CEF UI thread -> our framebuffer) ───────────────────────────────

bool copy_frame(uint8_t* dst, int dst_stride, int dst_w, int dst_h,
                int* frame_w, int* frame_h) {
    std::lock_guard<std::mutex> lk(g_paint_mutex);
    if (g_pixels.empty() || g_pw <= 0 || g_ph <= 0) {
        static std::atomic<int> n{0};
        int k = n++;
        if (k < 5 || k % 600 == 0)
            dbgf("copy_frame vacio #%d (pw=%d ph=%d)", k, g_pw, g_ph);
        return false;
    }
    {
        static bool once = false;
        if (!once) { once = true; dbgf("copy_frame PRIMERO %dx%d", g_pw, g_ph); }
    }
    if (frame_w) *frame_w = g_pw;
    if (frame_h) *frame_h = g_ph;
    const int rows = std::min(dst_h, g_ph);
    const int cols = std::min(dst_w, g_pw) * 4;
    for (int r = 0; r < rows; ++r)
        std::memcpy(dst + (size_t)r * dst_stride,
                    g_pixels.data() + (size_t)r * g_pw * 4, cols);
    return true;
}

int cursor() { return g_cursor_type.load(); }

int paints() { return g_paints.load(); }

bool pop_console(int* level, std::string* text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_console.empty()) return false;
    if (level) *level = g_console.front().first;
    if (text)  *text  = std::move(g_console.front().second);
    g_console.pop_front();
    return true;
}

std::string url()    { std::lock_guard<std::mutex> lk(g_mutex); return g_url; }
std::string title()  { std::lock_guard<std::mutex> lk(g_mutex); return g_title; }
bool loaded()        { std::lock_guard<std::mutex> lk(g_mutex); return g_loaded; }

void shutdown() {
    {
        CefRefPtr<CefBrowser> b = browser();
        if (b) b->GetHost()->CloseBrowser(true);
    }
    CefShutdown();
}

} // namespace luxium::cef
