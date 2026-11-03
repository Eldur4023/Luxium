#include "page/page.hpp"

#include <page/bundle.hpp>
#include <page/doc_codec.hpp>
#include <page/luxp.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace luxium::page {

bool Page::load_file(const std::string& path) {
    if (path.ends_with(".luxp")) {
        std::string err;
        auto entry = bundle::mount(path, &err);
        if (!entry) {
            load_string("<p>" + path + ": " + err + "</p>", path);
            console(2, ".luxp rechazado: " + err);
            return true;
        }
        return load_file(*entry);
    }
    if (const auto* sections = bundle::find_page(path)) return load_compiled(*sections, path);
    auto html = bundle::read_file(path);
    if (!html) return false;
    return load_string(*html, path);
}

bool Page::load_string(const std::string& html, const std::string& source_name) {
    reset_for_load(source_name);
    source_ = html;

    // The one translator (page/luxp.hpp) -- the same Lux uses to produce a
    // .luxp, so an HTML page and its .luxp cannot mean different things.
    const std::string dir = base_dir();
    Translation t = translate_html(html, [&](const std::string& url) { return bundle::read_file(dir + url); });
    for (const auto& w : t.warnings) console(1, w);
    root_ = std::move(t.document);
    if (dom::Node* title = root_->find_first("title")) title_ = title->text_content();
    set_rules(std::move(t.rules));

    saw_js_ = t.uses_javascript;   // -> the shell hands the page to Chromium
    if (!t.errors.empty()) {
        // El juicio de la casa: un <script> pelado que no compila como Lux
        // casi seguro es JavaScript (la web habla JS).
        saw_js_ = true;
        console(1, "el script no compila como LuxScript: probablemente JavaScript, pasando a Chromium");
        for (const auto& e : t.errors) console(2, "luxscript: " + e);
        return true;
    }
    if (t.program) {
        engine_.set_program(std::move(t.program));
        engine_.run_main();
    }
    return true;
}

bool Page::load_luxp(const std::string& bytes, const std::string& name) {
    Container c;
    std::string err;
    if (!decode_container(bytes, c, &err)) {
        load_string("<p>" + name + ": .luxp rechazado: " + err + "</p>", name);
        console(2, ".luxp rechazado: " + err);
        return true;
    }
    return load_compiled(c.pages[0].second, name);
}

void Page::reset_for_load(const std::string& name) {
    name_ = name;
    source_.clear();
    title_.clear();
    console_.clear();
    sheet_.clear();
    uses_hover_ = false;
    saw_js_ = false;
    dirty_  = true;
    engine_.reset();   // before the old DOM goes: its handles die with it
}

// A .luxp page: the engine's structures, decoded and verified -- no HTML
// tokenizer, no CSS parser, no compiler, never a hand-off to Chromium.  Any
// rejected section shows an error page instead of a half-loaded document.
bool Page::load_compiled(const PageSections& s, const std::string& name) {
    std::string err;
    auto doc = decode_document(s.document, &err);
    std::vector<style::Rule> rules;
    if (doc && !decode_sheet(s.sheet, rules, &err)) doc.reset();
    if (!doc) {
        load_string("<p>" + name + ": .luxp rechazado: " + err + "</p>", name);
        console(2, ".luxp rechazado: " + err);
        return true;
    }
    reset_for_load(name);
    root_ = std::move(doc);
    if (dom::Node* t = root_->find_first("title")) title_ = t->text_content();
    set_rules(std::move(rules));
    if (s.program.empty()) return true;
    if (!engine_.load_precompiled(s.program, &err)) {
        console(2, "bytecode rechazado: " + err);
        return true;
    }
    engine_.run_main();
    return true;
}

static bool sel_uses_hover(const style::Selector& sel) {
    for (const auto& c : sel.parts)
        for (const auto& ss : c.simples) {
            if (ss.kind == style::SimpleSel::Kind::Hover) return true;
            for (const auto& a : ss.args) if (sel_uses_hover(a)) return true;
        }
    return false;
}

void Page::set_rules(std::vector<style::Rule> rules) {
    sheet_ = std::move(rules);
    // <meta name="color-scheme" content="dark">: tag <html> so the UA sheet themes controls and the shell the scrollbars
    dark_scheme_ = false;
    if (root_)
        for (dom::Node* m : root_->find_all("meta"))
            if (m->attr_or("name") == "color-scheme" && m->attr_or("content").find("dark") != std::string::npos) dark_scheme_ = true;
    if (root_ && dark_scheme_)
        if (dom::Node* h = root_->find_first("html")) h->attrs.emplace_back("data-luxium-scheme", "dark");
    focus_ = nullptr;
    if (root_) {   // `autofocus`: the first such control takes focus on load
        std::vector<dom::Node*> st{root_.get()};
        while (!st.empty() && !focus_) {
            dom::Node* n = st.back(); st.pop_back();
            if (n->is_element() && n->has_attr("autofocus") && focusable(n)) focus(n);
            for (size_t i = n->children.size(); i-- > 0;) st.push_back(n->children[i].get());
        }
    }
    uses_hover_ = false;
    for (const auto& r : sheet_)
        for (const auto& sel : r.selectors) uses_hover_ = uses_hover_ || sel_uses_hover(sel);
}

void Page::console(int level, const std::string& msg) {
    console_.push_back({level, msg});
    if (console_.size() > 500) console_.erase(console_.begin());
}

void Page::click(const dom::Node* node) {
    if (!node) return;
    if (on_media_click && node->is_element() && (node->name == "video" || node->name == "audio"))
        on_media_click(node);

    // The nearest ancestor with href owns the click: navigation first (the
    // engine still runs its handlers, but the page is about to change).
    for (const dom::Node* n = node; n; n = n->parent) {
        if (!n->is_element()) continue;
        std::string href = n->attr_or("href");
        if (!href.empty()) {
            if (href.rfind("javascript:", 0) == 0) {
                console(1, "javascript: Luxium does not run JavaScript");
                return;
            }
            if (on_navigate) on_navigate(href);
            return;
        }
    }
    engine_.click_node(node);
}

bool Page::has_click_handler(const dom::Node* node) const {
    return engine_.has_handler_for(node);
}

void Page::click_by_id(const std::string& id) {
    if (!root_) return;
    std::vector<dom::Node*> stack{root_.get()};
    while (!stack.empty()) {
        dom::Node* n = stack.back();
        stack.pop_back();
        if (n->is_element() && n->attr_or("id") == id) { click(n); return; }
        for (size_t i = n->children.size(); i-- > 0;)
            stack.push_back(n->children[i].get());
    }
}

} // namespace luxium::page
