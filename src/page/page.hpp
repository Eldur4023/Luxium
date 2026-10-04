#pragma once
// The page model: one loaded document = DOM + compiled scripts + console.
//
// The shell owns a Page per tab (one today); the Engine talks to the page
// through PageHost, the page talks to the DOM through dom::.  Who owns what:
//   • the DOM tree (dom::Node)        -- the Page
//   • the compiled program + handles  -- the Engine
//   • the console buffer              -- the Page (the shell renders it)

#include <dom/node.hpp>
#include <script/engine.hpp>

#include <memory>
#include <string>
#include <vector>

namespace luxium::page {

class Page : public PageHost {
public:
    struct ConsoleLine {
        int         level = 0;   // 0 info, 1 warn, 2 error
        std::string text;
    };

    // Loads and runs.  Returns false if the HTML could not be read; a broken
    // script or bad HTML still yields a usable page (console shows why).
    bool load_file(const std::string& path);
    bool load_string(const std::string& html, const std::string& source_name);

    // PageHost ────────────────────────────────────────────────────────────
    void        console(int level, const std::string& msg) override;
    std::string page_title() const override { return title_; }
    dom::Node*  page_root() override { return root_.get(); }
    void        page_mutated() override { dirty_ = true; }
    std::string page_source_name() const override { return name_; }

    // For the shell ───────────────────────────────────────────────────────
    const std::string& css() const { return css_; }
    const std::vector<ConsoleLine>& console_lines() const { return console_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // Routes a click at a node (hit-tested by the shell) to script handlers.
    void click(const dom::Node* node);

    // Test/CLI bridge: click the first element with this id.
    void click_by_id(const std::string& id);

private:
    void collect_css();
    void run_scripts();

    std::unique_ptr<dom::Node> root_;
    script::Engine             engine_{*this};
    std::vector<ConsoleLine>   console_;
    std::string                title_;
    std::string                name_;
    std::string                css_;
    bool                       dirty_ = true;
};

} // namespace luxium::page
