#include "page/page.hpp"

#include <html/parser.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace luxium::page {

bool Page::load_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_string(ss.str(), path);
}

bool Page::load_string(const std::string& html, const std::string& source_name) {
    name_   = source_name;
    title_.clear();
    console_.clear();
    dirty_  = true;

    html::ParseResult result = html::parse(html);
    root_ = std::move(result.root);
    for (const auto& e : result.errors)
        console(1, "html: " + e);

    if (dom::Node* t = root_->find_first("title")) title_ = t->text_content();
    run_scripts();
    return true;
}

void Page::run_scripts() {
    // Every <script> block compiles into ONE program, in document order.
    // type="text/javascript" is refused with a message, not silently ignored:
    // a browser that drops scripts teaches the wrong lesson (docs/LUXSCRIPT.md §2).
    std::vector<std::pair<std::string, int>> blocks;
    for (dom::Node* s : root_->find_all("script")) {
        const std::string type = s->attr_or("type");
        if (type.find("javascript") != std::string::npos) {
            console(1, "script type=" + type + ": Luxium does not run JavaScript");
            continue;
        }
        if (s->has_attr("src")) {
            console(1, "script src=\"" + s->attr_or("src") +
                           "\": external scripts are not loaded yet");
            continue;
        }
        blocks.emplace_back(s->text_content(), s->line);
    }
    if (blocks.empty()) return;

    for (const auto& [body, line] : blocks)
        engine_.add_script(body, line);
    if (!engine_.finalize()) {
        for (const auto& e : engine_.errors())
            console(2, "luxscript: " + e);
        return;
    }
    engine_.run_main();
}

void Page::console(int level, const std::string& msg) {
    console_.push_back({level, msg});
    if (console_.size() > 500) console_.erase(console_.begin());
}

void Page::click(const dom::Node* node) {
    if (!node) return;
    engine_.click_node(node);
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
