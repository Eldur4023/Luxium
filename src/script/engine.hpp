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
// Element handles are indexes into the page's live node table, validated on
// every use: a script cannot forge one, and a removed node fails loudly.

#include <memory>
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

namespace luxium {

// What the script engine needs from the page it runs in.  The page model
// (page/page.hpp) implements it; the shell drives the page.
class PageHost {
public:
    virtual ~PageHost() = default;
    virtual void        console(int level, const std::string& msg) = 0;  // 0 info 1 warn 2 error
    virtual std::string page_title() const                         = 0;
    virtual dom::Node*  page_root()                                = 0;
    virtual void        page_mutated()                             = 0;  // relayout + repaint
    virtual std::string page_source_name() const                   = 0;  // "page.html", for diagnostics
};

} // namespace luxium

namespace luxium::script {

using lux_script::Value;

// The handler signature a page registers with el.on("click", fn): the engine
// runs the script's own function through a fresh VM.
struct EventHandler {
    long long         handle = -1;   // element handle
    std::string       event;         // "click" today
    lux_script::Value fn;            // function reference
};

class Engine : public lux_script::Host {
public:
    explicit Engine(class PageHost& page) : page_(page) {}
    ~Engine() { delete compiled_; }

    // Forget the previous document's program, handlers, timers, handles and
    // page.* store.  The page calls it on every load: without it a timer of
    // page A would fire into page B's function table.
    void reset();

    // The program this engine runs: a page's scripts compiled together
    // (page_compile.hpp, via page/luxp.hpp's translator) ...
    void set_program(std::unique_ptr<Compiled> program);
    // ... or a .luxp's bytecode, verified first (script/bytecode_io.hpp).
    // false + err if it is rejected.
    bool load_precompiled(const std::string& bytes, std::string* err);

    // Runs __luxium_main__ (top level).  Errors go to the page's console.
    void run_main();

    // Fires every handler registered for `event` on `handle`, in registration
    // order.  Returns false if a handler raised a script error (already
    // reported to the console).
    bool fire(const std::string& event, long long handle);

    // Shell bridge: the page hit-tested a click on `node`; fire its handlers.
    void click_node(const dom::Node* node);
    bool has_handler_for(const dom::Node* node) const;

    // The shell calls this every frame: fires timers whose deadline passed.
    void tick();

    // Frees what remove() took out of the DOM.  Only once nothing (layout,
    // hover) can still point into it: the shell calls it after a relayout.
    void free_removed() { removed_.clear(); }

    // ── lux_script::Host ─────────────────────────────────────────────────
    void        log(int level, const std::string& msg) override;
    Value       document_get(const std::string& id) override;
    Value       document_query_all(const std::string& tag) override;
    Value       document_root() override;
    std::string document_title() override;
    Value       document_create(const std::string& tag) override;
    Value       page_get(const std::string& key) override;
    void        page_set(const std::string& key, Value v) override;
    void        page_after(int ms, const Value& fn) override;
    Value       page_clicked() override;

    Value       element_text(long long handle) override;
    bool        element_set_text(long long h, const std::string& t, std::string& err) override;
    bool        element_click(long long h, const Value& fn, std::string& err) override;
    bool        element_append(long long parent, long long child, std::string& err) override;
    bool        element_remove(long long h, std::string& err) override;
    Value       element_attr(long long h, const std::string& name) override;
    bool        element_set_attr(long long h, const std::string& n,
                                 const std::string& v, std::string& err) override;
    std::string element_tag(long long handle) override;
    std::string element_id(long long handle) override;
    long long   element_child_count(long long handle) override;

private:
    // Bridges to the page; the page owns the DOM and the repaint dirty flag.
    dom::Node*   node_of(long long handle) const;
    long long    handle_of(dom::Node* node);
    void         script_error(const std::string& what, const lux_script::SourceLoc& loc);
    Value        make_element(dom::Node* node);
    bool         run_fn(size_t table_index, const char* what);

    lux_script::NativeCtx make_ctx() {
        lux_script::NativeCtx ctx;
        ctx.host      = this;
        ctx.functions = compiled_ ? &compiled_->functions : nullptr;
        return ctx;
    }

    PageHost&                                page_;
    Compiled*                                compiled_ = nullptr;
    std::vector<EventHandler>                handlers_;
    struct Timer { long long at = 0; Value fn; };
    std::vector<Timer>                       timers_;
    long long                                clicked_handle_ = -1;
    std::vector<std::unique_ptr<dom::Node>>  detached_;   // document.create() output
    std::vector<std::unique_ptr<dom::Node>>  removed_;    // remove() output, see free_removed()
    long long                                next_handle_ = 1;
    std::unordered_map<long long, dom::Node*> nodes_;
    std::unordered_map<std::string, Value>    page_state_;
};

} // namespace luxium::script
