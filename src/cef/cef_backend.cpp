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
#include "include/cef_dialog_handler.h"
#include "include/cef_jsdialog_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_permission_handler.h"

#include "media_bridge.hpp"

#include <X11/Xlib.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
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
std::atomic<bool>       g_fullscreen{false};
float                   g_scale = 1.0f;     // HiDPI factor, applied as page zoom (see OnLoadEnd)

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

// Runs a program and captures its stdout (dialogs go through zenity: OSR has
// no native dialogs of its own).  Returns the exit code, -1 if it cannot run.
int run_capture(const std::vector<std::string>& argv, std::string* out) {
    int fd[2];
    if (pipe(fd) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    close(fd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) if (out) out->append(buf, (size_t)n);
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string downloads_dir() {
    if (const char* d = std::getenv("XDG_DOWNLOAD_DIR")) if (*d) return d;   // tests / overrides
    std::string out;
    if (run_capture({"xdg-user-dir", "DOWNLOAD"}, &out) == 0) {
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (!out.empty()) return out;
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/Downloads";
}

void console_note(int level, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_console.size() < 200) g_console.emplace_back(level, text);
}

class LuxClient : public CefClient,
                  public CefLifeSpanHandler,
                  public CefDisplayHandler,
                  public CefLoadHandler,
                  public CefRenderHandler,
                  public CefDialogHandler,
                  public CefJSDialogHandler,
                  public CefDownloadHandler,
                  public CefPermissionHandler {
public:
    // CefClient
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler()  override { return this; }
    CefRefPtr<CefLoadHandler>     GetLoadHandler()     override { return this; }
    CefRefPtr<CefRenderHandler>   GetRenderHandler()   override { return this; }
    CefRefPtr<CefDialogHandler>   GetDialogHandler()   override { return this; }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }

    // CefDialogHandler: <input type=file> through zenity, off the UI thread.
    bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode mode, const CefString& title,
                      const CefString& default_path, const std::vector<CefString>& filters,
                      const std::vector<CefString>& extensions, const std::vector<CefString>&,
                      CefRefPtr<CefFileDialogCallback> cb) override {
        std::vector<std::string> a = {"zenity", "--file-selection"};
        const int m = ((int)mode & 0xFF);
        if (m == FILE_DIALOG_OPEN_MULTIPLE) { a.push_back("--multiple"); a.push_back("--separator=\n"); }
        if (m == FILE_DIALOG_OPEN_FOLDER) a.push_back("--directory");
        if (m == FILE_DIALOG_SAVE) { a.push_back("--save"); a.push_back("--confirm-overwrite"); }
        if (!title.empty()) a.push_back("--title=" + title.ToString());
        if (!default_path.empty()) a.push_back("--filename=" + default_path.ToString());
        std::string globs;
        auto add_group = [&](const std::string& g) { globs += (globs.empty() ? "" : " ") + g; };
        for (auto& f : filters) {
            const std::string s = f.ToString();
            if (s == "image/*") add_group("*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg *.avif");
            else if (s == "video/*") add_group("*.mp4 *.mkv *.webm *.avi *.mov *.m4v");
            else if (s == "audio/*") add_group("*.mp3 *.ogg *.flac *.wav *.m4a *.opus");
            else if (!s.empty() && s[0] == '.') add_group("*" + s);
        }
        for (auto& e : extensions) if (!e.empty()) add_group("*" + (e.ToString()[0] == '.' ? "" : std::string(".")) + e.ToString());
        if (!globs.empty()) a.push_back("--file-filter=" + globs);
        std::thread([a, cb] {
            std::string out;
            const int rc = run_capture(a, &out);
            std::vector<CefString> paths;
            for (size_t i = 0; rc == 0 && i < out.size();) {
                size_t j = out.find('\n', i);
                if (j == std::string::npos) j = out.size();
                if (j > i) paths.push_back(out.substr(i, j - i));
                i = j + 1;
            }
            if (paths.empty()) cb->Cancel(); else cb->Continue(paths);
        }).detach();
        return true;
    }

    // CefJSDialogHandler: alert/confirm/prompt through zenity.
    bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString& origin, JSDialogType type,
                    const CefString& message, const CefString& def,
                    CefRefPtr<CefJSDialogCallback> cb, bool&) override {
        std::vector<std::string> a = {"zenity", "--no-markup", "--title=" + origin.ToString(),
                                      "--text=" + message.ToString()};
        if (type == JSDIALOGTYPE_ALERT) a.push_back("--info");
        else if (type == JSDIALOGTYPE_CONFIRM) a.push_back("--question");
        else { a.push_back("--entry"); a.push_back("--entry-text=" + def.ToString()); }
        const bool prompt = type == JSDIALOGTYPE_PROMPT, alert = type == JSDIALOGTYPE_ALERT;
        std::thread([a, cb, prompt, alert] {
            std::string out;
            const int rc = run_capture(a, &out);
            while (!out.empty() && out.back() == '\n') out.pop_back();
            cb->Continue(alert || rc == 0, prompt ? out : std::string());
        }).detach();
        return true;
    }
    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool,
                              CefRefPtr<CefJSDialogCallback> cb) override {
        cb->Continue(true, CefString());   // navigating away never asks
        return true;
    }

    // CefDownloadHandler: straight into the XDG download directory.
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, const CefString& name,
                          CefRefPtr<CefBeforeDownloadCallback> cb) override {
        std::string dir = downloads_dir();
        mkdir(dir.c_str(), 0755);
        std::string base = name.empty() ? "download" : name.ToString();
        std::string path = dir + "/" + base;
        for (int i = 1; access(path.c_str(), F_OK) == 0 && i < 1000; ++i) {   // never overwrite
            const size_t dot = base.rfind('.');
            path = dir + "/" + (dot == std::string::npos ? base + " (" + std::to_string(i) + ")"
                                                         : base.substr(0, dot) + " (" + std::to_string(i) + ")" + base.substr(dot));
        }
        cb->Continue(path, false);
        console_note(0, "download -> " + path);
        return true;
    }
    void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
                           CefRefPtr<CefDownloadItemCallback>) override {
        if (item->IsComplete()) console_note(0, "download complete: " + item->GetFullPath().ToString());
        else if (item->IsCanceled()) console_note(1, "download canceled: " + item->GetURL().ToString());
    }

    // CefPermissionHandler: clipboard and the like yes; sensors, location, camera no.
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, const CefString&, uint32_t what,
                                CefRefPtr<CefPermissionPromptCallback> cb) override {
        const uint32_t allowed = CEF_PERMISSION_TYPE_CLIPBOARD | CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS |
                                 CEF_PERMISSION_TYPE_KEYBOARD_LOCK | CEF_PERMISSION_TYPE_POINTER_LOCK |
                                 CEF_PERMISSION_TYPE_STORAGE_ACCESS | CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS |
                                 CEF_PERMISSION_TYPE_DISK_QUOTA;
        cb->Continue((what & ~allowed) == 0 ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
        return true;
    }

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
        dbgf("OnAfterCreated pid=%d", (int)b->GetIdentifier());
        std::lock_guard<std::mutex> lk(g_mutex);
        g_browser = b;
    }
    // target=_blank / window.open: one window, so the target loads here.
    bool OnBeforePopup(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame>, int, const CefString& url,
                       const CefString&, cef_window_open_disposition_t, bool, const CefPopupFeatures&,
                       CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&, bool*) override {
        if (!url.empty()) b->GetMainFrame()->LoadURL(url);
        return true;
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
    void OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) override {
        g_fullscreen = fullscreen;
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
    void OnLoadEnd(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame>, int) override {
        // OSR ignores GetScreenInfo's device_scale_factor in this CEF (devicePixelRatio stays 1),
        // so HiDPI is page zoom: log(scale)/log(1.2) is Chromium's zoom-level unit.
        if (g_scale != 1.0f && b) b->GetHost()->SetZoomLevel(std::log(g_scale) / std::log(1.2));
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

// LUXIUM_SCALE wins; otherwise Xft.dpi / 96 like GTK and Chrome do on X11.
static float detect_scale(unsigned long display) {
    if (const char* e = std::getenv("LUXIUM_SCALE")) { const float f = (float)std::atof(e); if (f >= 0.5f && f <= 8) return f; }
    if (display) if (const char* rm = XResourceManagerString((Display*)display)) {
        if (const char* p = std::strstr(rm, "Xft.dpi:")) {
            const float dpi = (float)std::atof(p + 8);
            if (dpi >= 48) return std::clamp(std::round(dpi / 96 * 4) / 4, 1.0f, 4.0f);   // quarter steps
        }
    }
    return 1.0f;
}

void init(unsigned long display, unsigned long parent) {
    g_scale = detect_scale(display);
    static bool done = false;
    if (done) return;
    done = true;
    g_display = display;
    g_parent  = parent;

    CefMainArgs args(g_argc, g_argv);
    CefSettings settings;
    settings.multi_threaded_message_loop = true;   // CEF's own UI thread
    settings.no_sandbox = true;
    settings.disable_signal_handlers = true;       // SIGTERM/SIGINT reach SDL as a quit, so shutdown flushes the profile
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

    // Persistent profile (cookies, localStorage, cache): LUXIUM_PROFILE picks the
    // directory, "none" means a throw-away in-memory profile.  Chromium allows one
    // process per profile; a second Luxium on the same profile falls back to memory.
    std::string profile;
    if (const char* p = std::getenv("LUXIUM_PROFILE")) profile = p;
    else if (const char* h = std::getenv("HOME")) profile = std::string(h) + "/.local/share/luxium/profile";
    if (profile == "none") profile.clear();
    if (!profile.empty()) {
        std::string up;
        for (size_t i = 1; i <= profile.size(); ++i)
            if (i == profile.size() || profile[i] == '/') mkdir(profile.substr(0, i).c_str(), 0755);
        CefString(&settings.root_cache_path) = profile;
        CefString(&settings.cache_path) = profile;
        settings.persist_session_cookies = true;
    }
    if (!CefInitialize(args, settings, app(), nullptr) && !profile.empty()) {
        std::fprintf(stderr, "luxium: profile %s is in use by another instance; using a temporary one\n", profile.c_str());
        CefString(&settings.root_cache_path) = "";
        CefString(&settings.cache_path) = "";
        CefInitialize(args, settings, app(), nullptr);
    }
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

bool fullscreen() { return g_fullscreen.load(); }
void exit_fullscreen() {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetHost()->ExitFullscreen(true);
}

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

// Ctrl+C/X/V/A: OSR gets no editing shortcuts from raw key events, so the shell
// maps them to frame commands.
void edit(char cmd) {
    CefRefPtr<CefBrowser> b = browser();
    if (!b) return;
    CefRefPtr<CefFrame> f = b->GetFocusedFrame() ? b->GetFocusedFrame() : b->GetMainFrame();
    switch (cmd) {
    case 'c': f->Copy(); break;
    case 'x': f->Cut(); break;
    case 'v': f->Paste(); break;
    case 'a': f->SelectAll(); break;
    }
}

// Cookies are written lazily; wait (bounded) until they are on disk.
void flush_cookies() {
    struct Done : CefCompletionCallback {
        std::promise<void> p;
        void OnComplete() override { p.set_value(); }
        IMPLEMENT_REFCOUNTING(Done);
    };
    CefRefPtr<CefCookieManager> cm = CefCookieManager::GetGlobalManager(nullptr);
    if (!cm) return;
    CefRefPtr<Done> d = new Done;
    std::future<void> f = d->p.get_future();
    const bool started = cm->FlushStore(d);
    if (started) f.wait_for(std::chrono::seconds(3));
}

void ime_compose(const std::string& utf8, int caret) {
    CefRefPtr<CefBrowser> b = browser();
    if (!b) return;
    std::vector<CefCompositionUnderline> ul;
    CefCompositionUnderline u;
    u.range = CefRange(0, (int)CefString(utf8).length());
    u.color = 0xFF000000; u.background_color = 0; u.thick = false; u.style = CEF_CUS_SOLID;
    ul.push_back(u);
    b->GetHost()->ImeSetComposition(utf8, ul, CefRange(UINT32_MAX, UINT32_MAX), CefRange(caret, caret));
}
void ime_commit(const std::string& utf8) {
    CefRefPtr<CefBrowser> b = browser();
    if (b) b->GetHost()->ImeCommitText(utf8, CefRange(UINT32_MAX, UINT32_MAX), 0);
}

void shutdown() {
    {
        CefRefPtr<CefBrowser> b = browser();
        if (b) b->GetHost()->CloseBrowser(true);
    }
    flush_cookies();
    CefShutdown();
}

} // namespace luxium::cef
