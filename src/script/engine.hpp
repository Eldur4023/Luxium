#pragma once
// luxium::script -- the compile driver and the per-page execution surface.
//
// Lux the server compiles a project of .lux files (project.cpp); a browser
// page compiles ONE <script> body.  The body is plain statements and fns, so
// the driver wraps it in a synthetic `fn __luxium_main__():` -- the parser
// only takes declarations at the top level -- compiles every user fn/class
// exactly like the server does, and hands back the chunks.
//
// Page owns an Engine; Engine IS the lux_script::Host the builtins talk to.
// Everything a script can touch is declared in browser_api.inc and
// implemented by name in engine*.cpp:
//   engine.cpp        lifecycle, coroutines, timers, events, history
//   engine_dom.cpp    document.* and the Element methods
//   engine_net.cpp    http.*, ws.*, storage.*
//   engine_util.cpp   json, url, math, template, clipboard, forms, files
//
// Execution model: the top level, every event handler, every timer callback
// and every socket callback is a VM coroutine (Coro).  `await` suspends it;
// the engine resumes it from tick() when the awaited thing completes
// (sleep, the next frame, a curl worker thread's HTTP answer, a websocket
// message).  A script error is logged to the console and ends only that
// coroutine -- never the page.

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <script/page_compile.hpp>
#include <lux_script/diagnostic.hpp>
#include <lux_script/emitter.hpp>
#include <lux_script/natives.hpp>
#include <lux_script/value.hpp>
#include <lux_script/vm.hpp>

namespace luxium::dom {
struct Node;
}
namespace luxium::style {
struct Selector;
}

namespace luxium {

// ── What the shell (or a test harness) lends the page ───────────────────────
// Every member is optional; the engine falls back to something sensible
// without layout (the headless harness runs everything but geometry).
struct ShellHooks {
    // Border box of the node in page coordinates; false = not laid out.
    std::function<bool(const dom::Node*, float out_xywh[4])> rect;
    // Inner scroll offsets (overflow:auto boxes).
    std::function<float(const dom::Node*, bool vertical)>             scroll_get;
    std::function<void(const dom::Node*, bool vertical, float value)> scroll_set;
    std::function<void(const dom::Node*)> scroll_into_view;
    std::function<float()>                page_scroll_y;
    std::function<void(float)>            page_scroll_to;
    // Keyboard focus.  `focus(nullptr)` clears it.
    std::function<void(dom::Node*)> focus;
    std::function<dom::Node*()>     focused;
    // Element fullscreen: node = the element to show alone, nullptr = leave.
    std::function<void(dom::Node*)> fullscreen;
    std::function<dom::Node*()>     fullscreen_element;
    // System clipboard.
    std::function<void(const std::string&)> clipboard_set;
    std::function<std::string()>            clipboard_get;
    // Live value of a form control, when the shell keeps it outside the DOM
    // (B's text editing).  Return false = "read the DOM attributes".
    std::function<bool(const dom::Node*, std::string& out)> get_value;
    std::function<bool(dom::Node*, const std::string&)>     set_value;
    // Files chosen in an <input type=file> (absolute paths); empty = none.
    std::function<std::vector<std::string>(const dom::Node*)> files;
    // The page asks the shell to reload / go back past its own history.
    std::function<void()> reload;
    std::function<void()> history_back_outside;
    std::function<bool()> page_visible;
};

// ── <video>/<audio> ─────────────────────────────────────────────────────────
// The shell wires media::own behind this (shell/media_glue.cpp); the headless
// harness has no player, so state() says "unknown".
struct MediaState {
    bool   known = false;
    bool   paused = true, ended = false, seeking = false, waiting = false, error = false;
    double time = 0, duration = 0;
    int    audio_count = 0, audio_cur = 0;    // 1-based position of the audio stream playing
    int    sub_count = 0, sub_cur = 0;        // 1-based text track shown, 0 = off
    std::string audio_label, sub_label, cue;  // cue: active subtitle text, '\n' between lines
};
class MediaApi {
public:
    virtual ~MediaApi() = default;
    virtual MediaState state(const dom::Node*)               { return {}; }
    virtual void play(const dom::Node*)                      {}
    virtual void pause(const dom::Node*)                     {}
    virtual void seek(const dom::Node*, double /*seconds*/)  {}
    virtual void cycle_audio(const dom::Node*)               {}
    virtual void cycle_subtitle(const dom::Node*)            {}
    virtual void set_volume(const dom::Node*, double /*0..1*/) {}
    virtual void set_muted(const dom::Node*, bool)           {}
};

// What the script engine needs from the page it runs in.  The page model
// (page/page.hpp) implements it; the shell drives the page.
class PageHost {
public:
    virtual ~PageHost() = default;
    virtual void        console(int level, const std::string& msg) = 0;  // 0 info 1 warn 2 error
    virtual std::string page_title() const                         = 0;
    virtual void        set_page_title(const std::string&)         = 0;
    virtual dom::Node*  page_root()                                = 0;
    virtual void        page_mutated()                             = 0;  // relayout + repaint
    virtual std::string page_source_name() const                   = 0;  // "page.html" / the URL
    virtual ShellHooks& hooks()                                    = 0;
    virtual MediaApi*   media()                                    = 0;
    // Where this page's persistent data lives (storage.*): the main profile
    // for the normal browser, ~/.local/share/luxium/apps/<app_id> for an app window.
    virtual std::string profile_dir() const                        = 0;
    // history.push/replace: the address bar should show `url` (no reload).
    virtual void        location_changed(const std::string& url, bool replace) = 0;
};

// The data of a user-input event handed to Engine::dispatch.
struct EventData {
    std::string key, code, value, message;   // key/code: DOM names ("ArrowLeft", "Space")
    int   button = 0, close_code = 0;
    float x = 0, y = 0, delta = 0;
    bool  ctrl = false, shift = false, alt = false, meta = false, repeat = false;
};

} // namespace luxium

namespace luxium::script {

using lux_script::Value;
using Args = std::vector<Value>;

class Engine : public lux_script::Host {
public:
    explicit Engine(class PageHost& page);
    ~Engine();

    // Forget the previous document's program, handlers, timers, handles,
    // coroutines, sockets and page.* store.  The page calls it on every load:
    // without it a timer of page A would fire into page B's function table.
    void reset();

    void set_program(std::unique_ptr<Compiled> program);
    bool load_precompiled(const std::string& bytes, std::string* err);

    // Runs __luxium_main__ (top level), as a coroutine.
    void run_main();

    // ── events ───────────────────────────────────────────────────────────
    // Dispatches `kind` at `target` through capture, target and bubble
    // phases; returns true if a handler called event.prevent().  `target`
    // null = the document.
    bool dispatch(const std::string& kind, dom::Node* target, const EventData& d = {});
    // Window-level events (resize, visibilitychange, fullscreenchange).
    void dispatch_window(const std::string& kind, const EventData& d = {});
    bool has_listener(const std::string& kind) const;
    // A click handler anywhere from `node` up to the document?
    bool has_handler_for(const dom::Node* node) const;

    // <dialog>.close(value): hides it and fires "close".
    void close_dialog(dom::Node* dialog, const std::string& value);

    // The shell calls this every frame: network answers, timers, frame
    // waiters and media events.
    void tick();
    // The page's own back/forward (history.push entries).  False = nothing
    // to go back to inside this page.
    bool history_go(int delta);

    // Frees what remove() took out of the DOM.  Only once nothing (layout,
    // hover) can still point into it: the shell calls it after a relayout.
    void free_removed() { removed_.clear(); }

    // The page location after history.push (== page name until then).
    const std::string& location() const { return loc_; }

    // ── lux_script::Host ─────────────────────────────────────────────────
    void  log(int level, const std::string& msg) override;
    Value call_native(const char* name, Args& args, std::string& error) override;
    Value call_method(const std::string& type, long long handle, const std::string& method,
                      Args& args, std::string& error) override;

    // ── shared with the engine_*.cpp parts ───────────────────────────────
    using NativeImpl = std::function<Value(Args&, std::string&)>;
    using MethodImpl = std::function<Value(long long, Args&, std::string&)>;
    struct Listener {
        long long   id = 0;
        dom::Node*  node = nullptr;      // null = window
        std::string kind;
        Value       fn;
        bool        capture = false;
    };
    // The event being dispatched (or resumed): what the `event` object reads.
    struct EvState {
        std::string kind;
        dom::Node*  target = nullptr;
        dom::Node*  current = nullptr;
        EventData   d;
        bool        prevented = false, stopped = false;
    };
    // One running script: a VM plus the event it serves.
    struct Coro {
        uint64_t                        id = 0;
        std::unique_ptr<lux_script::VM> vm;
        std::shared_ptr<EvState>        ev;
        std::string                     what;
    };

    // Starts `fn` (a function value) as a coroutine.  `ev` may be null.
    void start_fn(const Value& fn, const char* what, std::shared_ptr<EvState> ev = nullptr);

    PageHost& page() { return page_; }

private:
    // lifecycle / coroutines (engine.cpp)
    void register_core();
    void drive(std::unique_ptr<Coro> c, lux_script::VM::Result r);
    void resume_coro(uint64_t id, Value v);
    void resume_coro_error(uint64_t id, const std::string& msg);
    // Starts the async operation `name`; true = answered right away in
    // `out`/`err`, false = parked until the engine resumes the coroutine.
    bool begin_async(Coro& c, const std::string& name, Args& args, Value& out, std::string& err);
    void script_error(const std::string& what, const lux_script::SourceLoc& loc);
    lux_script::NativeCtx make_ctx();
    void reg(const char* name, NativeImpl fn) { natives_[name] = std::move(fn); }
    void reg_method(const char* type, const char* name, MethodImpl fn) {
        methods_[std::string(type) + "." + name] = std::move(fn);
    }

    // timers
    long long add_timer(int ms, bool repeat, const Value& fn, uint64_t coro);
    void      cancel_timer(long long id);

    // events (engine.cpp)
    long long add_listener(dom::Node* node, const std::string& kind, const Value& fn, bool capture);
    bool      remove_listener(dom::Node* node, const std::string& kind, const Value& fn);
    void      run_listeners(dom::Node* at, const std::string& kind, bool capture_phase,
                            const std::shared_ptr<EvState>& ev);
    void      poll_media();

    // history (engine.cpp)
    bool  nav_to(const std::string& path, bool replace, std::string& err);
    Value location_value();
    std::string resolve_url(const std::string& rel) const;
    std::string origin() const;

    // DOM helpers (engine_dom.cpp)
    void register_dom();
    dom::Node* node_of(long long handle) const;
    long long  handle_of(dom::Node* node);
    Value      element_value(dom::Node* node);        // null if node is null
    dom::Node* arg_node(const Value& v, std::string& err, const char* what);
    void       forget_subtree(dom::Node* n);
    std::vector<dom::Node*> select_all(dom::Node* scope, const std::string& sel, bool first_only,
                                       std::string& err);
    std::string control_value(dom::Node* n);
    std::vector<std::string> input_files(dom::Node* n);   // hook, else the data-files attribute (one path per line)
    void        set_control_value(dom::Node* n, const std::string& v);
    void        set_children_html(dom::Node* n, const std::string& html, std::string& err);

    // net (engine_net.cpp)
    void register_net();
    void net_tick();
    void net_reset();
    bool net_begin(Coro& c, const std::string& name, Args& args, Value& out, std::string& err);

    // util (engine_util.cpp)
    void register_util();
    Value file_value(const std::string& path);   // a File object for a path on disk

    PageHost&                                page_;
    Compiled*                                compiled_ = nullptr;
    uint64_t                                 gen_ = 1;       // bumped by reset(); late answers are dropped

    std::unordered_map<std::string, NativeImpl> natives_;
    std::unordered_map<std::string, MethodImpl> methods_;

    // coroutines
    uint64_t                                 next_coro_ = 1;
    std::unordered_map<uint64_t, std::unique_ptr<Coro>> parked_;
    std::shared_ptr<EvState>                 active_ev_;      // what `event.*` reads right now
    std::vector<uint64_t>                    frame_waiters_;

    // timers
    struct Timer { long long id = 0, at = 0, every = 0; Value fn; uint64_t coro = 0; };
    std::vector<Timer>                       timers_;
    long long                                next_timer_ = 1;

    // DOM objects
    std::vector<std::unique_ptr<dom::Node>>  detached_;   // document.create() output
    std::vector<std::unique_ptr<dom::Node>>  removed_;    // see free_removed()
    long long                                next_handle_ = 1;
    std::unordered_map<long long, dom::Node*> nodes_;
    std::unordered_map<const dom::Node*, long long> node_handle_;
    std::unordered_map<std::string, Value>   page_state_;
    std::unordered_map<std::string, std::vector<style::Selector>> sel_cache_;
    std::map<std::string, std::shared_ptr<void>> tpl_cache_;   // template.render: compiled lux_script::Template

    // events
    std::unordered_map<const dom::Node*, std::vector<Listener>> listeners_;   // key null = window
    std::unordered_map<std::string, int>     kind_count_;
    long long                                next_listener_ = 1;
    std::vector<Value>                       navigate_fns_;   // page.on_navigate
    struct MediaSnap { MediaState st; double last_tick = -1; bool had = false; bool want_play = false; double want_seek = -1; };
    std::unordered_map<dom::Node*, MediaSnap> media_watch_;
    std::unordered_map<dom::Node*, std::string> dialog_ret_;

    // history
    std::string                              loc_;
    std::vector<std::string>                 hist_;
    size_t                                   hist_idx_ = 0;

    // net state (engine_net.cpp)
    struct Net;
    std::shared_ptr<Net>                     net_;
};

} // namespace luxium::script
