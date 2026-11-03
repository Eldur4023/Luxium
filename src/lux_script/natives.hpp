#pragma once
// Luxium's face of lux_script/natives.hpp.
//
// Lux the server reaches the world through lux::Request/lux::Response and a
// pile of server builtins; a browser must reach almost nothing (see
// docs/ARCHITECTURE.md §4.3 -- the sandbox IS this header).  So this is a
// rewrite, not a copy: the language frontend and VM are Lux's, verbatim, but
// the only door out of the VM is `Host`, the bridge the page implements.
//
// What a page's script can do, in full: the table in browser_api.inc and
// nothing else -- there is no fs, no db, no state, no session; the network is
// `http`/`ws` and is same-origin.  docs/LUXSCRIPT.md §3.1 lists it.

#include <string>
#include <vector>

#include "bytecode.hpp"
#include "value.hpp"

namespace lux_script {

// The bridge every browser builtin talks to.  The engine (engine.hpp)
// implements it per page; element handles are ints the host validates on
// every use -- a stale handle fails loudly, never crashes.
struct Host {
    virtual ~Host() = default;

    // 0 = info, 1 = warn, 2 = error.
    virtual void log(int level, const std::string& msg) = 0;

    // Every reserved-object member of browser_api.inc (LUX_BROWSER_CALL) lands
    // here by its "<object>_<member>" name ("document_get", "http_post"); the
    // engine owns the dispatch table.  A failure is written to `error`.
    // Async members are not called through here: the VM suspends and the
    // engine's driver resumes it (script/engine.cpp, start_async).
    virtual Value call_native(const char* name, std::vector<Value>& args,
                              std::string& error) = 0;

    // A method on an object handle (Element, Response, Socket, Form, File).
    virtual Value call_method(const std::string& type, long long handle,
                              const std::string& method, std::vector<Value>& args,
                              std::string& error) = 0;
};

// The context the VM hands every builtin.  Where the server carried a
// request and a response, the browser carries the page's Host.
struct Template;

struct NativeCtx {
    Host*                host      = nullptr;
    // Compiled templates a render() in flight evaluates against (template.cpp).
    const std::vector<Template>* templates = nullptr;
    const FunctionTable* functions = nullptr;   // user fns, for List.map callbacks
};

// A builtin returns a Value and, if it fails, writes the reason into `error`.
using NativeFn = Value (*)(NativeCtx& ctx, std::vector<Value>& args,
                           std::string& error);

struct NativeDef {
    const char* name;
    int         min_args;
    int         max_args;    // -1 = no limit
    NativeFn    fn;          // null if it is asynchronous
    bool        is_async = false;
};

// Stable index of the builtin in the table, or -1 if it does not exist.  The
// emitter stores it in the instruction, so the VM never looks it up at runtime.
int              native_id(const std::string& name);
const NativeDef& native_at(int id);

// Resolves `object.member` of a reserved object (log.info, document.get...)
// to the builtin implementing it, or -1 if that combination does not exist.
int              member_native_id(const std::string& object, const std::string& member);

// The language's '+' on two strings (vm.cpp's Op::Add path for strings) --
// vm.cpp defines it; declared here for template.hpp's sake.
bool add_values(const Value& a, const Value& b, Value& out, std::string& err);

// Method on a value: "hi".starts_with(...), list.add(...), el.set_text(...).
// Unlike the reserved objects, the receiver is known at runtime, so the
// dispatch is by type inside the VM.
Value call_method(NativeCtx& ctx, Value& receiver, const std::string& name,
                  std::vector<Value>& args, std::string& error);

// One of the methods call_method() recognizes, with the arguments it takes and
// what it returns.
struct BuiltinMethod {
    const char* name;
    int         min_args;
    int         max_args;
    // Type of the result, or nullptr if it returns the receiver itself.
    const char* return_type;
    // Async method: the async native it lowers to (browser_api.inc AMETHOD).
    const char* async_native = nullptr;
};

// The compile-time face of call_method(): the methods that exist for a Lux
// Script type ("string", "Element", ...).  nullptr = no closed list.
const std::vector<BuiltinMethod>* methods_of(const std::string& type);

bool is_reserved_object(const std::string& name);
bool is_db_module(const std::string& name);

// Lux switches its compiler to the browser's API with this; here the tables
// already ARE the browser's (browser_api.inc), so it is a no-op kept for
// source compatibility (script/page_compile.cpp is shared with Lux).
class BrowserProfile {};
inline bool browser_profile_active_default() { return true; }

// The "error" abort() raises on the server; the browser has no abort, but the
// VM's unwinding machinery still references the sentinel.
inline constexpr const char* kAbortMessage = "\x01" "abort";

// The VM records the context it is running with (vm.cpp's Op::CallNative) so
// re-entrant calls (a callback running a nested VM) can find it.
NativeCtx*& current_native_ctx();

// Objects the host owns (Element, Response, Socket, Form, File) travel into
// scripts as a two-key Dict: the type name and an integer handle.  call_method()
// dispatches on it and the Host validates the handle on every operation, so a
// script cannot forge or keep alive what the host dropped.
inline const char* kObjType   = "__lxt";
inline const char* kObjHandle = "__lxh";
Value make_object(const std::string& type, long long handle);
// true + type/handle if `v` is such an object
bool  host_object_of(const Value& v, std::string& type, long long& handle);

bool browser_profile_active();

} // namespace lux_script
