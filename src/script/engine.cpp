#include "script/engine.hpp"
#include <script/bytecode_io.hpp>

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

// ── Engine ──────────────────────────────────────────────────────────────────

void Engine::set_program(std::unique_ptr<Compiled> program) {
    delete compiled_;
    compiled_ = program.release();
}

void Engine::reset() {
    delete compiled_;
    compiled_ = nullptr;
    handlers_.clear();
    timers_.clear();
    clicked_handle_ = -1;
    nodes_.clear();
    detached_.clear();
    removed_.clear();
    page_state_.clear();
    next_handle_ = 1;
}

bool Engine::load_precompiled(const std::string& bytes, std::string* err) {
    delete compiled_;
    compiled_ = nullptr;
    auto c = load_bytecode(bytes, err);
    if (!c) return false;
    compiled_ = c.release();
    return true;
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
    if (dom::depth_of(p) + dom::subtree_height(*ch) > dom::kMaxDepth) {
        err = "the tree would be deeper than " + std::to_string(dom::kMaxDepth) + " levels";
        return false;
    }
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
    // Kept alive until the shell has relaid out: its last layout (hit areas,
    // the hovered node) still points into this subtree.
    removed_.push_back(std::move(own));
    return true;
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
    // Snapshot first: a handler may remove() an element (erasing from
    // handlers_) or add a click() (push_back) while we would be iterating.
    std::vector<size_t> fns;
    for (const auto& h : handlers_)
        if (h.event == event && h.handle == handle && h.fn.is_func())
            fns.push_back(static_cast<size_t>(h.fn.as_func_index()));
    bool ok = true;
    for (size_t fn : fns) {
        clicked_handle_ = handle;   // page.clicked() inside the handler
        if (!run_fn(fn, "a click handler")) ok = false;
    }
    clicked_handle_ = -1;
    return ok;
}

void Engine::click_node(const dom::Node* node) {
    for (const auto& [h, n] : nodes_)
        if (n == node) { fire("click", h); return; }
}

bool Engine::has_handler_for(const dom::Node* node) const {
    for (const auto& [h, n] : nodes_)
        if (n == node)
            for (const auto& eh : handlers_)
                if (eh.handle == h) return true;
    return false;
}

} // namespace luxium::script
