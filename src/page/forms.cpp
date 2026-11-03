// Form controls and focus: the model side of <input>/<textarea>/<select>.  The shell turns SDL input into
// calls on Page; everything the script can observe leaves through Page::dispatch (FormEvent).
//
// State lives in the DOM itself: `value`/`checked`/`selected` attributes (a textarea's text child), so the
// stylesheet's :checked / :placeholder-shown and the layout see the same truth as scripts.

#include <page/page.hpp>

#include <algorithm>

namespace luxium::page {

namespace {

using dom::Node;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string input_type(const Node* n) { return lower(n->attr_or("type")); }

void set_attr(Node* n, const std::string& k, const std::string& v) {
    for (auto& a : n->attrs)
        if (a.first == k) { a.second = v; return; }
    n->attrs.emplace_back(k, v);
}
void del_attr(Node* n, const std::string& k) {
    n->attrs.erase(std::remove_if(n->attrs.begin(), n->attrs.end(), [&](const auto& a) { return a.first == k; }), n->attrs.end());
}

int u8_next(const std::string& s, int i) {
    if (i >= int(s.size())) return int(s.size());
    ++i;
    while (i < int(s.size()) && (s[i] & 0xC0) == 0x80) ++i;
    return i;
}
int u8_prev(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && (s[i] & 0xC0) == 0x80) --i;
    return i;
}
bool space(char c) { return c == ' ' || c == '\t' || c == '\n'; }
int word_left(const std::string& s, int i) {
    while (i > 0 && space(s[i - 1])) i = u8_prev(s, i);
    while (i > 0 && !space(s[i - 1])) i = u8_prev(s, i);
    return i;
}
int word_right(const std::string& s, int i) {
    const int n = int(s.size());
    while (i < n && space(s[i])) i = u8_next(s, i);
    while (i < n && !space(s[i])) i = u8_next(s, i);
    return i;
}

void options_of(const Node& n, std::vector<Node*>& out) {
    for (const auto& c : n.children) {
        if (!c->is_element()) continue;
        if (c->name == "option") out.push_back(c.get());
        else if (c->name == "optgroup") options_of(*c, out);
    }
}

bool hidden_chain(const Node* n) {
    for (; n; n = n->parent)
        if (n->is_element() && (n->has_attr("hidden") || (n->name == "details" && false))) return true;
    return false;
}

} // namespace

bool Page::is_text_control(const Node* n) {
    if (!n || !n->is_element()) return false;
    if (n->name == "textarea") return true;
    if (n->name != "input") return false;
    const std::string t = input_type(n);
    return t.empty() || t == "text" || t == "search" || t == "password" || t == "number" || t == "email" || t == "url" || t == "tel";
}

std::string Page::form_value(const Node* n) const {
    if (!n || !n->is_element()) return {};
    if (n->name == "textarea") {
        std::string v = n->text_content();
        if (v.starts_with('\n') && n->children.size() == 1) v.erase(0, 1);
        return v;
    }
    if (n->name == "select") {
        std::vector<Node*> opts;
        options_of(*n, opts);
        const int i = select_index(n);
        if (i < 0 || i >= int(opts.size())) return {};
        return opts[i]->has_attr("value") ? opts[i]->attr_or("value") : opts[i]->text_content();
    }
    if (n->name == "input" && input_type(n) == "file") {
        const auto f = form_files(n);
        return f.empty() ? std::string() : "C:\\fakepath\\" + f[0].substr(f[0].find_last_of('/') + 1);
    }
    return n->attr_or("value");
}

void Page::set_form_value(Node* n, const std::string& v) {
    if (!n || !n->is_element()) return;
    if (n->name == "textarea") {
        n->children.clear();
        auto t = std::make_unique<Node>(dom::NodeType::Text);
        t->name = "#text"; t->text = v;
        if (!v.empty()) n->append(std::move(t));
    } else if (n->name == "select") {
        std::vector<Node*> opts;
        options_of(*n, opts);
        for (size_t i = 0; i < opts.size(); ++i) {
            const std::string ov = opts[i]->has_attr("value") ? opts[i]->attr_or("value") : opts[i]->text_content();
            if (ov == v) { for (auto* o : opts) del_attr(o, "selected"); set_attr(opts[i], "selected", ""); break; }
        }
    } else if (n->name == "input") {
        if (input_type(n) == "file") { if (v.empty()) set_form_files(n, {}); }
        else set_attr(n, "value", v);
    }
    page_mutated();
}

bool Page::form_checked(const Node* n) const { return n && n->has_attr("checked"); }

void Page::set_form_checked(Node* n, bool on) {
    if (!n) return;
    if (on) set_attr(n, "checked", ""); else del_attr(n, "checked");
    page_mutated();
}

int Page::select_index(const Node* select) const {
    std::vector<Node*> opts;
    options_of(*select, opts);
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i]->has_attr("selected")) return int(i);
    return opts.empty() ? -1 : 0;
}

void Page::select_option(Node* select, int index) {
    std::vector<Node*> opts;
    options_of(*select, opts);
    if (index < 0 || index >= int(opts.size()) || select->has_attr("disabled") || opts[index]->has_attr("disabled")) return;
    const bool changed = index != select_index(select);
    for (auto* o : opts) del_attr(o, "selected");
    set_attr(opts[index], "selected", "");
    page_mutated();
    if (!changed) return;
    { EventData d; d.value = form_value(select); dispatch("input", select, d); }
    { EventData d; d.value = form_value(select); dispatch("change", select, d); }
}

void Page::set_form_files(Node* n, std::vector<std::string> paths) {
    if (paths.empty()) files_.erase(n); else files_[n] = std::move(paths);
    page_mutated();
}

// ── focus ───────────────────────────────────────────────────────────────────

bool Page::focusable(const Node* n) const {
    if (!n || !n->is_element() || hidden_chain(n)) return false;
    if (n->has_attr("tabindex")) return true;
    if (n->name == "a") return n->has_attr("href");
    if (n->name == "button" || n->name == "select" || n->name == "textarea") return !n->has_attr("disabled");
    if (n->name == "input") return input_type(n) != "hidden" && !n->has_attr("disabled");
    return false;
}

void Page::focus(Node* n, bool visible) {
    if (n == focus_) return;
    Node* old = focus_;
    focus_ = n;
    if (old) {
        old->state &= static_cast<unsigned char>(~(Node::Focus | Node::FocusVisible | Node::CaretOn));
        if (dom::g_ime.node == old) dom::g_ime = {};
        dispatch("blur", old);
    }
    if (n) {
        n->state |= Node::Focus;
        if (visible || is_text_control(n)) n->state |= Node::FocusVisible;
        if (is_text_control(n)) {
            n->caret = n->anchor = int(form_value(n).size());
            n->scroll_x = 0;
            n->state |= Node::CaretOn;
        }
        dispatch("focus", n);
    }
    page_mutated();
}

void Page::focus_next(bool backwards) {
    if (!root_) return;
    std::vector<Node*> order;
    std::vector<Node*> st{root_.get()};
    while (!st.empty()) {
        Node* n = st.back(); st.pop_back();
        if (n->is_element() && focusable(n)) {
            const std::string ti = n->attr_or("tabindex");
            if (ti.empty() || std::atoi(ti.c_str()) >= 0) order.push_back(n);
        }
        for (size_t i = n->children.size(); i-- > 0;) st.push_back(n->children[i].get());
    }
    if (order.empty()) return;
    size_t at = order.size();
    for (size_t i = 0; i < order.size(); ++i) if (order[i] == focus_) at = i;
    size_t next = at == order.size() ? (backwards ? order.size() - 1 : 0)
                                     : (backwards ? (at + order.size() - 1) % order.size() : (at + 1) % order.size());
    focus(order[next], true);
}

// ── editing ─────────────────────────────────────────────────────────────────

static bool editable(const Page& p, const Node* n) {
    return p.is_text_control(n) && !n->has_attr("readonly") && !n->has_attr("disabled");
}

void Page::edit_set_caret(int byte, bool extend) {
    if (!is_text_control(focus_)) return;
    const std::string v = form_value(focus_);
    focus_->caret = std::clamp(byte, 0, int(v.size()));
    if (!extend) focus_->anchor = focus_->caret;
    focus_->state |= Node::CaretOn;
    page_mutated();
}

void Page::edit_move(int dir, bool word, bool extend) {
    if (!is_text_control(focus_)) return;
    const std::string v = form_value(focus_);
    int t;
    if (!extend && focus_->anchor != focus_->caret && !word) t = dir < 0 ? std::min(focus_->anchor, focus_->caret) : std::max(focus_->anchor, focus_->caret);
    else if (word) t = dir < 0 ? word_left(v, focus_->caret) : word_right(v, focus_->caret);
    else t = dir < 0 ? u8_prev(v, focus_->caret) : u8_next(v, focus_->caret);
    edit_set_caret(t, extend);
}

void Page::edit_home_end(bool end, bool extend) {
    if (!is_text_control(focus_)) return;
    edit_set_caret(end ? int(form_value(focus_).size()) : 0, extend);
}

void Page::edit_select_all() {
    if (!is_text_control(focus_)) return;
    focus_->anchor = 0;
    focus_->caret = int(form_value(focus_).size());
    page_mutated();
}

std::string Page::edit_selection() const {
    if (!is_text_control(focus_)) return {};
    const std::string v = form_value(focus_);
    const int a = std::min(focus_->anchor, focus_->caret), b = std::max(focus_->anchor, focus_->caret);
    if (a == b || b > int(v.size())) return {};
    return input_type(focus_) == "password" ? std::string() : v.substr(a, b - a);
}

bool Page::edit_replace_selection(const std::string& utf8) {
    if (!editable(*this, focus_)) return false;
    std::string v = form_value(focus_);
    int a = std::min(focus_->anchor, focus_->caret), b = std::max(focus_->anchor, focus_->caret);
    a = std::clamp(a, 0, int(v.size())); b = std::clamp(b, 0, int(v.size()));
    std::string ins = utf8;
    const bool multi = focus_->name == "textarea";
    const std::string ty = input_type(focus_);
    std::string clean;
    for (char c : ins) {
        if ((c == '\n' || c == '\r') && !multi) continue;
        if (ty == "number" && !(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) continue;
        clean += c;
    }
    ins = clean;
    const int maxlen = std::atoi(focus_->attr_or("maxlength").c_str());
    if (maxlen > 0) {   // maxlength counts characters
        int chars = 0;
        for (size_t i = 0; i < v.size(); ++i) if ((v[i] & 0xC0) != 0x80) ++chars;
        for (int i = a; i < b; ++i) if ((v[i] & 0xC0) != 0x80) --chars;
        std::string cut;
        for (size_t i = 0; i < ins.size(); ++i) {
            if ((ins[i] & 0xC0) != 0x80) { if (chars >= maxlen) break; ++chars; }
            cut += ins[i];
            while (i + 1 < ins.size() && (ins[i + 1] & 0xC0) == 0x80) cut += ins[++i];
        }
        ins = cut;
    }
    if (a == b && ins.empty()) return false;
    v.replace(a, b - a, ins);
    set_form_value(focus_, v);
    focus_->caret = focus_->anchor = a + int(ins.size());
    focus_->state |= Node::CaretOn;
    { EventData d; d.key = ins; d.value = v; dispatch("input", focus_, d); }
    return true;
}

void Page::edit_insert(const std::string& utf8) { edit_replace_selection(utf8); }

void Page::edit_backspace(bool word) {
    if (!editable(*this, focus_)) return;
    if (focus_->anchor == focus_->caret) {
        const std::string v = form_value(focus_);
        const int p = word ? word_left(v, focus_->caret) : u8_prev(v, focus_->caret);
        if (p == focus_->caret) return;
        focus_->anchor = p;
    }
    edit_replace_selection("");
}

void Page::edit_delete(bool word) {
    if (!editable(*this, focus_)) return;
    if (focus_->anchor == focus_->caret) {
        const std::string v = form_value(focus_);
        const int p = word ? word_right(v, focus_->caret) : u8_next(v, focus_->caret);
        if (p == focus_->caret) return;
        focus_->anchor = p;
    }
    edit_replace_selection("");
}

void Page::edit_enter() {
    if (!focus_) return;
    if (focus_->name == "textarea") { edit_insert("\n"); return; }
    for (Node* f = focus_->parent; f; f = f->parent)
        if (f->is_element() && f->name == "form") {
            dispatch("submit", f);
            return;
        }
}

void Page::edit_select_word() {
    if (!is_text_control(focus_)) return;
    const std::string v = form_value(focus_);
    const int a = word_left(v, word_right(v, focus_->caret) == focus_->caret ? focus_->caret : word_right(v, focus_->caret));
    focus_->anchor = a;
    focus_->caret = word_right(v, a);
    page_mutated();
}

void Page::drop_stale_focus() {
    if (!focus_) return;
    for (const Node* n = focus_; n; n = n->parent)
        if (n == root_.get()) return;
    if (dom::g_ime.node == focus_) dom::g_ime = {};
    focus_ = nullptr;   // the node is gone: no Blur to deliver
}

bool Page::activate(Node* n) {
    // label -> its control; otherwise the nearest control under the click
    for (Node* c = n; c; c = c->parent) {
        if (!c->is_element()) continue;
        if (c->name == "label") {
            Node* target = nullptr;
            const std::string id = c->attr_or("for");
            std::vector<Node*> st{root_.get()};
            while (!st.empty() && !target) {
                Node* x = st.back(); st.pop_back();
                if (x == n) {}   // keep scanning
                if (x->is_element() && ((!id.empty() && x->attr_or("id") == id) ||
                    (id.empty() && x != c && x->is_element() && (x->name == "input" || x->name == "select" || x->name == "textarea") && [&] {
                        for (Node* p = x->parent; p; p = p->parent) if (p == c) return true;
                        return false; }())))
                    target = x;
                for (size_t i = x->children.size(); i-- > 0;) st.push_back(x->children[i].get());
            }
            // a click that already hit the control itself is handled below, not twice
            for (Node* p = n; p; p = p->parent) if (p == target) return activate(target);
            return target ? activate(target) : false;
        }
        if (c->name == "input" || c->name == "textarea" || c->name == "select") {
            if (c->has_attr("disabled")) return true;
            const std::string t = input_type(c);
            if (c->name == "input" && (t == "checkbox" || t == "radio")) {
                focus(c, false);
                if (t == "radio") {
                    if (form_checked(c)) return true;
                    const std::string name = c->attr_or("name");
                    if (!name.empty() && root_) {
                        Node* form = nullptr;
                        for (Node* f = c->parent; f && !form; f = f->parent) if (f->is_element() && f->name == "form") form = f;
                        std::vector<Node*> st{form ? form : root_.get()};
                        while (!st.empty()) {
                            Node* x = st.back(); st.pop_back();
                            if (x != c && x->is_element() && x->name == "input" && input_type(x) == "radio" && x->attr_or("name") == name)
                                del_attr(x, "checked");
                            for (auto& ch : x->children) st.push_back(ch.get());
                        }
                    }
                    set_form_checked(c, true);
                } else set_form_checked(c, !form_checked(c));
                { EventData d; d.value = form_checked(c) ? "on" : ""; dispatch("input", c, d); }
                { EventData d; d.value = form_checked(c) ? "on" : ""; dispatch("change", c, d); }
                return true;
            }
            focus(c, false);
            return true;
        }
        if (focusable(c) && c->name != "a" && c->name != "button") { focus(c, false); return false; }
        if (c->name == "button") { focus(c, false); return false; }
    }
    if (focus_) blur();
    return false;
}

} // namespace luxium::page
