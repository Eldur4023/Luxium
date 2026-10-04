// Luxium's browser builtins: the stdlib that survived the trip from the
// server (len/str/int/float/range + the string/List/Dict methods, copied
// verbatim from Lux's natives.cpp) plus the browser surface (log, document,
// Element).  Everything server-side -- request/response, session, state,
// sse/ws, db, files -- has no entry here, which is the whole sandbox:
// the VM can only call what this table lists.
//
// The stdlib blocks carry Lux's own comments; they are the same code, not a
// reimplementation, so a behavior fixed there behaves the same here.

#include <lux_script/natives.hpp>
#include <lux_script/vm.hpp>

#include <algorithm>
#include <array>
#include <utility>
#include <unordered_set>

namespace lux_script {

namespace {

// ── Host wrappers: the reserved objects ─────────────────────────────────────

Value fn_log_info(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.host->log(0, args[0].to_string());
    return Value::null();
}
Value fn_log_warn(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.host->log(1, args[0].to_string());
    return Value::null();
}
Value fn_log_error(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.host->log(2, args[0].to_string());
    return Value::null();
}

Value fn_document_get(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "document.get() expects the id as a string"; return Value::null(); }
    return ctx.host->document_get(args[0].as_str());
}
Value fn_document_query_all(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "document.query_all() expects the tag as a string"; return Value::null(); }
    return ctx.host->document_query_all(args[0].as_str());
}
Value fn_document_title(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::str(ctx.host->document_title());
}
Value fn_document_root(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return ctx.host->document_root();
}

Value fn_page_get(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "page.get() expects the key as a string"; return Value::null(); }
    return ctx.host->page_get(args[0].as_str());
}
Value fn_page_set(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "page.set() expects the key as a string"; return Value::null(); }
    ctx.host->page_set(args[0].as_str(), args[1]);
    return Value::null();
}

Value fn_len(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_str())  return Value::integer((long long)utf8_length(v.as_str()));
    if (v.is_list()) return Value::integer((long long)v.as_list().size());
    if (v.is_dict()) return Value::integer((long long)v.as_dict().size());
    error = std::string("len() does not apply to ") + v.type_name();
    return Value::null();
}

Value fn_str(NativeCtx&, std::vector<Value>& args, std::string&) {
    return Value::str(args[0].to_string());
}

Value fn_float(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_num())  return Value::real(v.as_float());
    if (v.is_bool()) return Value::real(v.as_bool() ? 1.0 : 0.0);
    if (v.is_str()) {
        char* end = nullptr;
        const double d = std::strtod(v.as_str().c_str(), &end);
        if (!v.as_str().empty() && *end == '\0') return Value::real(d);
        error = "float(): '" + v.as_str() + "' is not a number";
        return Value::null();
    }
    error = std::string("float() does not apply to ") + v.type_name();
    return Value::null();
}

Value fn_int(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_int())   return v;
    if (v.is_float()) return Value::integer((long long)v.as_float());
    if (v.is_bool())  return Value::integer(v.as_bool() ? 1 : 0);
    if (v.is_str()) {
        try { return Value::integer(std::stoll(v.as_str())); }
        catch (...) { error = "int(): '" + v.as_str() + "' is not a number"; }
        return Value::null();
    }
    error = std::string("int() does not apply to ") + v.type_name();
    return Value::null();
}

// range(n) -> [0, n); range(start, end) -> [start, end); range(start, end,
// step) -> stepped, matching Python's range() shape (the one language
// Lux Script draws its other iteration/truthiness rules from too, see
// GUIDE.md's Truthiness section). Eager, not lazy: it builds the whole
// List up front, exactly what `for x in [1, 2, 3]` already does over a
// literal, and there is no lazy-sequence concept anywhere else in the
// language to make range() the one exception to.
Value fn_range(NativeCtx&, std::vector<Value>& args, std::string& error) {
    for (auto& a : args)
        if (!a.is_int()) { error = "range() expects int arguments"; return Value::null(); }

    long long start = 0, end, step = 1;
    if (args.size() == 1)      end = args[0].as_int();
    else if (args.size() == 2) { start = args[0].as_int(); end = args[1].as_int(); }
    else                       { start = args[0].as_int(); end = args[1].as_int(); step = args[2].as_int(); }

    if (step == 0) { error = "range(): step cannot be 0"; return Value::null(); }

    Value::List out;
    if (step > 0) for (long long i = start; i < end; i += step) out.push_back(Value::integer(i));
    else          for (long long i = start; i > end; i += step) out.push_back(Value::integer(i));
    return Value::list(std::move(out));
}

// Element handles travel as a one-key Dict; every operation re-validates the
// handle through the Host, so a forged or stale one is a clean script error.
bool element_handle(const Value& recv, long long& out) {
    if (!recv.is_dict()) return false;
    const auto& d = recv.as_dict();
    auto it = d.find(kElementKey);
    if (it == d.end() || !it->second.is_int()) return false;
    out = it->second.as_int();
    return true;
}

Value el_text(NativeCtx& ctx, Value& recv, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    return ctx.host->element_text(h);
}
Value el_set_text(NativeCtx& ctx, Value& recv, std::vector<Value>& args, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    if (!args[0].is_str()) { error = "set_text() expects a string"; return Value::null(); }
    std::string err;
    if (!ctx.host->element_set_text(h, args[0].as_str(), err)) { error = err; return Value::null(); }
    return recv;
}
Value el_click(NativeCtx& ctx, Value& recv, std::vector<Value>& args, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    if (!args[0].is_func()) { error = "click() expects a function: el.click(fn)"; return Value::null(); }
    std::string err;
    if (!ctx.host->element_click(h, args[0], err)) { error = err; return Value::null(); }
    return recv;
}
Value el_attr(NativeCtx& ctx, Value& recv, std::vector<Value>& args, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    if (!args[0].is_str()) { error = "attr() expects the attribute name as a string"; return Value::null(); }
    return ctx.host->element_attr(h, args[0].as_str());
}
Value el_set_attr(NativeCtx& ctx, Value& recv, std::vector<Value>& args, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    if (!args[0].is_str() || !args[1].is_str()) { error = "set_attr() expects two strings"; return Value::null(); }
    std::string err;
    if (!ctx.host->element_set_attr(h, args[0].as_str(), args[1].as_str(), err)) { error = err; return Value::null(); }
    return recv;
}
Value el_tag(NativeCtx& ctx, Value& recv, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    return Value::str(ctx.host->element_tag(h));
}
Value el_id(NativeCtx& ctx, Value& recv, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    return Value::str(ctx.host->element_id(h));
}
Value el_child_count(NativeCtx& ctx, Value& recv, std::string& error) {
    long long h;
    if (!element_handle(recv, h)) { error = "internal: lost the element handle"; return Value::null(); }
    return Value::integer(ctx.host->element_child_count(h));
}

namespace {

bool want(size_t got, size_t min, size_t max, const std::string& name,
          std::string& error) {
    if (got >= min && got <= max) return true;
    error = "'" + name + "()' received a number of arguments it does not accept";
    return false;
}

// Saves an uploaded part keeping only the file name, with no path: that way a
// filename with ".." or an absolute one cannot escape the directory.
std::string safe_name(const std::string& raw) {
    // Truncate at the first NUL rather than stripping/replacing it: whatever
    // comes after a NUL is invisible to every C API this name eventually
    // reaches (::open() below takes a C string via c_str()), so keeping it
    // in the C++-side value only makes the mismatch worse -- a name that
    // LOOKS like "evil.sh\0.png" to any .ends_with()/.contains() check an
    // app runs on it, but writes to disk as "evil.sh". Belt-and-suspenders
    // with multipart.hpp already doing the same at parse time: this makes
    // save() safe regardless of where its filename argument came from, not
    // just the multipart path.
    std::string raw_trunc = raw.substr(0, raw.find('\0'));
    size_t slash = raw_trunc.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? raw_trunc : raw_trunc.substr(slash + 1);
    if (base.empty() || base == "." || base == "..") base = "subida";
    return base;
}

// Splits "name.ext" into {"name", ".ext"}. No dot, or a dot-only hidden
// file like ".bashrc", keeps the whole thing as the stem and an empty ext.
std::pair<std::string, std::string> split_ext(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {name, ""};
    return {name.substr(0, dot), name.substr(dot)};
}

// Clamps a [start, end) range to a valid slice of something `len` long --
// shared by string.slice() and List.slice(). A negative or out-of-range
// bound is clamped rather than rejected (Python's own slicing does the
// same: s[-100:100] on a 5-char string is not an error, it is the whole
// string) -- errors are for genuinely wrong TYPES of argument, not for a
// range that simply does not need clamping.
void clamp_range(long long len, long long& start, long long& end) {
    if (start < 0) start = std::max<long long>(0, len + start);
    if (end   < 0) end   = std::max<long long>(0, len + end);
    start = std::min(start, len);
    end   = std::min(end, len);
    if (end < start) end = start;
}

// Invokes a Value::Func (List.map/filter/reduce/for_each's callback)
// synchronously, from inside call_method() -- itself already a synchronous
// C++ function called from the VM's own opcode dispatch loop, so there is
// no "suspend and let the driver resume us later" available here the way a
// real route handler has. A NESTED, re-entrant VM::start() is the answer:
// the same idea render_template() (template.cpp) uses to let a template
// expression call a user function.
//
// A FRESH VM per call, not a shared/thread_local one: VM::start() clears
// frames_/stack_ unconditionally on entry (vm.cpp), so a shared instance
// reused for a NESTED call -- a callback passed to .map() that itself calls
// .map() again, on the same thread, easy to write by accident -- would wipe
// the outer call's still-in-progress state out from under it while it is
// blocked waiting for the inner call to return. A fresh VM costs a few
// small allocations per call; that is a real, provable bug otherwise, not
// a hypothetical one, so correctness wins over an optimization this new
// feature never had a measured need for in the first place.
//
// `error` covers two different failures: the callback threw a normal Lux
// Script runtime error (division by zero, wrong arity, ...) -- passed
// straight through, so a callback's own error is what the caller sees, not
// a wrapper -- or the callback tried to `await` something. That second one
// cannot work here (Status::Suspended has nowhere to go: this call has no
// event loop, no coroutine, nothing to resume it later) and is reported
// with a clear, specific message instead of hanging, crashing, or silently
// returning null.
Value call_func_value(NativeCtx& ctx, const Value& fn, std::vector<Value> args,
                      const char* caller, std::string& error) {
    if (!ctx.functions || fn.as_func_index() < 0 ||
        static_cast<size_t>(fn.as_func_index()) >= ctx.functions->size()) {
        error = std::string(caller) + "(): the function reference is not valid here";
        return Value::null();
    }
    VM vm;
    const Chunk& chunk = *(*ctx.functions)[static_cast<size_t>(fn.as_func_index())];
    VM::Result r = vm.start(chunk, std::move(args), ctx, ctx.functions);
    if (r.status == VM::Status::Suspended) {
        error = std::string(caller) + "(): the function passed to it used `await`, "
                "which is not supported here";
        return Value::null();
    }
    if (r.status == VM::Status::Error) { error = r.error; return Value::null(); }
    return std::move(r.value);
}

} // namespace


// ── The table ────────────────────────────────────────────────────────────────
// Ids are table indexes; the emitter bakes them into the bytecode.  Nothing
// here is asynchronous: the browser's M-demo has no awaitables, and a page
// that writes `await` gets a compile error at the call site, not a hang.

const std::array<NativeDef, 14> kNatives = {{
    // Utilities (same semantics as the server's)
    {"len",   1, 1, fn_len},
    {"str",   1, 1, fn_str},
    {"int",   1, 1, fn_int},
    {"float", 1, 1, fn_float},
    {"range", 1, 3, fn_range},
    // log.*
    {"__log_info",  1, 1, fn_log_info},
    {"__log_warn",  1, 1, fn_log_warn},
    {"__log_error", 1, 1, fn_log_error},
    // document.*
    {"__document_get",       1, 1, fn_document_get},
    {"__document_query_all", 1, 1, fn_document_query_all},
    {"__document_title",     0, 0, fn_document_title},
    {"__document_root",      0, 0, fn_document_root},
    // page.* -- the page-scoped store
    {"__page_get",           1, 1, fn_page_get},
    {"__page_set",           2, 2, fn_page_set},
}};

} // namespace

int native_id(const std::string& name) {
    for (size_t i = 0; i < kNatives.size(); ++i)
        if (name == kNatives[i].name) return static_cast<int>(i);
    return -1;
}

const NativeDef& native_at(int id) { return kNatives[static_cast<size_t>(id)]; }

NativeCtx*& current_native_ctx() {
    thread_local NativeCtx* ctx = nullptr;
    return ctx;
}

Value call_method(NativeCtx& ctx, Value& recv, const std::string& name,
                  std::vector<Value>& args, std::string& error) {
    // ── Element ──────────────────────────────────────────────────────────
    long long el;
    if (element_handle(recv, el)) {
        if (name == "text")        return el_text(ctx, recv, error);
        if (name == "set_text")    return el_set_text(ctx, recv, args, error);
        if (name == "click")       return el_click(ctx, recv, args, error);
        if (name == "attr")        return el_attr(ctx, recv, args, error);
        if (name == "set_attr")    return el_set_attr(ctx, recv, args, error);
        if (name == "tag")         return el_tag(ctx, recv, error);
        if (name == "id")          return el_id(ctx, recv, error);
        if (name == "child_count") return el_child_count(ctx, recv, error);
        error = "Elements have no method '" + name + "'";
        return Value::null();
    }

    // ── The stdlib, Lux's own code ───────────────────────────────────────
    // ── string ───────────────────────────────────────────────────────────────
    if (recv.is_str()) {
        const std::string& s = recv.as_str();
        if (name == "starts_with" || name == "ends_with" || name == "contains") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "'" + name + "()' expects un string"; return Value::null(); }
            const std::string& n = args[0].as_str();
            if (name == "starts_with") return Value::boolean(s.rfind(n, 0) == 0);
            if (name == "ends_with")
                return Value::boolean(s.size() >= n.size() &&
                                      s.compare(s.size() - n.size(), n.size(), n) == 0);
            return Value::boolean(s.find(n) != std::string::npos);
        }
        if (name == "upper" || name == "lower") {
            return Value::str(name == "upper" ? utf8_upper(s) : utf8_lower(s));
        }
        if (name == "trim") return Value::str(trim_ascii_ws(s));
        // Byte offsets, not Unicode codepoints -- matching how the rest of
        // the runtime already treats strings (value.cpp's escape_json/
        // utf8_seq_len work byte-wise too). Correct for ASCII, and for
        // multi-byte UTF-8 as long as a cut does not land mid-sequence --
        // the same tradeoff the language already made, not a new one.
        if (name == "index_of") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "'index_of()' expects a string"; return Value::null(); }
            size_t pos = s.find(args[0].as_str());
            return Value::integer(pos == std::string::npos ? -1 : static_cast<long long>(pos));
        }
        if (name == "replace") {
            if (!want(args.size(), 2, 2, name, error)) return Value::null();
            if (!args[0].is_str() || !args[1].is_str()) {
                error = "'replace()' expects two strings"; return Value::null();
            }
            const std::string& from = args[0].as_str();
            if (from.empty()) { error = "'replace()': the text to replace cannot be empty"; return Value::null(); }
            const std::string& to = args[1].as_str();
            std::string out;
            size_t pos = 0, prev = 0;
            while ((pos = s.find(from, prev)) != std::string::npos) {
                out.append(s, prev, pos - prev);
                out += to;
                prev = pos + from.size();
            }
            out.append(s, prev, std::string::npos);
            return Value::str(std::move(out));
        }
        if (name == "split") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "'split()' expects a string"; return Value::null(); }
            const std::string& sep = args[0].as_str();
            // An empty separator splits into individual characters --
            // codepoints, not bytes, via utf8_chars() (value.hpp), the same
            // thing `for c in <string>` iterates -- instead of the
            // "separator cannot be empty" error this used to be
            // unconditionally: `s.split("")` is the one way Lux Script has
            // to build a slugify/character-by-character transform at all
            // (there is no other character-iteration form of `for`), and
            // rejecting it left that with no answer.
            if (sep.empty()) {
                Value::List out;
                for (auto& ch : utf8_chars(s)) out.push_back(Value::str(std::move(ch)));
                return Value::list(std::move(out));
            }
            Value::List out;
            size_t pos = 0, prev = 0;
            while ((pos = s.find(sep, prev)) != std::string::npos) {
                out.push_back(Value::str(s.substr(prev, pos - prev)));
                prev = pos + sep.size();
            }
            out.push_back(Value::str(s.substr(prev)));
            return Value::list(std::move(out));
        }
        if (name == "slice") {
            if (!want(args.size(), 1, 2, name, error)) return Value::null();
            if (!args[0].is_int() || (args.size() > 1 && !args[1].is_int())) {
                error = "'slice()' expects int bounds"; return Value::null();
            }
            long long len = static_cast<long long>(s.size());
            long long start = args[0].as_int();
            long long end   = args.size() > 1 ? args[1].as_int() : len;
            clamp_range(len, start, end);
            return Value::str(s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
        }
        if (name == "repeat") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_int()) { error = "'repeat()' expects an int"; return Value::null(); }
            long long n = args[0].as_int();
            if (n < 0) { error = "'repeat()': the count cannot be negative"; return Value::null(); }
            std::string out;
            out.reserve(s.size() * static_cast<size_t>(n));
            for (long long i = 0; i < n; ++i) out += s;
            return Value::str(std::move(out));
        }
        error = "strings have no method '" + name + "'";
        return Value::null();
    }

    // ── List ─────────────────────────────────────────────────────────────────
    if (recv.is_list()) {
        auto& l = recv.as_list();
        if (name == "add") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            l.push_back(args[0]);
            return recv;
        }
        if (name == "contains" || name == "index_of") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            for (size_t i = 0; i < l.size(); ++i) {
                if (l[i].equals(args[0]))
                    return name == "contains" ? Value::boolean(true) : Value::integer(static_cast<long long>(i));
            }
            return name == "contains" ? Value::boolean(false) : Value::integer(-1);
        }
        if (name == "remove_at") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_int()) { error = "'remove_at()' expects an int"; return Value::null(); }
            long long i = args[0].as_int();
            if (i < 0 || i >= static_cast<long long>(l.size())) return Value::boolean(false);
            l.erase(l.begin() + i);
            return Value::boolean(true);
        }
        // In-place, natural order only: numbers ascending, strings
        // lexicographic, via Value::less_than() -- the same comparison `<`
        // itself uses (see vm.cpp's compare()). There is no custom-
        // comparator overload (sort by a key, reverse order via a callback)
        // because Lux Script has no function values to pass one with
        // (NATIVE-MODULES.md) -- reverse() right below covers the one
        // custom order that matters often enough to special-case.
        if (name == "sort") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            bool ok = true;
            std::stable_sort(l.begin(), l.end(), [&ok](const Value& a, const Value& b) {
                bool this_ok = true;
                bool r = a.less_than(b, this_ok);
                if (!this_ok) ok = false;
                return r;
            });
            if (!ok) { error = "sort(): the List has values that cannot be compared with each other"; return Value::null(); }
            return recv;
        }
        if (name == "reverse") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            std::reverse(l.begin(), l.end());
            return recv;
        }
        if (name == "slice") {
            if (!want(args.size(), 1, 2, name, error)) return Value::null();
            if (!args[0].is_int() || (args.size() > 1 && !args[1].is_int())) {
                error = "'slice()' expects int bounds"; return Value::null();
            }
            long long len = static_cast<long long>(l.size());
            long long start = args[0].as_int();
            long long end   = args.size() > 1 ? args[1].as_int() : len;
            clamp_range(len, start, end);
            return Value::list(Value::List(l.begin() + start, l.begin() + end));
        }
        if (name == "concat") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_list()) { error = "'concat()' expects a List"; return Value::null(); }
            Value::List out = l;
            const auto& other = args[0].as_list();
            out.insert(out.end(), other.begin(), other.end());
            return Value::list(std::move(out));
        }
        if (name == "join") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "'join()' expects a string separator"; return Value::null(); }
            const std::string& sep = args[0].as_str();
            std::string out;
            for (size_t i = 0; i < l.size(); ++i) {
                if (i) out += sep;
                out += l[i].to_string();
            }
            return Value::str(std::move(out));
        }
        // map/filter/reduce/for_each: the one place Value::Type::Func
        // (value.hpp) is actually used -- a `fn` passed by reference, no
        // captured environment (it is not a closure), invoked once per
        // element via call_func_value() above. That is also why there is
        // no sort-by-key or a custom comparator anywhere in this file: the
        // language could not express one before these four existed, and
        // these are deliberately the smallest, most-asked-for slice of
        // "pass a function around" rather than the full feature.
        if (name == "map") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'map()' expects a function"; return Value::null(); }
            Value::List out;
            out.reserve(l.size());
            for (auto& item : l) {
                Value r = call_func_value(ctx, args[0], {item}, "map", error);
                if (!error.empty()) return Value::null();
                out.push_back(std::move(r));
            }
            return Value::list(std::move(out));
        }
        if (name == "filter") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'filter()' expects a function"; return Value::null(); }
            Value::List out;
            for (auto& item : l) {
                Value keep = call_func_value(ctx, args[0], {item}, "filter", error);
                if (!error.empty()) return Value::null();
                if (keep.truthy()) out.push_back(item);
            }
            return Value::list(std::move(out));
        }
        if (name == "reduce") {
            if (!want(args.size(), 2, 2, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'reduce()' expects a function"; return Value::null(); }
            Value acc = args[1];
            for (auto& item : l) {
                acc = call_func_value(ctx, args[0], {acc, item}, "reduce", error);
                if (!error.empty()) return Value::null();
            }
            return acc;
        }
        if (name == "for_each") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'for_each()' expects a function"; return Value::null(); }
            for (auto& item : l) {
                call_func_value(ctx, args[0], {item}, "for_each", error);
                if (!error.empty()) return Value::null();
            }
            return recv;
        }
        // sort_by/find/find_index: the same "pass a fn, no closure" shape as
        // map/filter/reduce/for_each above, added because sort()'s natural-
        // order-only limit (comment above it) and index_of()'s equals-only
        // match are exactly the two gaps a hand-rolled loop keeps getting
        // reintroduced for (sort a list of Dicts by one field, find the
        // first element matching more than a single equals check).
        if (name == "sort_by") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'sort_by()' expects a function"; return Value::null(); }
            std::vector<Value> keys;
            keys.reserve(l.size());
            for (auto& item : l) {
                Value k = call_func_value(ctx, args[0], {item}, "sort_by", error);
                if (!error.empty()) return Value::null();
                keys.push_back(std::move(k));
            }
            std::vector<size_t> order(l.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            bool ok = true;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                bool this_ok = true;
                bool r = keys[a].less_than(keys[b], this_ok);
                if (!this_ok) ok = false;
                return r;
            });
            if (!ok) { error = "sort_by(): the key values cannot be compared with each other"; return Value::null(); }
            Value::List out;
            out.reserve(l.size());
            for (size_t i : order) out.push_back(l[i]);
            l = std::move(out);
            return recv;
        }
        if (name == "find") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'find()' expects a function"; return Value::null(); }
            for (auto& item : l) {
                Value keep = call_func_value(ctx, args[0], {item}, "find", error);
                if (!error.empty()) return Value::null();
                if (keep.truthy()) return item;
            }
            return Value::null();
        }
        if (name == "find_index") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'find_index()' expects a function"; return Value::null(); }
            for (size_t i = 0; i < l.size(); ++i) {
                Value keep = call_func_value(ctx, args[0], {l[i]}, "find_index", error);
                if (!error.empty()) return Value::null();
                if (keep.truthy()) return Value::integer(static_cast<long long>(i));
            }
            return Value::integer(-1);
        }
        if (name == "first" || name == "last") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            return l.empty() ? Value::null() : name == "first" ? l.front() : l.back();
        }
        if (name == "pop") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            if (l.empty()) return Value::null();
            Value v = std::move(l.back());
            l.pop_back();
            return v;
        }
        if (name == "insert") {
            if (!want(args.size(), 2, 2, name, error)) return Value::null();
            if (!args[0].is_int()) { error = "'insert()' expects an int position"; return Value::null(); }
            const long long n = static_cast<long long>(l.size());
            long long i = args[0].as_int() < 0 ? args[0].as_int() + n : args[0].as_int();
            l.insert(l.begin() + std::clamp(i, 0LL, n), args[1]);
            return recv;
        }
        // Numbers only; stays an int while every value is one.
        if (name == "sum") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            long long ints = 0; double total = 0; bool all_int = true;
            for (const Value& v : l) {
                if (!v.is_num()) { error = "sum(): every value must be a number"; return Value::null(); }
                all_int = all_int && v.is_int();
                if (v.is_int()) ints += v.as_int(); else total += v.as_float();
            }
            return all_int ? Value::integer(ints) : Value::real(total + static_cast<double>(ints));
        }
        if (name == "min" || name == "max") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            const Value* best = nullptr;
            for (const Value& v : l) {
                bool ok = true;
                if (!best || (name == "min" ? v.less_than(*best, ok) : best->less_than(v, ok))) best = &v;
                if (!ok) { error = name + "(): the List has values that cannot be compared with each other"; return Value::null(); }
            }
            return best ? *best : Value::null();
        }
        // First occurrence of each value, in order.
        if (name == "unique") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            std::unordered_set<std::string> seen;
            Value::List out;
            for (const Value& v : l)
                if (seen.insert(v.to_json_text()).second) out.push_back(v);
            return Value::list(std::move(out));
        }
        // any/all/count with a predicate, or on the values' truthiness.
        if (name == "any" || name == "all" || name == "count") {
            if (!want(args.size(), 0, 1, name, error)) return Value::null();
            if (!args.empty() && !args[0].is_func()) { error = "'" + name + "()' expects a function"; return Value::null(); }
            long long hits = 0;
            for (const Value& v : l) {
                const bool yes = args.empty() ? v.truthy() : call_func_value(ctx, args[0], {v}, name.c_str(), error).truthy();
                if (!error.empty()) return Value::null();
                hits += yes;
                if (name == "any" && yes) return Value::boolean(true);
                if (name == "all" && !yes) return Value::boolean(false);
            }
            return name == "count" ? Value::integer(hits) : Value::boolean(name == "all");
        }
        // {key: [items...]} by what fn returns for each item.
        if (name == "group_by") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_func()) { error = "'group_by()' expects a function"; return Value::null(); }
            Value::Dict out;
            for (const Value& v : l) {
                Value key = call_func_value(ctx, args[0], {v}, name.c_str(), error);
                if (!error.empty()) return Value::null();
                Value& bucket = out[key.to_string()];
                if (!bucket.is_list()) bucket = Value::list();
                bucket.as_list().push_back(v);
            }
            return Value::dict(std::move(out));
        }
        // Lists of at most n items -- batching, table rows.
        if (name == "chunk") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_int() || args[0].as_int() < 1) { error = "'chunk()' expects a positive int"; return Value::null(); }
            const size_t n = static_cast<size_t>(args[0].as_int());
            Value::List out;
            for (size_t i = 0; i < l.size(); i += n)
                out.push_back(Value::list(Value::List(l.begin() + i, l.begin() + std::min(l.size(), i + n))));
            return Value::list(std::move(out));
        }
        error = "Lists have no method '" + name + "'";
        return Value::null();
    }

    // ── Dict, File included ──────────────────────────────────────────────────
    if (recv.is_dict()) {
        auto& d = recv.as_dict();

        if (name == "has") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            return Value::boolean(d.count(args[0].to_string()) > 0);
        }
        if (name == "keys") {
            Value::List ks;
            for (const auto& [k, _] : d) if (k.rfind("__", 0) != 0) ks.push_back(Value::str(k));
            return Value::list(std::move(ks));
        }
        if (name == "values") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            Value::List vs;
            for (const auto& [k, v] : d) if (k.rfind("__", 0) != 0) vs.push_back(v);
            return Value::list(std::move(vs));
        }
        if (name == "get") {
            if (!want(args.size(), 1, 2, name, error)) return Value::null();
            auto it = d.find(args[0].to_string());
            if (it != d.end()) return it->second;
            return args.size() > 1 ? args[1] : Value::null();
        }
        if (name == "remove") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            return Value::boolean(d.erase(args[0].to_string()) > 0);
        }
        // Mutates and returns the receiver, same convention as add(): a
        // key present in both wins from `other`, matching how Lux's own
        // Dict literal would behave if the same key were written twice.
        if (name == "merge") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_dict()) { error = "'merge()' expects a Dict"; return Value::null(); }
            for (const auto& [k, v] : args[0].as_dict()) d[k] = v;
            return recv;
        }
        // [[key, value], ...] -- for iterating both at once.
        if (name == "items") {
            if (!want(args.size(), 0, 0, name, error)) return Value::null();
            Value::List out;
            for (const auto& [k, v] : recv.as_dict()) out.push_back(Value::list({Value::str(k), v}));
            return Value::list(std::move(out));
        }
        error = "Dicts have no method '" + name + "'";
        return Value::null();
    }

    error = std::string("values of type ") + recv.type_name() +
            " have no methods";
    return Value::null();

    error = std::string("values of type ") + recv.type_name() +
            " have no methods";
    return Value::null();

    error = std::string("values of type ") + recv.type_name() +
            " have no methods";
    return Value::null();
}

// The compile-time face of call_method().  Same rule as the server's: a
// method added to the runtime above has to appear here, or the checker
// rejects it before the page ever loads.
const std::vector<BuiltinMethod>* methods_of(const std::string& type) {
    static const std::vector<BuiltinMethod> kElement = {
        {"text",        0, 0, "string"},
        {"set_text",    1, 1, nullptr},
        {"click",       1, 1, nullptr},
        {"attr",        1, 1, "string"},
        {"set_attr",    2, 2, nullptr},
        {"tag",         0, 0, "string"},
        {"id",          0, 0, "string"},
        {"child_count", 0, 0, "int"},
    };
    static const std::vector<BuiltinMethod> kString = {
        {"starts_with", 1, 1, "bool"}, {"ends_with", 1, 1, "bool"},
        {"contains", 1, 1, "bool"},    {"upper", 0, 0, "string"},
        {"lower", 0, 0, "string"},     {"trim", 0, 0, "string"},
        {"index_of", 1, 1, "int"},     {"replace", 2, 2, "string"},
        {"split", 1, 1, "List"},       {"slice", 1, 2, "string"},
        {"repeat", 1, 1, "string"},
    };
    static const std::vector<BuiltinMethod> kList = {
        {"add", 1, 1, nullptr},        {"contains", 1, 1, "bool"},
        {"index_of", 1, 1, "int"},     {"remove_at", 1, 1, "bool"},
        {"sort", 0, 0, nullptr},       {"reverse", 0, 0, nullptr},
        {"slice", 1, 2, "List"},       {"concat", 1, 1, "List"},
        {"join", 1, 1, "string"},
        {"map", 1, 1, "List"},         {"filter", 1, 1, "List"},
        {"reduce", 2, 2, "Json"},      {"for_each", 1, 1, nullptr},
        {"sort_by", 1, 1, nullptr},    {"find", 1, 1, "Json"},
        {"find_index", 1, 1, "int"},
        {"first", 0, 0, "Json"},       {"last", 0, 0, "Json"},
        {"pop", 0, 0, "Json"},         {"insert", 2, 2, nullptr},
        {"sum", 0, 0, "Json"},         {"min", 0, 0, "Json"},
        {"max", 0, 0, "Json"},         {"unique", 0, 0, "List"},
        {"any", 0, 1, "bool"},         {"all", 0, 1, "bool"},
        {"count", 0, 1, "int"},        {"group_by", 1, 1, "Dict"},
        {"chunk", 1, 1, "List"},
    };
    static const std::vector<BuiltinMethod> kDict = {
        {"has", 1, 1, "bool"},   {"keys", 0, 0, "List"},
        {"values", 0, 0, "List"}, {"get", 1, 2, "Json"}, {"remove", 1, 1, "bool"},
        {"merge", 1, 1, nullptr}, {"items", 0, 0, "List"},
    };

    if (type == "Element") return &kElement;
    if (type == "string")  return &kString;
    if (type == "List")    return &kList;
    if (type == "Dict")    return &kDict;
    if (type == "int" || type == "long" || type == "float" ||
        type == "double" || type == "bool")
        return nullptr;   // numbers and bools have no methods of their own
    return nullptr;
}

namespace {
struct MemberMap { const char* object; const char* member; const char* native; };

// The reserved objects of the browser, in full.  The server's table had
// request/response/session/jwt/state/sse/ws/db/error; a browser page has
// log and document, and that is the sandbox.
constexpr std::array<MemberMap, 9> kMembers = {{
    {"log",      "info",      "__log_info"},
    {"log",      "warn",      "__log_warn"},
    {"log",      "error",     "__log_error"},
    {"document", "get",       "__document_get"},
    {"document", "query_all", "__document_query_all"},
    {"document", "title",     "__document_title"},
    {"document", "root",      "__document_root"},
    {"page",     "get",       "__page_get"},
    {"page",     "set",       "__page_set"},
}};
} // namespace

int member_native_id(const std::string& object, const std::string& member) {
    for (const auto& m : kMembers)
        if (object == m.object && member == m.member) return native_id(m.native);
    return -1;
}

bool is_reserved_object(const std::string& name) {
    for (const auto& m : kMembers) if (name == m.object) return true;
    return false;
}

bool is_db_module(const std::string&) {
    return false;   // no database modules exist in a browser
}

} // namespace lux_script
