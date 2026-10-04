#include "script/engine.hpp"

#include <dom/node.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>
#include <lux_script/emitter.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace luxium::script {

using lux_script::Chunk;
using lux_script::DiagnosticBag;
using lux_script::Emitter;
using lux_script::FnSig;
using lux_script::SourceFile;

namespace {

// Fills FnSig from a parameter list -- project.cpp's make_sig, minus the
// Response-type checks (there is no response in a browser for a script to
// leak: the checker rejects the type name long before this would matter).
FnSig make_sig(size_t index, const std::vector<lux_script::Param>& params,
               const lux_script::TypeRef& return_type, DiagnosticBag& diags) {
    FnSig sig;
    sig.index       = index;
    sig.return_type = lux_script::Type::from_declared(return_type);
    bool seen_default = false;
    for (const auto& p : params) {
        sig.defaults.push_back(p.default_value.get());
        if (p.default_value) seen_default = true;
        else {
            if (seen_default)
                diags.error(p.loc, "a parameter without a default cannot come "
                                   "after one that has one");
            ++sig.required;
        }
    }
    return sig;
}

// Strips the HTML margin from a script body: the page's own indentation
// wraps every line (a <script> inside <body> carries four or more spaces),
// and a lifted `fn` must reach the top level at column 0 -- else the lexer
// sees Indent tokens around it.  Same rule as the language's own
// strip_margin for triple-quoted strings: remove the common leading-space
// count of every non-blank line.  The line count never changes, so the
// diagnostics map stays valid; a line starting with a tab pins the margin
// at zero rather than guessing the tab's width.
std::string strip_block_margin(const std::string& body) {
    size_t margin = std::string::npos;
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        size_t end = (nl == std::string::npos) ? body.size() : nl;
        size_t j = i;
        while (j < end && body[j] == ' ') ++j;
        if (j < end) margin = std::min(margin, j - i);   // non-blank line
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    if (margin == std::string::npos || margin == 0) return body;

    std::string out;
    out.reserve(body.size());
    i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        size_t end = (nl == std::string::npos) ? body.size() : nl;
        size_t skip = std::min(margin, end - i);
        out.append(body, i + skip, end - i - skip);
        if (nl == std::string::npos) break;
        out += '\n';
        i = nl + 1;
    }
    return out;
}

struct Wrapped {
    SourceFile       file;
    std::vector<int> wrapped_to_doc;   // wrapped line (1-based) -> document line
};

// The wrapped source: every <script> body's statements go into ONE
// `fn void __luxium_main__():`, in document order -- two <script> blocks of
// the same page share scope exactly like two top-level scripts share
// globals in every browser.  `fn`/`class`/`enum` declarations may only live
// at a program's top level (the parser rejects them nested), so they are
// lifted out verbatim.
//
// The statements are re-indented by four spaces: relative indent inside the
// block is preserved, and a triple-quoted string's margin stripping works
// on the common margin, which shifts by the same four.  wrapped_to_doc
// remaps every wrapped line back to its document line, so a compile error
// reads as "line 37 of the page", not "line 12 of an invisible string".
Wrapped wrap_blocks(const std::vector<std::pair<std::string, int>>& blocks,
                    const std::string& path) {
    Wrapped w;
    w.file.path = path;
    std::vector<std::pair<std::string, int>> pending_decls;   // lifted fn/class/enum lines
    auto& text = w.file.text;
    auto push_line = [&](const std::string& s, int doc_line) {
        text += s;
        text += '\n';
        w.wrapped_to_doc.push_back(doc_line);
    };

    push_line("fn void __luxium_main__():", blocks.empty() ? 1 : blocks.front().second);

    for (size_t i = 0; i < blocks.size(); ++i) {
        const int base = blocks[i].second;
        const std::string body = strip_block_margin(blocks[i].first);

        // Which body lines are top-level DECLARATIONS?  `fn`/`class`/`enum`
        // (with their indented bodies) are lifted verbatim; everything else
        // runs inside __luxium_main__.  The body is lexed with the real
        // lexer, so Indent/Dedent decide -- no guessing from columns.
        SourceFile bf;
        bf.path = "block";
        bf.text = body;
        DiagnosticBag bd;
        lux_script::Lexer bl(bf, bd);
        auto toks = bl.tokenize();

        std::vector<int> is_decl(body.size() + 2, 0);
        {
            int depth      = 0;
            bool in_decl   = false;
            int  decl_from = 0;
            int  last_line = 1;
            for (const auto& t : toks) {
                switch (t.kind) {
                case lux_script::Tok::Indent:
                    if (in_decl) ++depth;
                    break;
                case lux_script::Tok::Dedent:
                    if (in_decl && depth > 0 && --depth == 0) {
                        for (int l = decl_from; l <= last_line && l < (int)is_decl.size(); ++l)
                            is_decl[l] = 1;
                        in_decl = false;
                    }
                    break;
                case lux_script::Tok::KwFn:
                case lux_script::Tok::KwClass:
                case lux_script::Tok::KwEnum:
                    if (!in_decl) { in_decl = true; depth = 0; decl_from = t.loc.line; }
                    break;
                case lux_script::Tok::EndOfFile:
                    if (in_decl) {
                        for (int l = decl_from; l <= last_line && l < (int)is_decl.size(); ++l)
                            is_decl[l] = 1;
                        in_decl = false;
                    }
                    break;
                default: break;
                }
                if (t.kind != lux_script::Tok::EndOfFile) last_line = t.loc.line;
            }
        }

        int doc = base;
        size_t start = 0;
        while (start <= body.size()) {
            size_t nl = body.find('\n', start);
            size_t end = (nl == std::string::npos) ? body.size() : nl;
            std::string line = body.substr(start, end - start);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (!is_decl[static_cast<size_t>(doc - base + 1)]) {
                push_line("    " + line, doc);
            } else {
                pending_decls.push_back({line, doc});
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
            ++doc;
        }
        push_line("", base);
    }

    // The lifted declarations, after main's body (all sigs exist before any
    // body is emitted, so blocks may call fns declared in later blocks).
    for (const auto& [line, doc] : pending_decls) push_line(line, doc);
    return w;
}

} // namespace

// ── compile driver ──────────────────────────────────────────────────────────

std::unique_ptr<Compiled> compile_blocks(const std::vector<std::pair<std::string, int>>& blocks,
                                         const std::string& path) {
    Wrapped w = wrap_blocks(blocks, path);

    DiagnosticBag diags;
    lux_script::Lexer lexer(w.file, diags);
    auto tokens = lexer.tokenize();
    lux_script::Parser parser(std::move(tokens), diags);
    lux_script::Program program;
    parser.parse_into(program);

    auto out = std::make_unique<Compiled>();

    // Function table + signatures, in project.cpp's order: user fns first
    // (in source order), then each class's methods and constructors.  All
    // sigs exist before any body is emitted, so a fn can call one declared
    // further down.
    for (const auto& f : program.functions) {
        out->fn_sigs[f.name] = make_sig(out->functions.size(), f.params, f.return_type, diags);
        out->functions.push_back(std::make_shared<Chunk>());
    }
    for (const auto& c : program.classes) {
        if (out->class_sigs.count(c.name)) continue;   // duplicate already reported
        lux_script::ClassSig sig;
        for (const auto& f : c.fields) sig.fields.push_back(f.name);
        for (const auto& m : c.methods) {
            size_t idx = out->functions.size();
            out->functions.push_back(std::make_shared<Chunk>());
            sig.methods[m.name] = make_sig(idx, m.params, m.return_type, diags);
        }
        for (const auto& ct : c.ctors) {
            size_t idx = out->functions.size();
            out->functions.push_back(std::make_shared<Chunk>());
            sig.ctors[ct.params.size()] = idx;
        }
        // With no declared constructor, the full mapping one: every field in order.
        if (sig.ctors.empty() && !c.fields.empty()) {
            size_t idx = out->functions.size();
            out->functions.push_back(std::make_shared<Chunk>());
            sig.ctors[c.fields.size()] = idx;
        }
        out->class_sigs[c.name] = std::move(sig);
    }
    for (const auto& e : program.enums)
        out->enum_sigs[e.name] = std::set<std::string>(e.members.begin(), e.members.end());

    // Emit every body.
    const std::set<std::string>* imports = &program.imports;
    for (const auto& f : program.functions) {
        auto it = out->fn_sigs.find(f.name);
        if (it == out->fn_sigs.end()) continue;
        Emitter emitter(diags, &out->fn_sigs, &out->class_sigs, imports, nullptr,
                        &out->enum_sigs);
        emitter.emit_function(f, *out->functions[it->second.index]);
    }
    for (const auto& c : program.classes) {
        auto cs = out->class_sigs.find(c.name);
        if (cs == out->class_sigs.end()) continue;
        for (const auto& m : c.methods) {
            auto ms = cs->second.methods.find(m.name);
            if (ms == cs->second.methods.end()) continue;
            Emitter emitter(diags, &out->fn_sigs, &out->class_sigs, imports, nullptr,
                            &out->enum_sigs);
            emitter.emit_method(c.name, m, *out->functions[ms->second.index]);
        }
        for (const auto& ct : c.ctors) {
            auto ix = cs->second.ctors.find(ct.params.size());
            if (ix == cs->second.ctors.end()) continue;
            Emitter emitter(diags, &out->fn_sigs, &out->class_sigs, imports, nullptr,
                            &out->enum_sigs);
            emitter.emit_ctor(c.name, cs->second.fields, ct, *out->functions[ix->second]);
        }
        if (c.ctors.empty() && !c.fields.empty()) {
            lux_script::CtorDecl implicit;
            implicit.loc = c.loc;
            for (const auto& f : c.fields) {
                lux_script::Param p;
                p.loc  = f.loc;
                p.name = f.name;
                p.type = f.type;
                implicit.params.push_back(std::move(p));
            }
            auto ix = cs->second.ctors.find(c.fields.size());
            if (ix != cs->second.ctors.end()) {
                Emitter emitter(diags, &out->fn_sigs, &out->class_sigs, imports, nullptr,
                                &out->enum_sigs);
                emitter.emit_ctor(c.name, cs->second.fields, implicit,
                                  *out->functions[ix->second]);
            }
        }
    }

    out->line_map = w.wrapped_to_doc;

    // Remap every diagnostic to document coordinates.
    for (const auto& d : diags.items()) {
        int wrapped_line = d.loc.line;
        int doc_line = 1;
        if (wrapped_line >= 1 &&
            wrapped_line <= static_cast<int>(w.wrapped_to_doc.size()))
            doc_line = w.wrapped_to_doc[static_cast<size_t>(wrapped_line - 1)];
        out->errors.push_back("line " + std::to_string(std::max(1, doc_line)) +
                              ": " + d.message);
    }
    return out;
}

// Single-block convenience (tests, headless tools).
std::unique_ptr<Compiled> compile_script(std::string_view source, int base_line) {
    return compile_blocks({{std::string(source), base_line}}, "script");
}

// ── Engine ──────────────────────────────────────────────────────────────────

void Engine::add_script(std::string_view source, int base_line) {
    blocks_.emplace_back(std::string(source), base_line);
}

bool Engine::finalize() {
    delete compiled_;
    compiled_ = nullptr;
    errors_.clear();
    if (blocks_.empty()) return true;
    auto compiled = compile_blocks(blocks_, page_.page_source_name());
    errors_ = compiled->errors;
    if (compiled->ok()) {
        compiled_ = compiled.release();
        return true;
    }
    return false;
}

dom::Node* Engine::node_of(long long handle) const {
    auto it = nodes_.find(handle);
    return it == nodes_.end() ? nullptr : it->second;
}

long long Engine::handle_of(dom::Node* node) {
    for (const auto& [h, n] : nodes_)
        if (n == node) return h;
    const long long h = next_handle_++;
    nodes_.emplace(h, node);
    return h;
}

Value Engine::make_element(dom::Node* node) {
    lux_script::Value::Dict d;
    d.append(std::string(lux_script::kElementKey),
             Value::integer(handle_of(node)));
    return Value::dict(std::move(d));
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

void Engine::log(int level, const std::string& msg) {
    page_.console(level < 0 ? 0 : (level > 2 ? 2 : level), msg);
}

std::string Engine::document_title() { return page_.page_title(); }

Value Engine::document_create(const std::string& tag) {
    std::string t = tag;
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto n = std::make_unique<dom::Node>(dom::NodeType::Element);
    n->name = t;
    detached_.push_back(std::move(n));
    return make_element(detached_.back().get());   // handle works BEFORE it is appended
}

// Takes a node out of wherever it lives (tree or the detached pool).
static std::unique_ptr<dom::Node> detach_node(dom::Node* n,
                                              std::vector<std::unique_ptr<dom::Node>>& detached) {
    for (auto it = detached.begin(); it != detached.end(); ++it)
        if (it->get() == n) {
            auto own = std::move(*it);
            detached.erase(it);
            return own;
        }
    if (!n->parent) return nullptr;
    auto& v = n->parent->children;
    auto it = std::find_if(v.begin(), v.end(),
                           [n](const auto& u) { return u.get() == n; });
    if (it == v.end()) return nullptr;
    auto own = std::move(*it);
    v.erase(it);
    own->parent = nullptr;
    return own;
}

Value Engine::document_get(const std::string& id) {
    dom::Node* root = page_.page_root();
    if (!root) return Value::null();
    // First element with attr id == <id>, document order.
    std::vector<dom::Node*> stack{root};
    while (!stack.empty()) {
        dom::Node* n = stack.back();
        stack.pop_back();
        if (n->is_element() && n->attr_or("id") == id) return make_element(n);
        for (size_t i = n->children.size(); i-- > 0;)
            stack.push_back(n->children[i].get());
    }
    return Value::null();
}

Value Engine::document_query_all(const std::string& tag) {
    dom::Node* root = page_.page_root();
    Value::List out;
    if (root) {
        std::vector<dom::Node*> stack{root};
        std::vector<dom::Node*> found;
        while (!stack.empty()) {
            dom::Node* n = stack.back();
            stack.pop_back();
            if (n->is_element(tag)) found.push_back(n);
            for (size_t i = n->children.size(); i-- > 0;)
                stack.push_back(n->children[i].get());
        }
        for (auto* n : found) out.push_back(make_element(n));
    }
    return Value::list(std::move(out));
}

Value Engine::document_root() {
    dom::Node* root = page_.page_root();
    return root ? make_element(root) : Value::null();
}

Value Engine::page_get(const std::string& key) {
    auto it = page_state_.find(key);
    return it == page_state_.end() ? Value::null() : it->second;
}

void Engine::page_set(const std::string& key, Value v) {
    page_state_[key] = std::move(v);
}

Value Engine::element_text(long long handle) {
    dom::Node* n = node_of(handle);
    return n ? Value::str(n->text_content()) : Value::null();
}

bool Engine::element_set_text(long long h, const std::string& t, std::string& err) {
    dom::Node* n = node_of(h);
    if (!n) {
        err = "the element no longer exists (the page changed under the script)";
        return false;
    }
    n->children.clear();
    auto tx = std::make_unique<dom::Node>(dom::NodeType::Text);
    tx->text = t;
    n->append(std::move(tx));
    page_.page_mutated();
    return true;
}

bool Engine::element_append(long long parent, long long child, std::string& err) {
    dom::Node* p = node_of(parent);
    dom::Node* ch = node_of(child);
    if (!p || !ch) { err = "an element no longer exists"; return false; }
    if (p == ch) { err = "cannot append an element to itself"; return false; }
    for (dom::Node* a = p->parent; a; a = a->parent)
        if (a == ch) { err = "cannot append an element inside its own subtree"; return false; }
    auto own = detach_node(ch, detached_);
    if (!own) { err = "the element cannot be moved"; return false; }
    p->append(std::move(own));
    page_.page_mutated();
    return true;
}

bool Engine::element_remove(long long h, std::string& err) {
    dom::Node* n = node_of(h);
    if (!n) { err = "the element no longer exists"; return false; }
    // Handles of the whole subtree die with it: using one afterwards is a
    // clean script error, never a dangling pointer.
    std::vector<dom::Node*> stack{n};
    while (!stack.empty()) {
        dom::Node* cur = stack.back();
        stack.pop_back();
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (it->second == cur) it = nodes_.erase(it);
            else ++it;
        }
        for (auto& c : cur->children) stack.push_back(c.get());
    }
    handlers_.erase(std::remove_if(handlers_.begin(), handlers_.end(),
                                   [&](const EventHandler& eh) {
                                       return nodes_.find(eh.handle) == nodes_.end();
                                   }),
                    handlers_.end());
    auto own = detach_node(n, detached_);
    if (!own) { err = "the element cannot be removed"; return false; }
    page_.page_mutated();
    return true;   // own destroyed here: the subtree goes with it
}

void Engine::page_after(int ms, const Value& fn) {
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    timers_.push_back({now + std::max(0, ms), fn});
}

void Engine::tick() {
    if (timers_.empty()) return;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<size_t> due;
    for (size_t i = 0; i < timers_.size(); ++i)
        if (timers_[i].at <= now) due.push_back(i);
    if (due.empty()) return;
    // Fire in schedule order; a handler may schedule more timers (they
    // append, so the collected indices stay valid).
    for (size_t i : due) {
        Value fn = timers_[i].fn;
        if (fn.is_func())
            run_fn(static_cast<size_t>(fn.as_func_index()), "a timer");
    }
    size_t kept = 0;
    for (size_t i = 0; i < timers_.size(); ++i)
        if (std::find(due.begin(), due.end(), i) == due.end())
            timers_[kept++] = std::move(timers_[i]);
    timers_.resize(kept);
}

Value Engine::page_clicked() {
    dom::Node* n = node_of(clicked_handle_);
    return n ? make_element(n) : Value::null();
}

bool Engine::element_click(long long h, const Value& fn, std::string& err) {
    if (!node_of(h)) {
        err = "the element no longer exists";
        return false;
    }
    for (const auto& existing : handlers_)
        if (existing.handle == h && existing.event == "click") {
            err = "this element already has a click handler";
            return false;
        }
    handlers_.push_back({h, "click", fn});
    return true;
}

Value Engine::element_attr(long long h, const std::string& name) {
    dom::Node* n = node_of(h);
    if (!n || !n->has_attr(name)) return Value::null();
    return Value::str(n->attr_or(name));
}

bool Engine::element_set_attr(long long h, const std::string& n,
                              const std::string& v, std::string& err) {
    dom::Node* node = node_of(h);
    if (!node) { err = "the element no longer exists"; return false; }
    for (auto& [k, val] : node->attrs)
        if (k == n) { val = v; page_.page_mutated(); return true; }
    node->attrs.emplace_back(n, v);
    page_.page_mutated();
    return true;
}

std::string Engine::element_tag(long long handle) {
    dom::Node* n = node_of(handle);
    return n ? n->name : "";
}

std::string Engine::element_id(long long handle) {
    dom::Node* n = node_of(handle);
    return n ? n->attr_or("id") : "";
}

long long Engine::element_child_count(long long handle) {
    dom::Node* n = node_of(handle);
    return n ? static_cast<long long>(n->children.size()) : 0;
}

bool Engine::run_fn(size_t table_index, const char* what) {
    if (!compiled_ || table_index >= compiled_->functions.size()) {
        page_.console(2, std::string(what) + ": nothing compiled to run");
        return false;
    }
    lux_script::NativeCtx ctx = make_ctx();
    lux_script::NativeCtx*& slot = lux_script::current_native_ctx();
    lux_script::NativeCtx* saved = slot;
    slot = &ctx;

    lux_script::VM vm;
    auto r = vm.start(*compiled_->functions[table_index], {}, ctx,
                      &compiled_->functions);

    slot = saved;

    switch (r.status) {
    case lux_script::VM::Status::Done:
        return true;
    case lux_script::VM::Status::Error:
        script_error(r.error, r.error_loc);
        return false;
    case lux_script::VM::Status::Suspended:
        page_.console(2, std::string(what) + ": used 'await', which no browser "
                            "builtin provides yet");
        return false;
    }
    return false;
}

void Engine::run_main() {
    if (!compiled_) return;
    // __luxium_main__ is the first function in the wrapped source, so chunk 0.
    run_fn(0, "the page's script");
}

bool Engine::fire(const std::string& event, long long handle) {
    bool ok = true;
    for (const auto& h : handlers_) {
        if (h.event != event || h.handle != handle) continue;
        if (!h.fn.is_func()) continue;
        clicked_handle_ = handle;   // page.clicked() inside the handler
        if (!run_fn(static_cast<size_t>(h.fn.as_func_index()), "a click handler"))
            ok = false;
    }
    clicked_handle_ = -1;
    return ok;
}

void Engine::click_node(const dom::Node* node) {
    for (const auto& [h, n] : nodes_)
        if (n == node) { fire("click", h); return; }
}

} // namespace luxium::script
