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
#include <page/luxp.hpp>
#include <style/stylesheet.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luxium::page {

// Everything the user does to the page, in one record.  The shell (own engine) fires these through
// Page::dispatch(); whoever scripts the page (the LuxScript runtime) sets Page::on_event and routes
// them to handlers.  A handler that wants to cancel the default action sets `prevented`.
struct FormEvent {
    enum class Kind { Input, Change, Submit, Focus, Blur, KeyDown, KeyUp, MouseEnter, MouseLeave, MouseMove, Scroll };
    Kind        kind = Kind::Input;
    dom::Node*  target = nullptr;     // the element concerned (focus owner, hovered element, scrolled box...)
    std::string key;                  // KeyDown/KeyUp: "a", "Enter", "ArrowDown", "Tab"...; Input: the inserted text
    std::string value;                // Input/Change: the control's value after the edit
    int         mods = 0;             // bit 1 shift, 2 ctrl, 4 alt, 8 meta
    float       x = 0, y = 0;         // MouseMove: document coords; Scroll: new scroll offset
    bool        prevented = false;
};

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
    // A .luxp received over the network (its entry page; resources are not
    // fetched yet).
    bool load_luxp(const std::string& bytes, const std::string& name);


    // The shell sets this: a click on (or under) an <a href> calls it with
    // the raw href.  The shell decides what it means (file, http, #anchor).
    std::function<void(const std::string& href)> on_navigate;
    // A click landed on a <video>/<audio> (not on its controls): play/pause.
    std::function<void(const dom::Node* media)> on_media_click;

    // The bytes currently displayed (view-source).
    const std::string& source() const { return source_; }

    // La pagina pide JavaScript?  Cualquier <script type="text/javascript">
    // o <script src> externo la marca: en la web real, src es JS.
    bool used_javascript() const { return saw_js_; }
    const std::string& name() const { return name_; }

    // El directorio del documento cargado (para resolver <img>/<link> relativos)
    std::string base_dir() const {
        const size_t slash = name_.find_last_of('/');
        return slash == std::string::npos ? std::string() : name_.substr(0, slash + 1);
    }

    // PageHost ────────────────────────────────────────────────────────────
    void        console(int level, const std::string& msg) override;
    std::string page_title() const override { return title_; }
    dom::Node*  page_root() override { return root_.get(); }
    void        page_mutated() override { dirty_ = true; }
    std::string page_source_name() const override { return name_; }

    // For the shell ───────────────────────────────────────────────────────
    // Author rules (<style>, <link>, @import flattened), parsed once per load
    // -- or decoded from a .luxp.  The layout reads them every relayout.
    const std::vector<style::Rule>& rules() const { return sheet_; }
    bool uses_hover() const { return uses_hover_; }
    bool dark_scheme() const { return dark_scheme_; }   // <meta name=color-scheme content=dark>
    const std::vector<ConsoleLine>& console_lines() const { return console_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // ── forms and focus (page/forms.cpp) ──────────────────────────────────────
    // THE dispatch point: every FormEvent goes through here.  Scripting hooks `on_event`; with none set
    // the default actions below still happen.
    std::function<void(FormEvent&)> on_event;
    // Fires `e`; returns false when a handler prevented the default action.
    bool dispatch(FormEvent& e);

    // Value of <input>/<textarea> (the `value` attribute / the text child), <select> (the selected
    // option's value or text).  Setters change the DOM and mark the page dirty; they fire no events.
    std::string form_value(const dom::Node* n) const;
    void        set_form_value(dom::Node* n, const std::string& v);
    bool        form_checked(const dom::Node* n) const;          // checkbox / radio
    void        set_form_checked(dom::Node* n, bool on);
    int         select_index(const dom::Node* select) const;
    void        select_option(dom::Node* select, int index);      // fires Input + Change
    std::vector<std::string> form_files(const dom::Node* n) const { auto it = files_.find(n); return it == files_.end() ? std::vector<std::string>{} : it->second; }
    void        set_form_files(dom::Node* n, std::vector<std::string> paths);   // file input (full paths)

    dom::Node*  focused() const { return focus_; }
    // Moves focus (null = blur).  `visible`: keyboard-driven, so :focus-visible applies (text fields always match).
    void        focus(dom::Node* n, bool visible = false);
    void        blur() { focus(nullptr); }
    bool        focusable(const dom::Node* n) const;
    void        focus_next(bool backwards);                       // Tab / Shift+Tab, document (tabindex 0) order
    static bool is_text_control(const dom::Node* n);              // text-like <input> or <textarea>

    // Editing of the focused text control (caret/anchor are byte offsets).  Each fires Input.
    void        edit_insert(const std::string& utf8);
    void        edit_backspace(bool word);
    void        edit_delete(bool word);
    void        edit_move(int dir, bool word, bool extend);       // dir -1 / +1
    void        edit_home_end(bool end, bool extend);
    void        edit_select_all();
    void        edit_set_caret(int byte, bool extend);
    std::string edit_selection() const;
    bool        edit_replace_selection(const std::string& utf8);  // cut / paste; true if the value changed
    // Enter in a text control: fires Submit on the enclosing <form> (default action: none).
    void        edit_enter();
    // A primary click that lands on `n` (not links): toggles checkbox/radio, label -> its control,
    // focuses fields.  Returns true when it consumed the click (then Page::click should not run).
    bool        activate(dom::Node* n);
    void        edit_select_word();                               // double click: the word around the caret
    void        drop_stale_focus();                               // focus owner left the document (set_html...)

    // Routes a click at a node (hit-tested by the shell) to script handlers.
    void click(const dom::Node* node);

    // Fires due timers (the shell calls it once per frame).
    void tick() { engine_.tick(); }
    void free_removed() { engine_.free_removed(); }

    // Does this node (or an ancestor?) carry a click handler registered from
    // LuxScript?  The shell asks to decide click-vs-text-selection.
    bool has_click_handler(const dom::Node* node) const;

    // Test/CLI bridge: click the first element with this id.
    void click_by_id(const std::string& id);

private:
    bool load_compiled(const PageSections& s, const std::string& name);
    void reset_for_load(const std::string& name);
    void set_rules(std::vector<style::Rule> rules);

    std::unique_ptr<dom::Node> root_;
    script::Engine             engine_{*this};
    std::vector<ConsoleLine>   console_;
    std::string                title_;
    std::string                name_;
    std::vector<style::Rule>   sheet_;
    bool                       uses_hover_ = false;
    bool                       dark_scheme_ = false;
    dom::Node*                 focus_ = nullptr;
    std::map<const dom::Node*, std::vector<std::string>> files_;
    bool                       saw_js_ = false;
    std::string                source_;
    bool                       dirty_ = true;
};

} // namespace luxium::page
