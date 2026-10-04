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

struct Compiled {
    lux_script::FunctionTable functions;        // slot 0 = __luxium_main__
    lux_script::FunctionSigs  fn_sigs;
    lux_script::ClassSigs     class_sigs;
    lux_script::EnumSigs      enum_sigs;
    std::vector<int>          line_map;         // wrapped line -> document line (runtime errors)
    std::vector<std::string>  errors;           // "line N: message", document coordinates
    bool ok() const { return errors.empty(); }
};

// Compiles one script body.  `base_line` is the document line of the first
// source line, so diagnostics remap to where the script sits in the HTML.
std::unique_ptr<Compiled> compile_script(std::string_view source, int base_line);

// The real entry point: every <script> body of a page, in document order,
// compiled as ONE program (shared scope, one __luxium_main__ that calls each
// block's wrapper in order).  `blocks` = (body, document line of the body's
// first line); diagnostics remap to document coordinates.
std::unique_ptr<Compiled> compile_blocks(
    const std::vector<std::pair<std::string, int>>& blocks,
    const std::string& source_name);

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

    // Sets the program this engine runs.  A page compiles all its <script>
    // blocks INTO ONE program (one parser pass per block, shared tables in
    // document order), so a fn defined in the first block is visible to the
    // second -- the same-file semantics the server's module gives a project.
    void add_script(std::string_view source, int base_line);
    bool finalize();                 // true if everything compiled
    const std::vector<std::string>& errors() const { return errors_; }

    // Runs __luxium_main__ (top level).  Errors go to the page's console.
    void run_main();

    // Fires every handler registered for `event` on `handle`, in registration
    // order.  Returns false if a handler raised a script error (already
    // reported to the console).
    bool fire(const std::string& event, long long handle);

    // Shell bridge: the page hit-tested a click on `node`; fire its handlers.
    void click_node(dom::Node* node);

    // ── lux_script::Host ─────────────────────────────────────────────────
    void        log(int level, const std::string& msg) override;
    Value       document_get(const std::string& id) override;
    Value       document_query_all(const std::string& tag) override;
    Value       document_root() override;
    std::string document_title() override;
    Value       element_text(long long handle) override;
    bool        element_set_text(long long h, const std::string& t, std::string& err) override;
    bool        element_click(long long h, const Value& fn, std::string& err) override;
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
    std::vector<std::pair<std::string, int>> blocks_;        // source, document line
    Compiled*                                compiled_ = nullptr;
    std::vector<std::string>                 errors_;
    std::vector<EventHandler>                handlers_;
    long long                                next_handle_ = 1;
    std::unordered_map<long long, dom::Node*> nodes_;
};

} // namespace luxium::script
