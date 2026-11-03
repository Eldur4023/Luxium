// document.* and the Element methods (browser_api.inc).
#include "script/engine.hpp"
#include "script/engine_internal.hpp"

#include <dom/node.hpp>
#include <html/parser.hpp>
#include <style/selector.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace luxium::script {

using dom::Node;
using dom::NodeType;

namespace {

const char* kNoEl = "the element no longer exists (the page changed under the script)";

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool is_void(const std::string& t) {
    static const char* v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                              "meta", "source", "track", "wbr"};
    for (const char* x : v) if (t == x) return true;
    return false;
}

void escape(const std::string& in, std::string& out, bool attr) {
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': if (!attr) out += "&lt;"; else out += c; break;
            case '>': if (!attr) out += "&gt;"; else out += c; break;
            case '"': if (attr) out += "&quot;"; else out += c; break;
            default: out += c;
        }
    }
}

void serialize(const Node& n, std::string& out) {
    switch (n.type) {
        case NodeType::Text: {
            const Node* p = n.parent;
            if (p && (p->name == "script" || p->name == "style")) out += n.text;
            else escape(n.text, out, false);
            break;
        }
        case NodeType::Comment: out += "<!--" + n.text + "-->"; break;
        case NodeType::Element: {
            out += "<" + n.name;
            for (const auto& [k, v] : n.attrs) { out += " " + k + "=\""; escape(v, out, true); out += "\""; }
            out += ">";
            if (is_void(n.name)) break;
            for (const auto& c : n.children) serialize(*c, out);
            out += "</" + n.name + ">";
            break;
        }
        default: for (const auto& c : n.children) serialize(*c, out);
    }
}

std::unique_ptr<Node> clone_tree(const Node& n, bool deep) {
    auto c = std::make_unique<Node>(n.type);
    c->name = n.name; c->text = n.text; c->attrs = n.attrs; c->line = n.line;
    if (deep) for (const auto& ch : n.children) c->append(clone_tree(*ch, true));
    return c;
}

// Takes a node out of wherever it lives (tree or the detached pool).
std::unique_ptr<Node> detach_node(Node* n, std::vector<std::unique_ptr<Node>>& detached) {
    for (auto it = detached.begin(); it != detached.end(); ++it)
        if (it->get() == n) {
            auto own = std::move(*it);
            detached.erase(it);
            return own;
        }
    if (!n->parent) return nullptr;
    auto& v = n->parent->children;
    auto it = std::find_if(v.begin(), v.end(), [n](const auto& u) { return u.get() == n; });
    if (it == v.end()) return nullptr;
    auto own = std::move(*it);
    v.erase(it);
    own->parent = nullptr;
    return own;
}

void drop_scripts(Node& n) {
    auto& v = n.children;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const std::unique_ptr<Node>& c) { return c->is_element("script"); }),
            v.end());
    for (auto& c : v) drop_scripts(*c);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string w;
    while (is >> w) out.push_back(w);
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& w : v) out += (out.empty() ? "" : " ") + w;
    return out;
}

void set_attr(Node& n, const std::string& k, const std::string& v) {
    for (auto& [ak, av] : n.attrs)
        if (ak == k) { av = v; return; }
    n.attrs.emplace_back(k, v);
}
void del_attr(Node& n, const std::string& k) {
    n.attrs.erase(std::remove_if(n.attrs.begin(), n.attrs.end(),
                                 [&](const auto& p) { return p.first == k; }), n.attrs.end());
}

// camelCase / snake name -> data-* attribute name
std::string data_attr(const std::string& name) {
    std::string out = "data-";
    for (char c : name) {
        if (std::isupper(static_cast<unsigned char>(c))) { out += '-'; out += char(std::tolower(c)); }
        else out += c;
    }
    return out;
}

void descendants(Node* n, std::vector<Node*>& out) {
    for (auto& c : n->children) {
        out.push_back(c.get());
        descendants(c.get(), out);
    }
}

Node* option_selected(Node* sel) {
    std::vector<Node*> all;
    descendants(sel, all);
    Node* first = nullptr;
    for (Node* o : all) {
        if (!o->is_element("option")) continue;
        if (!first) first = o;
        if (o->has_attr("selected")) return o;
    }
    return first;
}

std::string option_value(const Node& o) {
    return o.has_attr("value") ? o.attr_or("value") : trim(o.text_content());
}

} // namespace

// ── handles ─────────────────────────────────────────────────────────────────

Node* Engine::node_of(long long handle) const {
    auto it = nodes_.find(handle);
    return it == nodes_.end() ? nullptr : it->second;
}

long long Engine::handle_of(Node* node) {
    auto it = node_handle_.find(node);
    if (it != node_handle_.end()) return it->second;
    const long long h = next_handle_++;
    nodes_.emplace(h, node);
    node_handle_.emplace(node, h);
    return h;
}

Value Engine::element_value(Node* node) {
    return node ? lux_script::make_object("Element", handle_of(node)) : Value::null();
}

Node* Engine::arg_node(const Value& v, std::string& err, const char* what) {
    std::string type;
    long long h = 0;
    if (!lux_script::host_object_of(v, type, h) || type != "Element") {
        err = std::string(what) + " expects an Element";
        return nullptr;
    }
    Node* n = node_of(h);
    if (!n) err = kNoEl;
    return n;
}

void Engine::forget_subtree(Node* root) {
    std::vector<Node*> all{root};
    descendants(root, all);
    for (Node* n : all) {
        if (auto it = node_handle_.find(n); it != node_handle_.end()) {
            nodes_.erase(it->second);
            node_handle_.erase(it);
        }
        if (auto it = listeners_.find(n); it != listeners_.end()) {
            for (const auto& l : it->second)
                if (--kind_count_[l.kind] <= 0) kind_count_.erase(l.kind);
            listeners_.erase(it);
        }
        media_watch_.erase(n);
        dialog_ret_.erase(n);
    }
}

std::vector<Node*> Engine::select_all(Node* scope, const std::string& sel, bool first_only,
                                      std::string& err) {
    auto it = sel_cache_.find(sel);
    if (it == sel_cache_.end()) {
        auto list = style::parse_selector_list(sel);
        if (list.empty()) { err = "invalid selector '" + sel + "'"; return {}; }
        if (sel_cache_.size() > 256) sel_cache_.clear();
        it = sel_cache_.emplace(sel, std::move(list)).first;
    }
    std::vector<Node*> out;
    std::vector<Node*> stack;
    for (size_t i = scope->children.size(); i-- > 0;) stack.push_back(scope->children[i].get());
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->is_element()) {
            for (const auto& s : it->second)
                if (style::matches(s, *n)) { out.push_back(n); break; }
            if (first_only && !out.empty()) return out;
        }
        for (size_t i = n->children.size(); i-- > 0;) stack.push_back(n->children[i].get());
    }
    return out;
}

// ── form controls ───────────────────────────────────────────────────────────

std::string Engine::control_value(Node* n) {
    auto& h = page_.hooks();
    std::string v;
    if (h.get_value && h.get_value(n, v)) return v;
    if (n->is_element("textarea")) return n->text_content();
    if (n->is_element("select")) {
        Node* o = option_selected(n);
        return o ? option_value(*o) : std::string();
    }
    const std::string type = n->attr_or("type", "text");
    if (n->is_element("input") && (type == "checkbox" || type == "radio") && !n->has_attr("value"))
        return "on";
    return n->attr_or("value");
}

void Engine::set_control_value(Node* n, const std::string& v) {
    auto& h = page_.hooks();
    if (h.set_value && h.set_value(n, v)) { page_.page_mutated(); return; }
    if (n->is_element("textarea")) {
        for (auto& c : n->children) removed_.push_back(std::move(c));
        n->children.clear();
        auto t = std::make_unique<Node>(NodeType::Text);
        t->text = v;
        n->append(std::move(t));
    } else if (n->is_element("select")) {
        std::vector<Node*> all;
        descendants(n, all);
        bool hit = false;
        for (Node* o : all) {
            if (!o->is_element("option")) continue;
            if (!hit && option_value(*o) == v) { set_attr(*o, "selected", ""); hit = true; }
            else del_attr(*o, "selected");
        }
    } else {
        set_attr(*n, "value", v);
    }
    page_.page_mutated();
}

std::vector<std::string> Engine::input_files(Node* n) {
    if (page_.hooks().files) return page_.hooks().files(n);
    std::vector<std::string> paths;
    std::istringstream is(n->attr_or("data-files"));
    std::string p;
    while (std::getline(is, p)) if (!p.empty()) paths.push_back(p);
    return paths;
}

void Engine::set_children_html(Node* n, const std::string& html, std::string& err) {
    auto parsed = html::parse("<!doctype html><body>" + html);
    Node* body = parsed.root->find_first("body");
    std::vector<std::unique_ptr<Node>> fresh;
    if (body) fresh = std::move(body->children);
    int max_h = 0;
    fresh.erase(std::remove_if(fresh.begin(), fresh.end(),
                               [](const std::unique_ptr<Node>& c) { return c->is_element("script"); }),
                fresh.end());
    for (auto& c : fresh) { c->parent = nullptr; drop_scripts(*c); max_h = std::max(max_h, dom::subtree_height(*c)); }
    if (dom::depth_of(n) + max_h > dom::kMaxDepth) {
        err = "the tree would be deeper than " + std::to_string(dom::kMaxDepth) + " levels";
        return;
    }
    for (auto& c : n->children) {
        forget_subtree(c.get());
        c->parent = nullptr;
        removed_.push_back(std::move(c));
    }
    n->children.clear();
    for (auto& c : fresh) n->append(std::move(c));
    page_.page_mutated();
}

void Engine::close_dialog(Node* n, const std::string& value) {
    if (!n->has_attr("open")) return;
    del_attr(*n, "open");
    dialog_ret_[n] = value;
    page_.page_mutated();
    dispatch("close", n);
}

// ── registration ────────────────────────────────────────────────────────────

void Engine::register_dom() {
    auto page_root = [this]() { return page_.page_root(); };

    // document.*
    reg("document_get", [this, page_root](Args& a, std::string& e) {
        if (!a[0].is_str()) { e = "document.get() expects the id as a string"; return Value::null(); }
        Node* root = page_root();
        if (!root) return Value::null();
        std::vector<Node*> stack{root};
        while (!stack.empty()) {
            Node* n = stack.back();
            stack.pop_back();
            if (n->is_element() && n->attr_or("id") == a[0].as_str()) return element_value(n);
            for (size_t i = n->children.size(); i-- > 0;) stack.push_back(n->children[i].get());
        }
        return Value::null();
    });
    reg("document_query", [this, page_root](Args& a, std::string& e) {
        if (!a[0].is_str()) { e = "document.query() expects a selector"; return Value::null(); }
        Node* root = page_root();
        if (!root) return Value::null();
        std::string err;
        auto r = select_all(root, a[0].as_str(), true, err);
        if (!err.empty()) { e = err; return Value::null(); }
        return r.empty() ? Value::null() : element_value(r[0]);
    });
    reg("document_query_all", [this, page_root](Args& a, std::string& e) {
        if (!a[0].is_str()) { e = "document.query_all() expects a selector"; return Value::null(); }
        Value::List out;
        if (Node* root = page_root()) {
            std::string err;
            for (Node* n : select_all(root, a[0].as_str(), false, err)) out.push_back(element_value(n));
            if (!err.empty()) { e = err; return Value::null(); }
        }
        return Value::list(std::move(out));
    });
    reg("document_title", [this](Args&, std::string&) { return Value::str(page_.page_title()); });
    reg("document_root", [this, page_root](Args&, std::string&) { return element_value(page_root()); });
    reg("document_body", [this, page_root](Args&, std::string&) {
        Node* root = page_root();
        return element_value(root ? root->find_first("body") : nullptr);
    });
    reg("document_create", [this](Args& a, std::string& e) {
        if (!a[0].is_str()) { e = "document.create() expects the tag as a string"; return Value::null(); }
        std::string t = a[0].as_str();
        for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto n = std::make_unique<Node>(NodeType::Element);
        n->name = t;
        detached_.push_back(std::move(n));
        return element_value(detached_.back().get());
    });
    reg("document_listen", [this, page_root](Args& a, std::string& e) {
        if (!a[0].is_str() || !a[1].is_func()) { e = "document.listen(kind, fn [, capture]) expects a string and a function"; return Value::null(); }
        add_listener(page_root(), a[0].as_str(), a[1], a.size() > 2 && a[2].truthy());
        return Value::null();
    });
    reg("document_unlisten", [this, page_root](Args& a, std::string& e) {
        if (!a[0].is_str() || !a[1].is_func()) { e = "document.unlisten(kind, fn) expects a string and a function"; return Value::null(); }
        remove_listener(page_root(), a[0].as_str(), a[1]);
        return Value::null();
    });
    reg("document_fullscreen", [this](Args& a, std::string& e) {
        std::string err;
        Node* n = arg_node(a[0], err, "document.fullscreen()");
        if (!n) { e = err; return Value::null(); }
        if (page_.hooks().fullscreen) page_.hooks().fullscreen(n);
        return Value::null();
    });
    reg("document_exit_fullscreen", [this](Args&, std::string&) {
        if (page_.hooks().fullscreen) page_.hooks().fullscreen(nullptr);
        return Value::null();
    });
    reg("document_fullscreen_element", [this](Args&, std::string&) {
        auto& h = page_.hooks();
        return element_value(h.fullscreen_element ? h.fullscreen_element() : nullptr);
    });
    reg("document_active_element", [this](Args&, std::string&) {
        auto& h = page_.hooks();
        return element_value(h.focused ? h.focused() : nullptr);
    });
    reg("document_hidden", [this](Args&, std::string&) {
        auto& h = page_.hooks();
        return Value::boolean(h.page_visible ? !h.page_visible() : false);
    });

    // ── Element methods ──────────────────────────────────────────────────
    // `E` registers a method whose first step is resolving the live node.
#define EL(name, min_args_unused, ...)                                                  \
    reg_method("Element", name, [this](long long h, Args& a, std::string& e) -> Value {     \
        (void)a;                                                                            \
        Node* n = node_of(h);                                                               \
        if (!n) { e = kNoEl; return Value::null(); }                                        \
        __VA_ARGS__                                                                         \
    });

    EL("text", 0, { return Value::str(n->text_content()); })
    EL("set_text", 1, {
        for (auto& c : n->children) { forget_subtree(c.get()); c->parent = nullptr; removed_.push_back(std::move(c)); }
        n->children.clear();
        auto tx = std::make_unique<Node>(NodeType::Text);
        tx->text = a[0].to_string();
        n->append(std::move(tx));
        page_.page_mutated();
        return Value::null();
    })
    EL("html", 0, {
        std::string out;
        for (const auto& c : n->children) serialize(*c, out);
        return Value::str(out);
    })
    EL("set_html", 1, {
        if (!a[0].is_str()) { e = "set_html() expects a string"; return Value::null(); }
        std::string err;
        set_children_html(n, a[0].as_str(), err);
        if (!err.empty()) e = err;
        return Value::null();
    })
    EL("attr", 1, {
        if (!a[0].is_str()) { e = "attr() expects the attribute name"; return Value::null(); }
        return n->has_attr(a[0].as_str()) ? Value::str(n->attr_or(a[0].as_str())) : Value::null();
    })
    EL("set_attr", 2, {
        if (!a[0].is_str()) { e = "set_attr() expects the attribute name"; return Value::null(); }
        set_attr(*n, a[0].as_str(), a[1].to_string());
        page_.page_mutated();
        return Value::null();
    })
    EL("remove_attr", 1, {
        if (!a[0].is_str()) { e = "remove_attr() expects the attribute name"; return Value::null(); }
        del_attr(*n, a[0].as_str());
        page_.page_mutated();
        return Value::null();
    })
    EL("has_attr", 1, { return Value::boolean(a[0].is_str() && n->has_attr(a[0].as_str())); })
    EL("tag", 0, { return Value::str(n->name); })
    EL("id", 0, { return Value::str(n->attr_or("id")); })
    EL("child_count", 0, {
        long long c = 0;
        for (const auto& ch : n->children) c += ch->is_element();
        return Value::integer(c);
    })

    // tree
    EL("query", 1, {
        if (!a[0].is_str()) { e = "query() expects a selector"; return Value::null(); }
        std::string err;
        auto r = select_all(n, a[0].as_str(), true, err);
        if (!err.empty()) { e = err; return Value::null(); }
        return r.empty() ? Value::null() : element_value(r[0]);
    })
    EL("query_all", 1, {
        if (!a[0].is_str()) { e = "query_all() expects a selector"; return Value::null(); }
        std::string err;
        Value::List out;
        for (Node* x : select_all(n, a[0].as_str(), false, err)) out.push_back(element_value(x));
        if (!err.empty()) { e = err; return Value::null(); }
        return Value::list(std::move(out));
    })
    EL("closest", 1, {
        if (!a[0].is_str()) { e = "closest() expects a selector"; return Value::null(); }
        auto it = sel_cache_.find(a[0].as_str());
        if (it == sel_cache_.end()) {
            auto list = style::parse_selector_list(a[0].as_str());
            if (list.empty()) { e = "invalid selector '" + a[0].as_str() + "'"; return Value::null(); }
            it = sel_cache_.emplace(a[0].as_str(), std::move(list)).first;
        }
        for (Node* x = n; x; x = x->parent) {
            if (!x->is_element()) continue;
            for (const auto& s : it->second)
                if (style::matches(s, *x)) return element_value(x);
        }
        return Value::null();
    })
    EL("matches", 1, {
        if (!a[0].is_str()) { e = "matches() expects a selector"; return Value::null(); }
        for (const auto& s : style::parse_selector_list(a[0].as_str()))
            if (style::matches(s, *n)) return Value::boolean(true);
        return Value::boolean(false);
    })
    EL("parent", 0, { return element_value(n->parent && n->parent->is_element() ? n->parent : nullptr); })
    EL("children", 0, {
        Value::List out;
        for (auto& c : n->children) if (c->is_element()) out.push_back(element_value(c.get()));
        return Value::list(std::move(out));
    })
    EL("next", 0, {
        if (!n->parent) return Value::null();
        auto& v = n->parent->children;
        auto it = std::find_if(v.begin(), v.end(), [n](const auto& u) { return u.get() == n; });
        for (++it; it != v.end(); ++it) if ((*it)->is_element()) return element_value(it->get());
        return Value::null();
    })
    EL("prev", 0, {
        if (!n->parent) return Value::null();
        auto& v = n->parent->children;
        auto it = std::find_if(v.begin(), v.end(), [n](const auto& u) { return u.get() == n; });
        while (it != v.begin()) { --it; if ((*it)->is_element()) return element_value(it->get()); }
        return Value::null();
    })
    EL("contains", 1, {
        std::string err;
        Node* o = arg_node(a[0], err, "contains()");
        if (!o) { e = err; return Value::null(); }
        for (Node* x = o; x; x = x->parent) if (x == n) return Value::boolean(true);
        return Value::boolean(false);
    })
    EL("connected", 0, {
        Node* root = page_.page_root();
        for (Node* x = n; x; x = x->parent) if (x == root) return Value::boolean(true);
        return Value::boolean(false);
    })

    // mutation
    auto insert = [this](Node* parent, Node* child, Node* before, std::string& e) -> bool {
        if (parent == child) { e = "cannot append an element to itself"; return false; }
        for (Node* x = parent->parent; x; x = x->parent)
            if (x == child) { e = "cannot append an element inside its own subtree"; return false; }
        if (dom::depth_of(parent) + dom::subtree_height(*child) > dom::kMaxDepth) {
            e = "the tree would be deeper than " + std::to_string(dom::kMaxDepth) + " levels";
            return false;
        }
        auto own = detach_node(child, detached_);
        if (!own) { e = "the element cannot be moved"; return false; }
        if (before) {
            auto& v = parent->children;
            auto it = std::find_if(v.begin(), v.end(), [before](const auto& u) { return u.get() == before; });
            own->parent = parent;
            v.insert(it, std::move(own));
        } else {
            parent->append(std::move(own));
        }
        page_.page_mutated();
        return true;
    };
    reg_method("Element", "append", [this, insert](long long h, Args& a, std::string& e) -> Value {
        Node* n = node_of(h);
        if (!n) { e = kNoEl; return Value::null(); }
        std::string err;
        Node* c = arg_node(a[0], err, "append()");
        if (!c) { e = err; return Value::null(); }
        insert(n, c, nullptr, e);
        return Value::null();
    });
    reg_method("Element", "prepend", [this, insert](long long h, Args& a, std::string& e) -> Value {
        Node* n = node_of(h);
        if (!n) { e = kNoEl; return Value::null(); }
        std::string err;
        Node* c = arg_node(a[0], err, "prepend()");
        if (!c) { e = err; return Value::null(); }
        Node* first = n->children.empty() ? nullptr : n->children[0].get();
        if (first == c) return Value::null();
        insert(n, c, first, e);
        return Value::null();
    });
    reg_method("Element", "insert_before", [this, insert](long long h, Args& a, std::string& e) -> Value {
        Node* n = node_of(h);
        if (!n) { e = kNoEl; return Value::null(); }
        std::string err;
        Node* c = arg_node(a[0], err, "insert_before()");
        if (!c) { e = err; return Value::null(); }
        Node* ref = nullptr;
        if (!a[1].is_null()) {
            ref = arg_node(a[1], err, "insert_before()");
            if (!ref) { e = err; return Value::null(); }
            if (ref->parent != n) { e = "insert_before(): the reference is not a child"; return Value::null(); }
        }
        if (ref == c) return Value::null();
        insert(n, c, ref, e);
        return Value::null();
    });
    reg_method("Element", "replace_with", [this, insert](long long h, Args& a, std::string& e) -> Value {
        Node* n = node_of(h);
        if (!n) { e = kNoEl; return Value::null(); }
        std::string err;
        Node* c = arg_node(a[0], err, "replace_with()");
        if (!c) { e = err; return Value::null(); }
        if (!n->parent) { e = "replace_with(): the element has no parent"; return Value::null(); }
        Node* parent = n->parent;
        if (c == n) return Value::null();
        if (!insert(parent, c, n, e)) return Value::null();
        std::vector<Node*> sub{n};
        forget_subtree(n);
        auto own = detach_node(n, detached_);
        if (own) removed_.push_back(std::move(own));
        page_.page_mutated();
        return Value::null();
    });
    EL("remove", 0, {
        if (n == page_.page_root()) { e = "the document cannot be removed"; return Value::null(); }
        forget_subtree(n);
        auto own = detach_node(n, detached_);
        if (!own) { e = "the element cannot be removed"; return Value::null(); }
        page_.page_mutated();
        removed_.push_back(std::move(own));   // kept alive until the shell has relaid out
        return Value::null();
    })
    EL("clone", 0, {
        auto c = clone_tree(*n, a.empty() || a[0].truthy());
        detached_.push_back(std::move(c));
        return element_value(detached_.back().get());
    })

    // classes / style / data
    EL("add_class", 1, {
        auto cl = split_ws(n->attr_or("class"));
        for (const auto& w : split_ws(a[0].to_string()))
            if (std::find(cl.begin(), cl.end(), w) == cl.end()) cl.push_back(w);
        set_attr(*n, "class", join(cl));
        page_.page_mutated();
        return Value::null();
    })
    EL("remove_class", 1, {
        auto cl = split_ws(n->attr_or("class"));
        for (const auto& w : split_ws(a[0].to_string())) cl.erase(std::remove(cl.begin(), cl.end(), w), cl.end());
        set_attr(*n, "class", join(cl));
        page_.page_mutated();
        return Value::null();
    })
    EL("toggle_class", 1, {
        auto cl = split_ws(n->attr_or("class"));
        const std::string w = a[0].to_string();
        const bool has = std::find(cl.begin(), cl.end(), w) != cl.end();
        const bool want = a.size() > 1 ? a[1].truthy() : !has;
        if (want && !has) cl.push_back(w);
        if (!want && has) cl.erase(std::remove(cl.begin(), cl.end(), w), cl.end());
        if (want != has) { set_attr(*n, "class", join(cl)); page_.page_mutated(); }
        return Value::boolean(want);
    })
    EL("has_class", 1, {
        auto cl = split_ws(n->attr_or("class"));
        return Value::boolean(std::find(cl.begin(), cl.end(), a[0].to_string()) != cl.end());
    })
    EL("set_style", 2, {
        if (!a[0].is_str()) { e = "set_style(prop, value) expects strings"; return Value::null(); }
        const std::string prop = a[0].as_str(), val = a[1].to_string();
        // Rebuild the inline style: replace the declaration, or add it; "" removes it.
        std::vector<std::pair<std::string, std::string>> decls;
        std::istringstream is(n->attr_or("style"));
        std::string d;
        bool found = false;
        while (std::getline(is, d, ';')) {
            auto c = d.find(':');
            if (c == std::string::npos) continue;
            std::string k = trim(d.substr(0, c)), v = trim(d.substr(c + 1));
            if (k == prop) { found = true; if (!val.empty()) decls.emplace_back(k, val); }
            else decls.emplace_back(k, v);
        }
        if (!found && !val.empty()) decls.emplace_back(prop, val);
        std::string out;
        for (const auto& [k, v] : decls) out += k + ": " + v + "; ";
        set_attr(*n, "style", trim(out));
        page_.page_mutated();
        return Value::null();
    })
    EL("style", 1, {
        std::istringstream is(n->attr_or("style"));
        std::string d;
        while (std::getline(is, d, ';')) {
            auto c = d.find(':');
            if (c != std::string::npos && trim(d.substr(0, c)) == a[0].to_string()) return Value::str(trim(d.substr(c + 1)));
        }
        return Value::str("");
    })
    EL("data", 1, {
        const std::string k = data_attr(a[0].to_string());
        return n->has_attr(k) ? Value::str(n->attr_or(k)) : Value::null();
    })
    EL("set_data", 2, {
        set_attr(*n, data_attr(a[0].to_string()), a[1].to_string());
        return Value::null();
    })
    EL("hidden", 0, { return Value::boolean(n->has_attr("hidden")); })
    EL("set_hidden", 1, {
        if (a[0].truthy()) set_attr(*n, "hidden", ""); else del_attr(*n, "hidden");
        page_.page_mutated();
        return Value::null();
    })

    // forms
    EL("value", 0, { return Value::str(control_value(n)); })
    EL("set_value", 1, { set_control_value(n, a[0].to_string()); return Value::null(); })
    EL("selected_text", 0, {
        Node* o = n->is_element("select") ? option_selected(n) : nullptr;
        return Value::str(o ? trim(o->text_content()) : std::string());
    })
    EL("checked", 0, { return Value::boolean(n->has_attr("checked")); })
    EL("set_checked", 1, {
        if (a[0].truthy()) set_attr(*n, "checked", ""); else del_attr(*n, "checked");
        page_.page_mutated();
        return Value::null();
    })
    EL("disabled", 0, { return Value::boolean(n->has_attr("disabled")); })
    EL("set_disabled", 1, {
        if (a[0].truthy()) set_attr(*n, "disabled", ""); else del_attr(*n, "disabled");
        page_.page_mutated();
        return Value::null();
    })
    EL("files", 0, {
        Value::List out;
        for (const auto& p : input_files(n)) out.push_back(file_value(p));
        return Value::list(std::move(out));
    })
    EL("show_modal", 0, { set_attr(*n, "open", ""); page_.page_mutated(); return Value::null(); })
    EL("close", 0, { close_dialog(n, a.empty() ? std::string() : a[0].to_string()); return Value::null(); })
    EL("return_value", 0, {
        auto it = dialog_ret_.find(n);
        return Value::str(it == dialog_ret_.end() ? std::string() : it->second);
    })

    // events, focus, geometry
    EL("click", 1, {
        if (!a[0].is_func()) { e = "click() expects a function: el.click(fn)"; return Value::null(); }
        add_listener(n, "click", a[0], false);
        return Value::null();
    })
    EL("listen", 2, {
        if (!a[0].is_str() || !a[1].is_func()) { e = "listen(kind, fn [, capture]) expects a string and a function"; return Value::null(); }
        add_listener(n, a[0].as_str(), a[1], a.size() > 2 && a[2].truthy());
        return Value::null();
    })
    EL("unlisten", 2, {
        if (!a[0].is_str() || !a[1].is_func()) { e = "unlisten(kind, fn) expects a string and a function"; return Value::null(); }
        remove_listener(n, a[0].as_str(), a[1]);
        return Value::null();
    })
    EL("dispatch", 1, {
        if (!a[0].is_str()) { e = "dispatch(kind) expects a string"; return Value::null(); }
        dispatch(a[0].as_str(), n);
        return Value::null();
    })
    EL("focus", 0, { if (page_.hooks().focus) page_.hooks().focus(n); return Value::null(); })
    EL("blur", 0, {
        auto& hk = page_.hooks();
        if (hk.focus && hk.focused && hk.focused() == n) hk.focus(nullptr);
        return Value::null();
    })
    EL("rect", 0, {
        float r[4] = {0, 0, 0, 0};
        if (page_.hooks().rect) page_.hooks().rect(n, r);
        Value::Dict d;
        d.append("x", Value::real(r[0])); d.append("y", Value::real(r[1]));
        d.append("w", Value::real(r[2])); d.append("h", Value::real(r[3]));
        return Value::dict(std::move(d));
    })
    EL("scroll_into_view", 0, { if (page_.hooks().scroll_into_view) page_.hooks().scroll_into_view(n); return Value::null(); })
    EL("scroll_top", 0, { auto& hk = page_.hooks(); return Value::real(hk.scroll_get ? hk.scroll_get(n, true) : 0.0); })
    EL("scroll_left", 0, { auto& hk = page_.hooks(); return Value::real(hk.scroll_get ? hk.scroll_get(n, false) : 0.0); })
    EL("set_scroll_top", 1, { if (page_.hooks().scroll_set && a[0].is_num()) page_.hooks().scroll_set(n, true, float(a[0].as_float())); return Value::null(); })
    EL("set_scroll_left", 1, { if (page_.hooks().scroll_set && a[0].is_num()) page_.hooks().scroll_set(n, false, float(a[0].as_float())); return Value::null(); })

    // <video>/<audio>
    auto media_state = [this](Node* n) { MediaApi* m = page_.media(); return m ? m->state(n) : MediaState{}; };
    EL("play", 0, {
        auto& snap = media_watch_.try_emplace(n).first->second;
        MediaApi* m = page_.media();
        if (m && m->state(n).known) m->play(n); else snap.want_play = true;
        return Value::null();
    })
    EL("pause", 0, { if (MediaApi* m = page_.media()) m->pause(n); media_watch_[n].want_play = false; return Value::null(); })
    EL("seek", 1, {
        if (!a[0].is_num()) { e = "seek(t) expects seconds"; return Value::null(); }
        auto& snap = media_watch_.try_emplace(n).first->second;
        MediaApi* m = page_.media();
        if (m && m->state(n).known) m->seek(n, a[0].as_float()); else snap.want_seek = a[0].as_float();
        return Value::null();
    })
    reg_method("Element", "current_time", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        return Value::real(media_state(n).time);
    });
    reg_method("Element", "duration", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        return Value::real(media_state(n).duration);
    });
    reg_method("Element", "paused", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        return Value::boolean(media_state(n).paused);
    });
    reg_method("Element", "ended", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        return Value::boolean(media_state(n).ended);
    });
    reg_method("Element", "seeking", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        return Value::boolean(media_state(n).seeking);
    });
    reg_method("Element", "cue", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        return Value::str(media_state(n).cue);
    });
    EL("set_volume", 1, {
        if (!a[0].is_num()) { e = "set_volume(v) expects 0..1"; return Value::null(); }
        double v = std::clamp(a[0].as_float(), 0.0, 1.0);
        set_attr(*n, "data-volume", std::to_string(v));
        if (MediaApi* m = page_.media()) m->set_volume(n, v);
        dispatch("volumechange", n);
        return Value::null();
    })
    EL("volume", 0, { return Value::real(n->has_attr("data-volume") ? std::atof(n->attr_or("data-volume").c_str()) : 1.0); })
    EL("set_muted", 1, {
        if (a[0].truthy()) set_attr(*n, "muted", ""); else del_attr(*n, "muted");
        if (MediaApi* m = page_.media()) m->set_muted(n, a[0].truthy());
        page_.page_mutated();
        dispatch("volumechange", n);
        return Value::null();
    })
    EL("muted", 0, { return Value::boolean(n->has_attr("muted")); })
    reg_method("Element", "audio_tracks", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        const MediaState st = media_state(n);
        Value::List out;
        for (int i = 0; i < st.audio_count; ++i) {
            Value::Dict d;
            d.append("index", Value::integer(i));
            d.append("label", Value::str(i + 1 == st.audio_cur && !st.audio_label.empty()
                                             ? st.audio_label : "Audio " + std::to_string(i + 1)));
            d.append("current", Value::boolean(i + 1 == st.audio_cur));
            out.push_back(Value::dict(std::move(d)));
        }
        return Value::list(std::move(out));
    });
    reg_method("Element", "text_tracks", [this, media_state](long long h, Args&, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        const MediaState st = media_state(n);
        Value::List out;
        for (int i = 0; i < st.sub_count; ++i) {
            Value::Dict d;
            d.append("index", Value::integer(i));
            d.append("label", Value::str(i + 1 == st.sub_cur && !st.sub_label.empty()
                                             ? st.sub_label : "Subtitles " + std::to_string(i + 1)));
            d.append("current", Value::boolean(i + 1 == st.sub_cur));
            out.push_back(Value::dict(std::move(d)));
        }
        return Value::list(std::move(out));
    });
    // The engine only offers "next stream" (media::own cycle_*): select_* turns a
    // wanted index into the right number of steps.
    reg_method("Element", "select_audio", [this, media_state](long long h, Args& a, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        MediaApi* m = page_.media();
        const MediaState st = media_state(n);
        if (!m || !a[0].is_int() || st.audio_count < 1 || a[0].as_int() < 0 || a[0].as_int() >= st.audio_count) {
            e = "select_audio(i): no such audio track"; return Value::null();
        }
        const int steps = static_cast<int>((a[0].as_int() + 1 - st.audio_cur + st.audio_count) % st.audio_count);
        for (int i = 0; i < steps; ++i) m->cycle_audio(n);
        return Value::null();
    });
    reg_method("Element", "select_text", [this, media_state](long long h, Args& a, std::string& e) -> Value {
        Node* n = node_of(h); if (!n) { e = kNoEl; return Value::null(); }
        MediaApi* m = page_.media();
        const MediaState st = media_state(n);
        if (!m || !a[0].is_int() || a[0].as_int() < -1 || a[0].as_int() >= st.sub_count) {
            e = "select_text(i): no such text track"; return Value::null();
        }
        const int total = st.sub_count + 1;   // off + tracks
        const int steps = static_cast<int>((a[0].as_int() + 1 - st.sub_cur + total) % total);
        for (int i = 0; i < steps; ++i) m->cycle_subtitle(n);
        return Value::null();
    });
    EL("set_src", 1, { set_attr(*n, "src", a[0].to_string()); page_.page_mutated(); return Value::null(); })
    EL("src", 0, { return Value::str(n->attr_or("src")); })
#undef EL
}

} // namespace luxium::script
