#include "script/engine.hpp"
#include "script/engine_internal.hpp"
#include <script/bytecode_io.hpp>

#include <dom/node.hpp>
#include <style/selector.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>
#include <lux_script/emitter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace luxium::script {

using lux_script::VM;

#define NEED_STR(i, what) \
    if (a.size() <= (i) || !a[i].is_str()) { e = what " expects a string"; return Value::null(); }

// ── lifecycle ───────────────────────────────────────────────────────────────

Engine::Engine(PageHost& page) : page_(page) {
    register_core();
    register_dom();
    register_net();
    register_util();
}

Engine::~Engine() {
    net_reset();
    parked_.clear();
    delete compiled_;
}

void Engine::set_program(std::unique_ptr<Compiled> program) {
    delete compiled_;
    compiled_ = program.release();
}

bool Engine::load_precompiled(const std::string& bytes, std::string* err) {
    delete compiled_;
    compiled_ = nullptr;
    auto c = load_bytecode(bytes, err);
    if (!c) return false;
    compiled_ = c.release();
    return true;
}

void Engine::reset() {
    ++gen_;                     // answers of the old document that arrive late are dropped
    net_reset();
    parked_.clear();
    frame_waiters_.clear();
    delete compiled_;
    compiled_ = nullptr;
    timers_.clear();
    listeners_.clear();
    kind_count_.clear();
    navigate_fns_.clear();
    media_watch_.clear();
    dialog_ret_.clear();
    active_ev_.reset();
    nodes_.clear();
    node_handle_.clear();
    detached_.clear();
    removed_.clear();
    page_state_.clear();
    sel_cache_.clear();
    tpl_cache_.clear();
    next_handle_ = 1;
    loc_ = page_.page_source_name();
    hist_.assign(1, loc_);
    hist_idx_ = 0;
}

void Engine::log(int level, const std::string& msg) {
    page_.console(level < 0 ? 0 : (level > 2 ? 2 : level), msg);
}

void Engine::script_error(const std::string& what, const lux_script::SourceLoc& loc) {
    std::string msg = what;
    if (loc.line > 0) {
        int doc = loc.line;
        if (compiled_ && loc.line >= 1 &&
            loc.line <= static_cast<int>(compiled_->line_map.size()))
            doc = compiled_->line_map[static_cast<size_t>(loc.line - 1)];
        msg += " (page line " + std::to_string(std::max(1, doc)) + ")";
    }
    page_.console(2, msg);
}

lux_script::NativeCtx Engine::make_ctx() {
    lux_script::NativeCtx ctx;
    ctx.host      = this;
    ctx.functions = compiled_ ? &compiled_->functions : nullptr;
    return ctx;
}

// ── coroutines ──────────────────────────────────────────────────────────────

namespace {
// Makes `ctx` the one natives re-enter through while a VM call runs.
struct CtxScope {
    lux_script::NativeCtx*  saved;
    explicit CtxScope(lux_script::NativeCtx& c) : saved(lux_script::current_native_ctx()) {
        lux_script::current_native_ctx() = &c;
    }
    ~CtxScope() { lux_script::current_native_ctx() = saved; }
};
} // namespace

void Engine::start_fn(const Value& fn, const char* what, std::shared_ptr<EvState> ev) {
    if (!compiled_ || !fn.is_func() || fn.as_func_index() < 0 ||
        static_cast<size_t>(fn.as_func_index()) >= compiled_->functions.size()) {
        page_.console(2, std::string(what) + ": nothing compiled to run");
        return;
    }
    auto c = std::make_unique<Coro>();
    c->id   = next_coro_++;
    c->vm   = std::make_unique<VM>();
    c->ev   = std::move(ev);
    c->what = what;
    auto saved = active_ev_;
    active_ev_ = c->ev;
    lux_script::NativeCtx ctx = make_ctx();
    VM::Result r;
    {
        CtxScope scope(ctx);
        r = c->vm->start(*compiled_->functions[static_cast<size_t>(fn.as_func_index())], {}, ctx,
                         &compiled_->functions);
    }
    drive(std::move(c), std::move(r));
    active_ev_ = saved;
}

void Engine::drive(std::unique_ptr<Coro> c, VM::Result r) {
    auto saved = active_ev_;
    active_ev_ = c->ev;
    while (true) {
        if (r.status == VM::Status::Done) break;
        if (r.status == VM::Status::Error) {
            script_error(r.error, r.error_loc);
            break;
        }
        // Suspended: find out what it waits for.
        if (r.await_is_module || r.await_id < 0) {
            page_.console(2, c->what + ": unsupported await");
            break;
        }
        std::string name = lux_script::native_at(r.await_id).name;
        if (name.rfind("__", 0) == 0) name = name.substr(2);
        Value out;
        std::string err;
        const bool immediate = begin_async(*c, name, r.await_args, out, err);
        if (!immediate) {
            const uint64_t id = c->id;
            parked_[id] = std::move(c);
            active_ev_ = saved;
            return;
        }
        lux_script::NativeCtx ctx = make_ctx();
        CtxScope scope(ctx);
        r = err.empty() ? c->vm->resume(std::move(out), ctx)
                        : c->vm->resume_error(err, lux_script::SourceLoc{}, ctx);
    }
    active_ev_ = saved;
}

void Engine::resume_coro(uint64_t id, Value v) {
    auto it = parked_.find(id);
    if (it == parked_.end()) return;
    auto c = std::move(it->second);
    parked_.erase(it);
    auto saved = active_ev_;
    active_ev_ = c->ev;
    lux_script::NativeCtx ctx = make_ctx();
    VM::Result r;
    {
        CtxScope scope(ctx);
        r = c->vm->resume(std::move(v), ctx);
    }
    drive(std::move(c), std::move(r));
    active_ev_ = saved;
}

void Engine::resume_coro_error(uint64_t id, const std::string& msg) {
    auto it = parked_.find(id);
    if (it == parked_.end()) return;
    auto c = std::move(it->second);
    parked_.erase(it);
    auto saved = active_ev_;
    active_ev_ = c->ev;
    lux_script::NativeCtx ctx = make_ctx();
    VM::Result r;
    {
        CtxScope scope(ctx);
        r = c->vm->resume_error(msg, lux_script::SourceLoc{}, ctx);
    }
    drive(std::move(c), std::move(r));
    active_ev_ = saved;
}

bool Engine::begin_async(Coro& c, const std::string& name, Args& args, Value& out, std::string& err) {
    if (name == "sleep") {
        if (args.empty() || !args[0].is_num()) { err = "sleep() expects the delay in ms"; return true; }
        add_timer(static_cast<int>(args[0].as_float()), false, Value::null(), c.id);
        return false;
    }
    if (name == "page_frame") {
        frame_waiters_.push_back(c.id);
        return false;
    }
    return net_begin(c, name, args, out, err);
}

void Engine::run_main() {
    if (!compiled_) return;
    // __luxium_main__ is the first function in the wrapped source, so chunk 0.
    start_fn(Value::func(0), "the page's script");
}

// ── timers ──────────────────────────────────────────────────────────────────

long long Engine::add_timer(int ms, bool repeat, const Value& fn, uint64_t coro) {
    Timer t;
    t.id    = next_timer_++;
    t.at    = mono_ms() + std::max(0, ms);
    t.every = repeat ? std::max(1, ms) : 0;
    t.fn    = fn;
    t.coro  = coro;
    timers_.push_back(std::move(t));
    return timers_.back().id;
}

void Engine::cancel_timer(long long id) {
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return t.id == id; }),
                  timers_.end());
}

void Engine::tick() {
    net_tick();

    if (!timers_.empty()) {
        const long long now = mono_ms();
        std::vector<long long> due;
        for (const auto& t : timers_)
            if (t.at <= now) due.push_back(t.id);
        for (long long id : due) {
            auto it = std::find_if(timers_.begin(), timers_.end(),
                                   [&](const Timer& t) { return t.id == id; });
            if (it == timers_.end()) continue;   // cancelled by an earlier handler
            Value fn = it->fn;
            uint64_t coro = it->coro;
            if (it->every > 0) it->at = std::max(it->at + it->every, now);
            else timers_.erase(it);
            if (coro) resume_coro(coro, Value::null());
            else      start_fn(fn, "a timer");
        }
    }

    if (!frame_waiters_.empty()) {
        std::vector<uint64_t> w;
        w.swap(frame_waiters_);
        for (uint64_t id : w) resume_coro(id, Value::null());
    }

    poll_media();
}

// ── events ──────────────────────────────────────────────────────────────────

namespace {
bool bubbles(const std::string& k) {
    static const char* none[] = {"focus", "blur", "mouseenter", "mouseleave", "load", "error",
        "play", "pause", "ended", "timeupdate", "seeking", "seeked", "loadedmetadata",
        "durationchange", "volumechange", "waiting", "playing", "cuechange"};
    for (const char* n : none) if (k == n) return false;
    return true;
}
bool is_media_kind(const std::string& k) {
    static const char* m[] = {"play", "pause", "ended", "timeupdate", "seeking", "seeked",
        "loadedmetadata", "durationchange", "volumechange", "waiting", "playing", "cuechange", "error"};
    for (const char* n : m) if (k == n) return true;
    return false;
}
} // namespace

long long Engine::add_listener(dom::Node* node, const std::string& kind, const Value& fn, bool capture) {
    auto& v = listeners_[node];
    for (const auto& l : v)
        if (l.kind == kind && l.capture == capture && l.fn.is_func() && fn.is_func() &&
            l.fn.as_func_index() == fn.as_func_index())
            return l.id;   // same handler twice is one handler, like addEventListener
    Listener l;
    l.id = next_listener_++;
    l.node = node;
    l.kind = kind;
    l.fn = fn;
    l.capture = capture;
    v.push_back(std::move(l));
    ++kind_count_[kind];
    if (node && is_media_kind(kind)) media_watch_.try_emplace(node);
    return v.back().id;
}

bool Engine::remove_listener(dom::Node* node, const std::string& kind, const Value& fn) {
    auto it = listeners_.find(node);
    if (it == listeners_.end()) return false;
    auto& v = it->second;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Listener& l) {
        return l.kind == kind && fn.is_func() && l.fn.is_func() &&
               l.fn.as_func_index() == fn.as_func_index();
    }), v.end());
    const size_t gone = before - v.size();
    if (gone && (kind_count_[kind] -= static_cast<int>(gone)) <= 0) kind_count_.erase(kind);
    return gone > 0;
}

bool Engine::has_listener(const std::string& kind) const { return kind_count_.count(kind) > 0; }

bool Engine::has_handler_for(const dom::Node* node) const {
    if (!has_listener("click")) return false;
    for (const dom::Node* n = node; n; n = n->parent) {
        auto it = listeners_.find(n);
        if (it == listeners_.end()) continue;
        for (const auto& l : it->second)
            if (l.kind == "click") return true;
    }
    return false;
}

void Engine::run_listeners(dom::Node* at, const std::string& kind, bool capture_phase,
                           const std::shared_ptr<EvState>& ev) {
    auto it = listeners_.find(at);
    if (it == listeners_.end()) return;
    std::vector<Value> fns;   // snapshot: a handler may add or remove listeners
    for (const auto& l : it->second)
        if (l.kind == kind && l.capture == capture_phase) fns.push_back(l.fn);
    for (const auto& fn : fns) {
        ev->current = at;
        start_fn(fn, "an event handler", ev);
    }
}

bool Engine::dispatch(const std::string& kind, dom::Node* target, const EventData& d) {
    if (!has_listener(kind)) return false;
    dom::Node* root = page_.page_root();
    if (!target) target = root;
    if (!target) return false;
    auto ev = std::make_shared<EvState>();
    ev->kind = kind;
    ev->target = target;
    ev->d = d;
    if (ev->d.value.empty() && (kind == "input" || kind == "change") && target->is_element())
        ev->d.value = control_value(target);

    std::vector<dom::Node*> path;   // target first, document last
    for (dom::Node* n = target; n; n = n->parent) path.push_back(n);
    const bool bub = bubbles(kind);

    for (size_t i = path.size(); i-- > 1 && !ev->stopped;)
        run_listeners(path[i], kind, true, ev);
    if (!ev->stopped) run_listeners(target, kind, true, ev);
    if (!ev->stopped) run_listeners(target, kind, false, ev);
    for (size_t i = 1; bub && i < path.size() && !ev->stopped; ++i)
        run_listeners(path[i], kind, false, ev);
    if (bub && !ev->stopped) run_listeners(nullptr, kind, false, ev);   // window
    return ev->prevented;
}

void Engine::dispatch_window(const std::string& kind, const EventData& d) {
    if (!has_listener(kind)) return;
    auto ev = std::make_shared<EvState>();
    ev->kind = kind;
    ev->d = d;
    run_listeners(nullptr, kind, false, ev);
}

void Engine::poll_media() {
    MediaApi* api = page_.media();
    if (!api || media_watch_.empty()) return;
    std::vector<dom::Node*> nodes;
    for (const auto& [n, _] : media_watch_) nodes.push_back(n);
    for (dom::Node* n : nodes) {
        auto it = media_watch_.find(n);
        if (it == media_watch_.end()) continue;
        const MediaState st = api->state(n);
        if (!st.known) continue;
        MediaSnap& snap = it->second;
        if (snap.want_play) { snap.want_play = false; api->play(n); }
        if (snap.want_seek >= 0) { api->seek(n, snap.want_seek); snap.want_seek = -1; }
        const MediaState old = snap.st;
        const bool first = !snap.had;
        snap.st = st;
        snap.had = true;
        std::vector<const char*> fire;
        if (st.duration > 0 && (first || old.duration <= 0 || old.duration != st.duration)) {
            if (first || old.duration <= 0) fire.push_back("loadedmetadata");
            fire.push_back("durationchange");
        }
        if (!st.paused && (first || old.paused)) { fire.push_back("play"); fire.push_back("playing"); }
        if (st.paused && !first && !old.paused && !st.ended) fire.push_back("pause");
        if (st.ended && !old.ended) fire.push_back("ended");
        if (st.seeking && !old.seeking) fire.push_back("seeking");
        if (!st.seeking && old.seeking) fire.push_back("seeked");
        if (st.waiting && !old.waiting) fire.push_back("waiting");
        if (!st.waiting && old.waiting && !st.paused) fire.push_back("playing");
        if (st.error && !old.error) fire.push_back("error");
        if (!first && std::fabs(st.time - snap.last_tick) >= 0.25) fire.push_back("timeupdate");
        if (first || std::fabs(st.time - snap.last_tick) >= 0.25) snap.last_tick = st.time;
        if (!first && st.cue != old.cue) fire.push_back("cuechange");
        for (const char* k : fire) dispatch(k, n);
    }
}

// ── history / location ──────────────────────────────────────────────────────

std::string Engine::origin() const {
    const UrlParts p = split_url(loc_);
    return (p.scheme == "http" || p.scheme == "https") ? p.origin() : std::string();
}

std::string Engine::resolve_url(const std::string& rel) const {
    const UrlParts base = split_url(loc_);
    if (split_url(rel).absolute) return rel;
    std::string o = base.origin();
    if (rel.rfind("//", 0) == 0) return base.scheme + ":" + rel;
    if (!rel.empty() && rel[0] == '/') return o + rel;
    if (!rel.empty() && rel[0] == '?') return o + base.path + rel;
    if (!rel.empty() && rel[0] == '#') return o + base.path + (base.query.empty() ? "" : "?" + base.query) + rel;
    std::string dir = base.path.substr(0, base.path.find_last_of('/') + 1);
    return o + dir + rel;
}

Value Engine::location_value() {
    const UrlParts p = split_url(loc_);
    Value::Dict d;
    d.append("url", Value::str(loc_));
    d.append("origin", Value::str(origin()));
    d.append("path", Value::str(p.path));
    d.append("search", Value::str(p.query.empty() ? "" : "?" + p.query));
    d.append("hash", Value::str(p.hash));      // without '#': "t=token" for http://host/#t=token
    auto parse_q = [](const std::string& q) {
        Value::Dict dq;
        size_t i = 0;
        while (i <= q.size() && !q.empty()) {
            size_t amp = q.find('&', i);
            std::string kv = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
            size_t eq = kv.find('=');
            std::string k = percent_decode(kv.substr(0, eq), true);
            std::string v = eq == std::string::npos ? "" : percent_decode(kv.substr(eq + 1), true);
            if (!k.empty()) dq.set(std::move(k), Value::str(std::move(v)));
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
        return Value::dict(std::move(dq));
    };
    d.append("query", parse_q(p.query));
    d.append("fragment", parse_q(p.hash));     // the hash read as a query string
    return Value::dict(std::move(d));
}

bool Engine::nav_to(const std::string& path, bool replace, std::string& err) {
    const std::string target = resolve_url(path);
    const std::string o = origin();
    if (!o.empty() && split_url(target).origin() != o) {
        err = "history: " + path + " is not on this page's origin";
        return false;
    }
    if (replace) hist_[hist_idx_] = target;
    else {
        hist_.resize(hist_idx_ + 1);
        hist_.push_back(target);
        ++hist_idx_;
    }
    loc_ = target;
    page_.location_changed(target, replace);
    return true;
}

bool Engine::history_go(int delta) {
    const long long n = static_cast<long long>(hist_idx_) + delta;
    if (n < 0 || n >= static_cast<long long>(hist_.size())) return false;
    hist_idx_ = static_cast<size_t>(n);
    loc_ = hist_[hist_idx_];
    page_.location_changed(loc_, true);
    for (const auto& fn : std::vector<Value>(navigate_fns_)) start_fn(fn, "a navigation handler");
    return true;
}

// ── Host dispatch ───────────────────────────────────────────────────────────

Value Engine::call_native(const char* name, Args& args, std::string& error) {
    auto it = natives_.find(name);
    if (it == natives_.end()) {
        error = std::string("'") + name + "' is declared but not implemented in this Luxium build";
        return Value::null();
    }
    return it->second(args, error);
}

Value Engine::call_method(const std::string& type, long long handle, const std::string& method,
                          Args& args, std::string& error) {
    auto it = methods_.find(type + "." + method);
    if (it == methods_.end()) {
        error = type + " has no method '" + method + "' in this Luxium build";
        return Value::null();
    }
    return it->second(handle, args, error);
}

// ── core natives: log, page, event, history ─────────────────────────────────

void Engine::register_core() {
    reg("log_info",  [this](Args& a, std::string&) { log(0, a[0].to_string()); return Value::null(); });
    reg("log_warn",  [this](Args& a, std::string&) { log(1, a[0].to_string()); return Value::null(); });
    reg("log_error", [this](Args& a, std::string&) { log(2, a[0].to_string()); return Value::null(); });

    // page.*
    reg("page_get", [this](Args& a, std::string& e) {
        NEED_STR(0, "page.get()");
        auto it = page_state_.find(a[0].as_str());
        return it == page_state_.end() ? Value::null() : it->second;
    });
    reg("page_set", [this](Args& a, std::string& e) {
        NEED_STR(0, "page.set()");
        page_state_[a[0].as_str()] = a[1];
        return Value::null();
    });
    reg("page_after", [this](Args& a, std::string& e) {
        if (!a[0].is_num() || !a[1].is_func()) { e = "page.after(ms, fn) expects an int and a function"; return Value::null(); }
        return Value::integer(add_timer(static_cast<int>(a[0].as_float()), false, a[1], 0));
    });
    reg("page_every", [this](Args& a, std::string& e) {
        if (!a[0].is_num() || !a[1].is_func()) { e = "page.every(ms, fn) expects an int and a function"; return Value::null(); }
        return Value::integer(add_timer(static_cast<int>(a[0].as_float()), true, a[1], 0));
    });
    reg("page_cancel", [this](Args& a, std::string& e) {
        if (!a[0].is_int()) { e = "page.cancel() expects the id page.after/every returned"; return Value::null(); }
        cancel_timer(a[0].as_int());
        return Value::null();
    });
    reg("page_clicked", [this](Args&, std::string&) {
        if (!active_ev_ || active_ev_->kind != "click") return Value::null();
        return element_value(active_ev_->target);
    });
    reg("page_now", [](Args&, std::string&) { return Value::real(double(mono_ms())); });
    reg("page_location", [this](Args&, std::string&) { return location_value(); });
    reg("page_on_navigate", [this](Args& a, std::string& e) {
        if (!a[0].is_func()) { e = "page.on_navigate(fn) expects a function"; return Value::null(); }
        navigate_fns_.push_back(a[0]);
        return Value::null();
    });
    reg("page_listen", [this](Args& a, std::string& e) {
        if (!a[0].is_str() || !a[1].is_func()) { e = "page.listen(kind, fn) expects a string and a function"; return Value::null(); }
        add_listener(nullptr, a[0].as_str(), a[1], false);
        return Value::null();
    });
    reg("page_title", [this](Args& a, std::string&) {
        if (a.empty()) return Value::str(page_.page_title());
        page_.set_page_title(a[0].to_string());
        return Value::null();
    });
    reg("page_scroll_y", [this](Args&, std::string&) {
        auto& h = page_.hooks();
        return Value::real(h.page_scroll_y ? h.page_scroll_y() : 0.0);
    });
    reg("page_scroll_to", [this](Args& a, std::string&) {
        auto& h = page_.hooks();
        if (h.page_scroll_to && a[0].is_num()) h.page_scroll_to(static_cast<float>(a[0].as_float()));
        return Value::null();
    });
    reg("page_reload", [this](Args&, std::string&) {
        if (page_.hooks().reload) page_.hooks().reload();
        return Value::null();
    });

    // history.*
    reg("history_push", [this](Args& a, std::string& e) {
        NEED_STR(0, "history.push()");
        std::string err;
        if (!nav_to(a[0].as_str(), false, err)) e = err;
        return Value::null();
    });
    reg("history_replace", [this](Args& a, std::string& e) {
        NEED_STR(0, "history.replace()");
        std::string err;
        if (!nav_to(a[0].as_str(), true, err)) e = err;
        return Value::null();
    });
    reg("history_back", [this](Args&, std::string&) {
        if (!history_go(-1) && page_.hooks().history_back_outside) page_.hooks().history_back_outside();
        return Value::null();
    });
    reg("history_forward", [this](Args&, std::string&) { history_go(1); return Value::null(); });

    // event.* -- reads the event this coroutine was started for
    auto ev_str = [this](auto get) {
        return [this, get](Args&, std::string&) {
            return active_ev_ ? Value::str(get(*active_ev_)) : Value::str("");
        };
    };
    auto ev_bool = [this](auto get) {
        return [this, get](Args&, std::string&) {
            return Value::boolean(active_ev_ && get(*active_ev_));
        };
    };
    auto ev_num = [this](auto get) {
        return [this, get](Args&, std::string&) {
            return active_ev_ ? Value::real(get(*active_ev_)) : Value::real(0);
        };
    };
    reg("event_kind",    ev_str([](EvState& v) { return v.kind; }));
    reg("event_key",     ev_str([](EvState& v) { return v.d.key; }));
    reg("event_code",    ev_str([](EvState& v) { return v.d.code; }));
    reg("event_value",   ev_str([](EvState& v) { return v.d.value; }));
    reg("event_message", ev_str([](EvState& v) { return v.d.message; }));
    reg("event_ctrl",    ev_bool([](EvState& v) { return v.d.ctrl; }));
    reg("event_shift",   ev_bool([](EvState& v) { return v.d.shift; }));
    reg("event_alt",     ev_bool([](EvState& v) { return v.d.alt; }));
    reg("event_meta",    ev_bool([](EvState& v) { return v.d.meta; }));
    reg("event_repeat",  ev_bool([](EvState& v) { return v.d.repeat; }));
    reg("event_prevented", ev_bool([](EvState& v) { return v.prevented; }));
    reg("event_x",       ev_num([](EvState& v) { return double(v.d.x); }));
    reg("event_y",       ev_num([](EvState& v) { return double(v.d.y); }));
    reg("event_delta",   ev_num([](EvState& v) { return double(v.d.delta); }));
    reg("event_button",  [this](Args&, std::string&) { return Value::integer(active_ev_ ? active_ev_->d.button : 0); });
    reg("event_close_code", [this](Args&, std::string&) { return Value::integer(active_ev_ ? active_ev_->d.close_code : 0); });
    reg("event_target",  [this](Args&, std::string&) { return active_ev_ ? element_value(active_ev_->target) : Value::null(); });
    reg("event_current", [this](Args&, std::string&) { return active_ev_ ? element_value(active_ev_->current) : Value::null(); });
    reg("event_prevent", [this](Args&, std::string&) { if (active_ev_) active_ev_->prevented = true; return Value::null(); });
    reg("event_stop",    [this](Args&, std::string&) { if (active_ev_) active_ev_->stopped = true; return Value::null(); });
}

} // namespace luxium::script
