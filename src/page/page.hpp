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

using luxium::EventData;

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
    // `blocks_origin`: whose cached blocks the answer may refer to (default: the origin of `name`).
    bool load_luxp(const std::string& bytes, const std::string& name, const std::string& blocks_origin = std::string());


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

    // The shell sets this: history.push/replace changed the location (no load).
    std::function<void(const std::string& url, bool replace)> on_location;

    // What the shell lends the page (see script/engine.hpp): geometry, focus,
    // scroll, fullscreen, clipboard; and the <video> player.
    ShellHooks& hooks() override { return hooks_; }
    void set_media(MediaApi* m) { media_ = m; }
    MediaApi* media() override { return media_; }
    // Where storage.* persists (default: the main profile).
    void set_profile_dir(std::string dir) { profile_dir_ = std::move(dir); }
    std::string profile_dir() const override { return profile_dir_; }

    // Input, as events: the shell calls these.  `node` null = the page's
    // active element / the document.  Return true if a handler prevented it.
    // Returns true when a handler called event.prevent().
    bool dispatch(const std::string& kind, const dom::Node* node, const EventData& d = {});
    bool key_event(bool down, const EventData& d, const dom::Node* target = nullptr);
    void dispatch_window(const std::string& kind, const EventData& d = {}) { engine_.dispatch_window(kind, d); }
    // The page's own back/forward.  False = nothing to go back to inside it.
    bool history_go(int delta) { return engine_.history_go(delta); }
    // Where history.push put the page (== name() until then).
    const std::string& location() const { return engine_.location(); }

    // PageHost ────────────────────────────────────────────────────────────
    void        console(int level, const std::string& msg) override;
    std::string page_title() const override { return title_; }
    void        set_page_title(const std::string& t) override { title_ = t; }
    void        location_changed(const std::string& url, bool replace) override {
        if (on_location) on_location(url, replace);
    }
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
    // Every event the shell or the form code fires goes through Page::dispatch (kind, node, EventData) and
    // therefore through the script engine; `event_tap` also sees each one (logging, tests).  Kinds fired
    // by the engine side of the page: input, change, submit, focus, blur, keydown, keyup, mouseenter,
    // mouseleave, mousemove, wheel, scroll (EventData::x/y = the new scroll offset), click, visibilitychange.
    std::function<void(const std::string& kind, const dom::Node* target, const EventData&)> event_tap;
    // True when a script listens for `kind`: the shell skips the bookkeeping of events nobody wants.
    bool wants_event(const std::string& kind) const { return engine_.has_listener(kind) || event_tap; }

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

    // Routes a click at a node (hit-tested by the shell): the "click" event
    // goes through the script handlers, then the default action runs unless
    // one called event.prevent() (link navigation, form submit, <dialog>).
    void click(const dom::Node* node, float x = 0, float y = 0, int button = 0);

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
    ShellHooks                 hooks_;
    MediaApi*                  media_ = nullptr;
    std::string                profile_dir_;
};

} // namespace luxium::page
