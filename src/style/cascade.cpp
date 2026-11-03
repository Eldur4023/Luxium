#include "style/cascade.hpp"

#include <style/properties.hpp>
#include <style/values.hpp>

#include <algorithm>
#include <cmath>

namespace luxium::style {

extern const char* const kUAStylesheet;

float ComputedStyle::line_px() const {
    if (lh_mode == 1) return font_size * lh_val;
    if (lh_mode == 2) return lh_val;
    // Blink: round(ascent) + round(descent) of the face (DejaVu: 1901/483 per 2048 em)
    return std::round(font_size * (1901.f / 2048.f)) + std::round(font_size * (483.f / 2048.f));
}

bool ComputedStyle::is_blockish() const {
    switch (display) {
    case Display::Block: case Display::Flex: case Display::Grid: case Display::Table:
    case Display::ListItem: case Display::FlowRoot: case Display::TableCaption: return true;
    default: return false;
    }
}

ComputedStyle inherited_style(const ComputedStyle& p) {
    ComputedStyle s;
    s.color = p.color; s.font_size = p.font_size; s.font_weight = p.font_weight;
    s.italic = p.italic; s.family = p.family; s.family_name = p.family_name;
    s.lh_mode = p.lh_mode; s.lh_val = p.lh_val;
    s.text_align = p.text_align; s.text_transform = p.text_transform;
    s.letter_spacing = p.letter_spacing; s.word_spacing = p.word_spacing;
    s.text_indent = p.text_indent; s.white_space = p.white_space;
    s.visibility = p.visibility; s.cursor = p.cursor;
    s.list_style = p.list_style; s.list_inside = p.list_inside;
    s.border_collapse = p.border_collapse;
    s.border_spacing_h = p.border_spacing_h; s.border_spacing_v = p.border_spacing_v;
    s.overflow_wrap = p.overflow_wrap; s.word_break = p.word_break;
    s.pointer_events = p.pointer_events; s.accent = p.accent;
    s.text_overflow_ellipsis = false;
    s.custom = p.custom;
    for (float& w : s.border_w) w = 0;   // style none: computes to 0 (the struct default is `medium`)
    return s;
}

static const std::vector<Rule>& ua_rules() {
    static const std::vector<Rule> rules = [] {
        std::vector<Rule> r;
        parse_sheet(kUAStylesheet, r);
        return r;
    }();
    return rules;
}

static const RuleIndex& ua_index() {
    static const RuleIndex idx = [] { RuleIndex i; i.build(ua_rules()); return i; }();
    return idx;
}

Resolver::Resolver() : ua_(ua_rules()), ua_idx_(ua_index()) {}

void RuleIndex::build(const std::vector<Rule>& rules) {
    by_id.clear(); by_class.clear(); by_tag.clear(); universal.clear();
    for (size_t i = 0; i < rules.size(); ++i) {
        bool uni = false;
        std::vector<std::pair<int, std::string>> keys;   // 0 id, 1 class, 2 tag
        for (const auto& sel : rules[i].selectors) {
            if (sel.parts.empty()) continue;
            const Compound& c = sel.parts.back();
            const SimpleSel* id = nullptr; const SimpleSel* cls = nullptr;
            for (const auto& ss : c.simples) {
                if (ss.kind == SimpleSel::Kind::Id && !id) id = &ss;
                else if (ss.kind == SimpleSel::Kind::Class && !cls) cls = &ss;
            }
            if (id) keys.push_back({0, id->name});
            else if (cls) keys.push_back({1, cls->name});
            else if (!c.tag.empty() && c.tag != "*") keys.push_back({2, c.tag});
            else uni = true;
        }
        if (uni) { universal.push_back(int(i)); continue; }
        for (auto& [k, name] : keys) {
            auto& v = (k == 0 ? by_id : k == 1 ? by_class : by_tag)[name];
            if (v.empty() || v.back() != int(i)) v.push_back(int(i));
        }
    }
}

namespace {

// Replaces var(--x[, fallback]) using `custom`.  Returns false if a var()
// has neither a value nor a fallback (the declaration is then invalid).
bool substitute_vars(const std::string& in, const std::map<std::string, std::string>& custom,
                     std::string& out, int depth = 0) {
    if (depth > 16) return false;
    out.clear();
    size_t i = 0;
    while (i < in.size()) {
        const size_t p = in.find("var(", i);
        if (p == std::string::npos) { out += in.substr(i); break; }
        // `var(` must not be the tail of another identifier (e.g. "mvar(")
        if (p > 0 && (std::isalnum(static_cast<unsigned char>(in[p - 1])) || in[p - 1] == '-' || in[p - 1] == '_')) {
            out += in.substr(i, p + 4 - i);
            i = p + 4;
            continue;
        }
        out += in.substr(i, p - i);
        int d = 0;
        size_t q = p + 3;
        for (; q < in.size(); ++q) {
            if (in[q] == '(') ++d;
            if (in[q] == ')' && --d == 0) break;
        }
        if (q >= in.size()) return false;
        const std::string inner = in.substr(p + 4, q - p - 4);
        // name [, fallback]
        int pd = 0;
        size_t comma = std::string::npos;
        for (size_t k = 0; k < inner.size(); ++k) {
            if (inner[k] == '(') ++pd;
            if (inner[k] == ')') --pd;
            if (inner[k] == ',' && pd == 0) { comma = k; break; }
        }
        const std::string name = trim_ws(inner.substr(0, comma));
        auto it = custom.find(name);
        std::string val;
        if (it != custom.end()) {
            if (!substitute_vars(it->second, custom, val, depth + 1)) return false;
        } else if (comma != std::string::npos) {
            if (!substitute_vars(trim_ws(inner.substr(comma + 1)), custom, val, depth + 1)) return false;
        } else return false;
        out += val;
        i = q + 1;
    }
    return true;
}

std::string attr_function(const std::string& value, const dom::Node& el) {
    std::string out;
    size_t i = 0;
    while (i < value.size()) {
        const size_t p = value.find("attr(", i);
        if (p == std::string::npos) { out += value.substr(i); break; }
        out += value.substr(i, p - i);
        const size_t q = value.find(')', p);
        if (q == std::string::npos) { out += value.substr(p); break; }
        const std::string name = trim_ws(value.substr(p + 5, q - p - 5));
        std::string v = el.attr_or(name);
        std::string esc;
        for (char c : v) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
        out += "\"" + esc + "\"";
        i = q + 1;
    }
    return out;
}


// HTML presentational attributes (bgcolor, border, cellpadding, align...):
// Blink maps them to declarations that sit just above the UA sheet and below
// every author rule.
std::vector<Decl> presentational_hints(const dom::Node& el) {
    std::vector<Decl> out;
    auto add = [&](const char* n, const std::string& v) { out.push_back({n, v, false}); };
    auto dim = [](const std::string& v) -> std::string {   // "100" -> 100px, "50%" -> 50%
        std::string t = trim_ws(v);
        if (t.empty()) return "";
        if (t.back() == '%') { float n; return parse_number(t.substr(0, t.size() - 1), n) ? t : ""; }
        float n;
        return parse_number(t, n) ? t + "px" : "";
    };
    const std::string& tag = el.name;
    if (el.has_attr("bgcolor")) add("background-color", el.attr_or("bgcolor"));
    if (tag == "table") {
        if (el.has_attr("border")) {
            float n = 0;
            const std::string b = trim_ws(el.attr_or("border"));
            if (b.empty() || !parse_number(b, n)) n = b.empty() ? 1 : 0;
            if (n > 0) {
                add("border-width", std::to_string(int(n)) + "px");
                add("border-style", "outset");
            }
        }
        if (el.has_attr("cellspacing")) { const auto d = dim(el.attr_or("cellspacing")); if (!d.empty()) add("border-spacing", d); }
        if (el.has_attr("width")) { const auto d = dim(el.attr_or("width")); if (!d.empty()) add("width", d); }
        if (el.has_attr("height")) { const auto d = dim(el.attr_or("height")); if (!d.empty()) add("height", d); }
        const std::string al = lower_ascii(el.attr_or("align"));
        if (al == "center") { add("margin-left", "auto"); add("margin-right", "auto"); }
        else if (al == "left") add("float", "left");
        else if (al == "right") add("float", "right");
    } else if (tag == "td" || tag == "th") {
        const dom::Node* tbl = el.parent;
        while (tbl && tbl->name != "table") tbl = tbl->parent;
        if (tbl) {
            if (tbl->has_attr("border")) {
                float n = 0;
                const std::string b = trim_ws(tbl->attr_or("border"));
                if (b.empty() || !parse_number(b, n)) n = b.empty() ? 1 : 0;
                if (n > 0) { add("border-width", "1px"); add("border-style", "inset"); }
            }
            if (tbl->has_attr("cellpadding")) { const auto d = dim(tbl->attr_or("cellpadding")); if (!d.empty()) add("padding", d); }
        }
        if (el.has_attr("width")) { const auto d = dim(el.attr_or("width")); if (!d.empty()) add("width", d); }
        if (el.has_attr("height")) { const auto d = dim(el.attr_or("height")); if (!d.empty()) add("height", d); }
        const std::string al = lower_ascii(el.attr_or("align"));
        if (al == "left" || al == "right" || al == "center") add("text-align", al);
        const std::string va = lower_ascii(el.attr_or("valign"));
        if (va == "top" || va == "middle" || va == "bottom" || va == "baseline") add("vertical-align", va);
    } else if (tag == "tr" || tag == "thead" || tag == "tbody" || tag == "tfoot") {
        const std::string al = lower_ascii(el.attr_or("align"));
        if (al == "left" || al == "right" || al == "center") add("text-align", al);
        const std::string va = lower_ascii(el.attr_or("valign"));
        if (va == "top" || va == "middle" || va == "bottom" || va == "baseline") add("vertical-align", va);
    } else if (tag == "img" || tag == "video") {
        if (el.has_attr("width")) { const auto d = dim(el.attr_or("width")); if (!d.empty()) add("width", d); }
        if (el.has_attr("height")) { const auto d = dim(el.attr_or("height")); if (!d.empty()) add("height", d); }
    } else if (tag == "div" || tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
               tag == "h5" || tag == "h6") {
        const std::string al = lower_ascii(el.attr_or("align"));
        if (al == "left" || al == "right" || al == "center" || al == "justify") add("text-align", al);
    } else if (tag == "font") {
        if (el.has_attr("color")) add("color", el.attr_or("color"));
    } else if (tag == "body") {
        if (el.has_attr("text")) add("color", el.attr_or("text"));
    }
    return out;
}

struct Entry {
    const Decl* decl;
    int rank;        // 0 UA, 1 author, 2 inline  (flipped for !important)
    int spec;
    int order;
    bool important;
};

} // namespace

ComputedStyle Resolver::compute(const dom::Node& el, const ComputedStyle* parent, Pseudo pseudo) const {
    // ── 1. matching declarations ───────────────────────────────────────────
    std::vector<Entry> entries;
    thread_local std::vector<uint32_t> stamp;
    thread_local uint32_t gen = 0;
    auto collect = [&](const std::vector<Rule>& rules, const RuleIndex& idx, int rank) {
        if (stamp.size() < rules.size()) stamp.resize(rules.size(), 0);
        ++gen;
        std::vector<int> cand = idx.universal;
        auto add = [&](const std::unordered_map<std::string, std::vector<int>>& m, const std::string& k) {
            auto it = m.find(k);
            if (it != m.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
        };
        add(idx.by_tag, el.name);
        const std::string id = el.attr_or("id");
        if (!id.empty()) add(idx.by_id, id);
        {
            const std::string cl = el.attr_or("class");
            size_t i = 0;
            while (i < cl.size()) {
                while (i < cl.size() && std::isspace(static_cast<unsigned char>(cl[i]))) ++i;
                size_t j = i;
                while (j < cl.size() && !std::isspace(static_cast<unsigned char>(cl[j]))) ++j;
                if (j > i) add(idx.by_class, cl.substr(i, j - i));
                i = j;
            }
        }
        for (int ri : cand) {
            if (stamp[size_t(ri)] == gen) continue;
            stamp[size_t(ri)] = gen;
            const Rule& r = rules[size_t(ri)];
            if (!r.media.empty() && !media_matches(r.media, vw_, vh_)) continue;
            int best = -1;
            for (const auto& sel : r.selectors) {
                if (sel.pseudo != pseudo) continue;
                if (matches(sel, el)) best = std::max(best, sel.specificity);
            }
            if (best < 0) continue;
            for (const auto& d : r.decls) entries.push_back({&d, rank, best, r.order, d.important});
        }
    };
    collect(ua_, ua_idx_, 0);
    collect(*author_, author_idx_, 1);
    std::vector<Decl> hint_decls;
    if (pseudo == Pseudo::None) {
        hint_decls = presentational_hints(el);
        int n = 0;
        for (const auto& d : hint_decls) entries.push_back({&d, 1, -1, n++, false});
    }
    std::vector<Decl> inline_decls;
    if (pseudo == Pseudo::None && el.has_attr("style")) {
        inline_decls = parse_decls(el.attr_or("style"));
        int n = 0;
        for (const auto& d : inline_decls) entries.push_back({&d, 2, 0, n++, d.important});
    }
    // CSS order: normal (UA < author < inline), then !important (author < inline < UA... UA important is rare)
    auto key = [](const Entry& e) {
        const int tier = e.important ? 3 + (e.rank == 0 ? 2 : e.rank - 1 + 0) : e.rank;
        return tier;
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        const int ka = key(a), kb = key(b);
        if (ka != kb) return ka < kb;
        if (a.spec != b.spec) return a.spec < b.spec;
        return a.order < b.order;
    });

    // Same matched declarations + same parent style + same tag = same result.  Presentational attributes and
    // inline `style` are per element, so those elements are computed for real.
    const bool cacheable = hint_decls.empty() && inline_decls.empty();
    std::string ckey;
    if (cacheable) {
        ckey.reserve(16 + entries.size() * sizeof(void*));
        const uint32_t pid = parent ? parent->cache_id : 0;
        ckey.append(reinterpret_cast<const char*>(&pid), sizeof pid);
        ckey.push_back(char(pseudo));
        ckey += el.name;
        ckey.push_back('\0');
        for (const Entry& e : entries) { const Decl* d = e.decl; ckey.append(reinterpret_cast<const char*>(&d), sizeof d); }
        if (parent && pid == 0) ckey.clear();   // an unidentified parent (hand-built style): no sharing
        if (!ckey.empty()) {
            auto hit = cache_.find(ckey);
            if (hit != cache_.end()) {
                ComputedStyle r = hit->second;
                if (pseudo == Pseudo::None && post_hook && post_hook(el, r)) r.cache_id = ++next_id_;
                return r;
            }
        }
    }

    // ── 2. starting point: inherited properties from the parent ────────────
    ComputedStyle s;
    ComputedStyle defaults;
    if (parent) s = inherited_style(*parent);
    // table sections inherit border-color from the table in Blink; border-color
    // here is not inherited, so `border-color: inherit` is honoured below.

    Env env;
    env.parent = parent;
    env.root_em = parent ? root_em_ : 16.f;
    env.vw = vw_; env.vh = vh_;
    env.em = parent ? parent->font_size : 16.f;

    // ── 3. apply: custom props + order-independent basics first, then the rest
    auto resolve = [&](const Decl& d, std::string& value) -> bool {
        value = d.value;
        if (value.find("var(") != std::string::npos) {
            std::string out;
            if (!substitute_vars(value, s.custom, out)) return false;
            value = out;
        }
        if (d.name == "content" && value.find("attr(") != std::string::npos) value = attr_function(value, el);
        return true;
    };
    auto apply_one = [&](const Decl& d) {
        std::string value;
        if (!resolve(d, value)) return;   // invalid at computed-value time: ignored
        const std::string lv = lower_ascii(trim_ws(value));
        if (lv == "inherit" || lv == "initial" || lv == "unset" || lv == "revert" || lv == "revert-layer") {
            const bool inh = lv == "inherit" || ((lv == "unset" || lv == "revert" || lv == "revert-layer") && property_inherits(d.name));
            if (inh && parent) copy_property(s, *parent, d.name);
            else copy_property(s, defaults, d.name);
            return;
        }
        apply_property(s, env, d.name, value);
    };

    for (const auto& e : entries) {
        const Decl& d = *e.decl;
        if (d.name.starts_with("--")) { s.custom[d.name] = d.value; continue; }
    }
    // custom properties may reference each other: resolve once more
    for (auto& [k, v] : s.custom) {
        std::string out;
        if (v.find("var(") != std::string::npos && substitute_vars(v, s.custom, out)) v = out;
    }
    for (const auto& e : entries) {
        const std::string& n = e.decl->name;
        if (n == "font-size" || n == "font") {
            // font-size's own em refers to the PARENT's size
            env.em = parent ? parent->font_size : 16.f;
            apply_one(*e.decl);
        }
    }
    env.em = s.font_size;
    for (const auto& e : entries) {
        const std::string& n = e.decl->name;
        if (n == "color" || n == "font-weight" || n == "font-family" || n == "font-style") apply_one(*e.decl);
    }
    for (const auto& e : entries) {
        const std::string& n = e.decl->name;
        if (n.starts_with("--") || n == "font-size" || n == "font" || n == "color" ||
            n == "font-weight" || n == "font-family" || n == "font-style") continue;
        apply_one(*e.decl);
    }

    // ── 4. post-processing ─────────────────────────────────────────────────
    if (!parent) root_em_ = s.font_size;
    for (int i = 0; i < 4; ++i) {
        if (s.border_style[i] == BorderStyle::None || s.border_style[i] == BorderStyle::Hidden) s.border_w[i] = 0;
        if (s.bc_current[i]) s.border_color[i] = s.color;
    }
    if (s.decoration_color == 0) s.decoration_color = 0;   // 0 = currentcolor
    // floated / absolutely positioned boxes are blockified
    if (s.float_ != Float::None || s.position == Position::Absolute || s.position == Position::Fixed) {
        switch (s.display) {
        case Display::Inline: case Display::InlineBlock: case Display::TableRow: case Display::TableCell:
        case Display::TableRowGroup: case Display::TableHeaderGroup: case Display::TableFooterGroup:
        case Display::TableCaption: case Display::TableColumn: case Display::TableColumnGroup:
            s.display = Display::Block; break;
        case Display::InlineFlex: s.display = Display::Flex; break;
        case Display::InlineGrid: s.display = Display::Grid; break;
        case Display::InlineTable: s.display = Display::Table; break;
        default: break;
        }
    }
    // overflow: when only one axis scrolls/clips, `visible` computes to auto and `clip` to hidden on the other
    {
        auto plain = [](Overflow o) { return o == Overflow::Visible || o == Overflow::Clip; };
        if (plain(s.overflow_x) != plain(s.overflow_y)) {
            Overflow& p = plain(s.overflow_x) ? s.overflow_x : s.overflow_y;
            p = p == Overflow::Visible ? Overflow::Auto : Overflow::Hidden;
        }
    }
    if (el.name == "input") {   // Blink: clipping on an <input> is always `clip`
        if (s.overflow_x == Overflow::Hidden) s.overflow_x = Overflow::Clip;
        if (s.overflow_y == Overflow::Hidden) s.overflow_y = Overflow::Clip;
    }
    if (s.position == Position::Absolute || s.position == Position::Fixed) s.float_ = Float::None;
    if (pseudo != Pseudo::None && !s.has_content) s.display = Display::None;
    // children of a flex/grid container are blockified
    if (parent && (parent->display == Display::Flex || parent->display == Display::InlineFlex ||
                   parent->display == Display::Grid || parent->display == Display::InlineGrid)) {
        switch (s.display) {
        case Display::Inline: case Display::InlineBlock: case Display::TableRow: case Display::TableCell:
        case Display::TableRowGroup: case Display::TableHeaderGroup: case Display::TableFooterGroup:
        case Display::TableCaption: s.display = Display::Block; break;
        case Display::InlineFlex: s.display = Display::Flex; break;
        case Display::InlineGrid: s.display = Display::Grid; break;
        case Display::InlineTable: s.display = Display::Table; break;
        default: break;
        }
    }
    s.cache_id = ++next_id_;
    if (cacheable && !ckey.empty()) cache_.emplace(ckey, s);
    if (pseudo == Pseudo::None && post_hook && post_hook(el, s)) s.cache_id = ++next_id_;
    return s;
}

} // namespace luxium::style
