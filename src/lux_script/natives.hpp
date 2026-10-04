#pragma once
// Luxium's face of lux_script/natives.hpp.
//
// Lux the server reaches the world through lux::Request/lux::Response and a
// pile of server builtins; a browser must reach almost nothing (see
// docs/ARCHITECTURE.md §4.3 -- the sandbox IS this header).  So this is a
// rewrite, not a copy: the language frontend and VM are Lux's, verbatim, but
// the only door out of the VM is `Host`, the bridge the page implements.
//
// What a page's script can do, in full:
//   str/int/float/len/range     -- the stdlib, same semantics as the server
//   log.info/warn/error         -- the page's console pane
//   document.get/query_all/title/root/create
//   page.get/page.set (store), page.after (timers), page.clicked (in handlers)
//   <Element>.text/set_text/click/append/remove/attr/set_attr/tag/id/child_count
// and nothing else: there is no fs, no net, no db, no state, no session.

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

    virtual Value document_get(const std::string& id)                = 0;  // Element or null
    virtual Value document_query_all(const std::string& tag)         = 0;  // List<Element>
    virtual Value document_root()                                    = 0;  // Element
    virtual Value document_create(const std::string& tag)            = 0;  // detached Element
    virtual std::string document_title()                             = 0;

    // The page-scoped store: the browser equivalent of the server's
    // state.* -- the one place handlers share mutable state, because a
    // script's top level is a fn body and its locals die with it.
    virtual Value       page_get(const std::string& key)             = 0;
    virtual void        page_set(const std::string& key, Value v)    = 0;
    virtual void        page_after(int ms, const Value& fn)          = 0;   // one-shot timer
    virtual Value       page_clicked()                               = 0;   // Element, inside a handler

    virtual Value       element_text(long long handle)               = 0;
    virtual bool        element_set_text(long long h, const std::string& t, std::string& err) = 0;
    virtual bool        element_click(long long h, const Value& fn, std::string& err) = 0;
    virtual bool        element_append(long long parent, long long child, std::string& err) = 0;
    virtual bool        element_remove(long long h, std::string& err)  = 0;
    virtual Value       element_attr(long long h, const std::string& name) = 0;
    virtual bool        element_set_attr(long long h, const std::string& n,
                                         const std::string& v, std::string& err) = 0;
    virtual std::string element_tag(long long handle)                = 0;
    virtual std::string element_id(long long handle)                 = 0;
    virtual long long   element_child_count(long long handle)        = 0;
};

// The context the VM hands every builtin.  Where the server carried a
// request and a response, the browser carries the page's Host.
struct NativeCtx {
    Host*                host      = nullptr;
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
};

// The compile-time face of call_method(): the methods that exist for a Lux
// Script type ("string", "Element", ...).  nullptr = no closed list.
const std::vector<BuiltinMethod>* methods_of(const std::string& type);

bool is_reserved_object(const std::string& name);
bool is_db_module(const std::string& name);

// The "error" abort() raises on the server; the browser has no abort, but the
// VM's unwinding machinery still references the sentinel.
inline constexpr const char* kAbortMessage = "\x01" "abort";

// The VM records the context it is running with (vm.cpp's Op::CallNative) so
// re-entrant calls (a callback running a nested VM) can find it.
NativeCtx*& current_native_ctx();

// Elements travel into scripts as a Dict with this one key; call_method()
// dispatches on it and the Host validates the handle on every operation.
inline const char* kElementKey = "__luxium_element";

} // namespace lux_script
