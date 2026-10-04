#include "render/layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace luxium::render {

namespace {

// ── The UA stylesheet: the browser's default look, as CSS ───────────────────
// It goes through the same parser as page CSS; a page rule wins purely by
// specificity, no special cases.  Properties that inherit (color, size,
// line-height, family, align) are deliberately absent from the tags that
// should inherit them.
constexpr std::string_view kUAStylesheet = R"css(
body       { font-size: 15.5px; line-height: 1.6; color: #232936; }
h1         { font-size: 31px; font-weight: bold; line-height: 1.25;
             color: #141926; margin: 6px 0 16px 0; }
h2         { font-size: 22px; font-weight: bold; line-height: 1.3;
             color: #141926; margin: 26px 0 10px 0; }
h3         { font-size: 18px; font-weight: bold; line-height: 1.3;
             color: #141926; margin: 20px 0 8px 0; }
p          { margin: 0 0 12px 0; }
li         { margin: 0 0 7px 26px; }
b, strong  { font-weight: bold; }
a          { color: #2F6FED; }
code       { font-family: monospace; font-size: 13.5px; color: #933B32;
             background-color: #EDEFF3; }
blockquote { color: #646B76; margin: 10px 0 10px 18px; }
button     { font-size: 14.5px; font-weight: bold; line-height: 1.2;
             color: #FFFFFF; }
pre        { font-family: monospace; font-size: 13px; line-height: 1.5;
             color: #37424E; }
)css";

constexpr std::string_view kSkipped[] = {
    "head", "title", "script", "style", "meta", "link", "template",
};
constexpr std::string_view kBlocks[] = {
    "body", "html", "h1", "h2", "h3", "p", "ul", "ol", "li", "pre",
    "blockquote", "button", "div", "hr", "br", "section",
};

bool in_list(std::string_view name, const std::string_view* list, size_t n) {
    return std::find(list, list + n, name) != list + n;
}

// ── computed style ──────────────────────────────────────────────────────────

struct Style {
    // text face (what a word needs)
    float    size  = 15.5f;
    bool     bold  = false;
    bool     mono  = false;
    uint32_t color = 0xFF232936;
    float    lh    = 1.6f;
    uint32_t pill  = 0;      // inline background (code); painted per word
    // boxes
    float    mt = 0, mb = 0, ml = 0;
    float    pt = 0, pr = 0, pb = 0, pl = 0;
    uint32_t block_bg = 0;
    bool     has_block_bg = false;
    int      align = 0;      // 0 left, 1 center, 2 right
    bool     display_none = false;
};

uint32_t parse_color(const std::string& v, uint32_t fallback) {
    std::string s;
    for (char ch : v)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += static_cast<char>(std::tolower(ch));
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() == 6) {
        unsigned long rgb = 0;
        try { rgb = std::stoul(s, nullptr, 16); } catch (...) { return fallback; }
        return 0xFF000000u | static_cast<uint32_t>(rgb);
    }
    if (s.size() == 3) {
        unsigned long rgb = 0;
        try { rgb = std::stoul(s + s, nullptr, 16); } catch (...) { return fallback; }
        return 0xFF000000u | static_cast<uint32_t>(rgb);
    }
    static const std::array<std::pair<std::string_view, uint32_t>, 12> kNames{{
        {"white", 0xFFFFFFFF}, {"black", 0xFF000000}, {"red", 0xFFCC2E2E},
        {"green", 0xFF2E8B3A}, {"blue", 0xFF2F6FED}, {"gray", 0xFF808690},
        {"grey", 0xFF808690}, {"orange", 0xFFE88A2E}, {"yellow", 0xFFE8C22E},
        {"purple", 0xFF8A4FBF}, {"pink", 0xFFE86AA6}, {"brown", 0xFF8A5A2E},
    }};
    for (const auto& [n, c] : kNames)
        if (s == n) return c;
    return fallback;
}

float parse_len(const std::string& v, float parent_size, float fallback) {
    std::string s;
    for (char ch : v)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += static_cast<char>(std::tolower(ch));
    try {
        if (s.ends_with("px"))  return std::stof(s);
        if (s.ends_with("em"))  return std::stof(s) * parent_size;
        if (s.ends_with("rem")) return std::stof(s) * 15.5f;
        if (s.ends_with("%"))   return std::stof(s) / 100.f * parent_size;
        if (!s.empty() && (std::isdigit(s[0]) || s[0] == '.' || s[0] == '-'))
            return std::stof(s);
    } catch (...) {}
    return fallback;
}

struct StyleResolver {
    std::vector<css::Rule> ua;
    std::vector<css::Rule> page;

    explicit StyleResolver(const std::string& page_css) {
        ua   = css::parse_stylesheet(std::string(kUAStylesheet));
        page = css::parse_stylesheet(page_css);
    }

    // One declaration onto a style.  `parent` is only for relative units.
    void apply(Style& s, const Style& parent, const std::string& prop,
               const std::string& v) const {
        if (prop == "color")                    s.color = parse_color(v, s.color);
        else if (prop == "background-color") {
            s.block_bg = parse_color(v, s.block_bg);
            s.has_block_bg = true;
            if (!s.mono) s.pill = s.block_bg;   // inline pills, too (code)
        }
        else if (prop == "font-size")           s.size = std::max(6.f, parse_len(v, parent.size, s.size));
        else if (prop == "font-weight")         s.bold = (v.find("bold") != std::string::npos ||
                                                          v == "700" || v == "800" || v == "900");
        else if (prop == "font-family")         s.mono = v.find("mono") != std::string::npos;
        else if (prop == "line-height") {
            if (v.find("px") != std::string::npos) s.lh = parse_len(v, s.size, s.lh) / s.size;
            else { try { s.lh = std::stof(v); } catch (...) {} }
        }
        else if (prop == "text-align") {
            if (v == "center") s.align = 1;
            else if (v == "right") s.align = 2;
            else s.align = 0;
        }
        else if (prop == "display")             s.display_none = (v == "none");
        else if (prop == "margin" || prop == "padding") {
            float t, r, b, l;
            if (!sides(v, t, r, b, l)) return;
            if (prop == "margin") { s.mt = t; s.mb = b; s.ml = l; }
            else                  { s.pt = t; s.pr = r; s.pb = b; s.pl = l; }
        }
        else if (prop == "margin-top")    s.mt = parse_len(v, s.size, s.mt);
        else if (prop == "margin-bottom") s.mb = parse_len(v, s.size, s.mb);
        else if (prop == "margin-left")   s.ml = parse_len(v, s.size, s.ml);
        else if (prop == "margin-right")  { /* vertical layout never uses it */ }
        else if (prop == "padding-top")    s.pt = parse_len(v, s.size, s.pt);
        else if (prop == "padding-bottom") s.pb = parse_len(v, s.size, s.pb);
        else if (prop == "padding-left")   s.pl = parse_len(v, s.size, s.pl);
        else if (prop == "padding-right")  s.pr = parse_len(v, s.size, s.pr);
    }

    // `1px` / `1px 2px` / `1px 2px 3px` / `1px 2px 3px 4px` -> t r b l
    static bool sides(const std::string& v, float& t, float& r, float& b, float& l) {
        std::vector<float> vals;
        std::string cur;
        for (char ch : v) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!cur.empty()) { vals.push_back(parse_len(cur, 0, 0)); cur.clear(); }
            } else cur += ch;
        }
        if (!cur.empty()) vals.push_back(parse_len(cur, 0, 0));
        if (vals.empty() || vals.size() > 4) return false;
        t = vals[0];
        r = vals.size() > 1 ? vals[1] : t;
        b = vals.size() > 2 ? vals[2] : t;
        l = vals.size() > 3 ? vals[3] : r;
        return true;
    }

    // The cascade, in CSS order: UA rules (specificity, order), then page
    // rules, then the style="" attribute.
    Style compute(const dom::Node& el, const Style& parent) const {
        Style s;
        s.size = parent.size; s.bold = parent.bold; s.mono = parent.mono;
        s.color = parent.color; s.lh = parent.lh; s.align = parent.align;

        auto fold = [&](const std::vector<css::Rule>& rules) {
            std::vector<std::pair<int, const css::Rule*>> matched;
            for (const auto& r : rules) {
                int spec = r.match(el);
                if (spec >= 0) matched.push_back({spec, &r});
            }
            std::stable_sort(matched.begin(), matched.end(),
                             [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [spec, r] : matched)
                for (const auto& [prop, val] : r->decls) apply(s, parent, prop, val);
        };
        fold(ua);
        fold(page);

        for (const auto& [prop, val] : css::parse_decls(el.attr_or("style")))
            apply(s, parent, prop, val);
        return s;
    }
};

// ── layout context ──────────────────────────────────────────────────────────

struct WordPending {
    float x = 0;            // from the line's left edge (alignment shifts it)
    Word  word;
    uint32_t color = 0;
    uint32_t pill = 0;      // inline background (code)
    float    lh = 0;        // for the pill's height
};

struct Ctx {
    Font&         font;
    LayoutResult& out;
    const StyleResolver* res = nullptr;

    float y      = 0;
    float line_w = 0;
    float line_h = 0;
    bool  line_has_atom = false;   // a button made the line not centerable

    float body_pad = 26;
    float measure  = 720;

    // The line being built belongs to ONE block: its left edge (margins
    // included) and its available width.  flush() needs both -- without
    // line_left, every margin-left in the document silently disappears and
    // list bullets land on top of their first letter.
    float line_left  = 0;
    float line_avail = 0;

    int   align = 0;               // of the block being laid out
    std::vector<WordPending> pending;

    // Runs glued together: `<i>Lux</i>,` is ONE word visually -- there is no
    // space in the source between the i's text and the comma, so no word gap
    // may appear at that boundary.  last_space is the gap the previous word
    // added; glue_next takes it back off the line for the first glued word.
    float last_space  = 0;
    bool  glue_next   = false;

    void flush() {
        const float off = (align == 1 && !line_has_atom) ? std::max(0.f, (line_avail - line_w) / 2.f)
                        : (align == 2 && !line_has_atom) ? std::max(0.f, line_avail - line_w)
                                                         : 0.f;
        if (std::getenv("LUXIUM_DEBUG_WORDS"))
            for (const auto& p : pending)
                std::fprintf(stderr, "word '%s' w=%.1f x=%.1f\n", p.word.text.c_str(),
                             p.word.width, p.x);
        for (auto& p : pending) {
            const float fx = line_left + p.x + off;
            if (p.pill)   // the word's pill shifts WITH the word (and centers with it)
                out.shapes.push_back({ShapeCmd::Kind::RRect, fx - 3, y + 1.5f,
                                      p.word.width + 6, p.lh - 3, 3, p.pill});
            out.words.push_back({fx, y, p.color, std::move(p.word)});
        }
        pending.clear();
        y += line_h;
        line_w = 0;
        line_h = 0;
        line_has_atom = false;
    }

    void emit(const Word& w, float x_from_left, const Style& s) {
        if (line_w == 0) line_h = s.size * s.lh;
        // The pill, if any, is emitted at flush() with the word's FINAL x
        // (alignment included) -- emitting it here would paint it at the
        // line-relative x, floating in the page margin.
        pending.push_back({x_from_left, w, s.color, s.pill, s.size * s.lh});
    }
};

void emit_word(Ctx& c, const Word& w, float left, float avail, const Style& s) {
    const float space = s.size * 0.30f;
    if (c.line_w > 0 && c.line_w + w.width > avail) c.flush();
    const float x = c.line_w == 0 ? 0 : c.line_w;
    c.line_w = x + w.width + space;
    c.line_h = std::max(c.line_h, s.size * s.lh);
    c.last_space = space;
    c.emit(w, x, s);
}

void inline_text(Ctx& c, std::string_view text, const Style& s, float left, float avail) {
    if (text.empty()) return;
    c.font.set_size(s.size);
    // A new run whose first character is not a space GLUES to the previous
    // run's last word: take the pending word gap back off the line.
    const bool first_glues = c.glue_next && c.line_w > 0 && text.front() != ' ';
    if (first_glues) c.line_w = std::max(0.f, c.line_w - c.last_space);
    for (const Word& w : shape_words(c.font, text, s.bold, s.mono))
        emit_word(c, w, left, avail, s);
    c.glue_next = !std::isspace(static_cast<unsigned char>(text.back()));
}

// Inline subtree (b/i/code/a/span): flows with the open line, each element
// taking its own computed style for its own text.
void layout_inline_children(Ctx& c, const dom::Node& el, const Style& el_style,
                            float left, float avail) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            inline_text(c, child->text, el_style, left, avail);
        } else if (child->is_element()) {
            Style cs = c.res->compute(*child, el_style);
            layout_inline_children(c, *child, cs, left, avail);
        }
    }
}

std::string flatten_text(const dom::Node& el) {
    std::string out;
    for (const auto& c : el.children) {
        if (c->type == dom::NodeType::Text) {
            if (!out.empty()) out += ' ';
            out += c->text;
        } else if (c->is_element()) {
            if (!out.empty()) out += ' ';
            out += flatten_text(*c);
        }
    }
    return out;
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail,
                  const Style& parent);

void layout_children(Ctx& c, const dom::Node& el, float left, float avail,
                     const Style& el_style) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            inline_text(c, child->text, el_style, left, avail);
        } else if (child->is_element()) {
            layout_block(c, *child, left, avail, el_style);
        }
    }
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail,
                  const Style& parent) {
    if (el.type != dom::NodeType::Element) return;
    const Style s = c.res->compute(el, parent);
    if (s.display_none) return;   // the subtree goes with it

    const std::string& tag = el.name;
    if (in_list(tag, kSkipped, std::size(kSkipped))) return;
    c.font.set_size(s.size);

    if (tag == "br") {
        c.flush();
        c.y += s.size * 0.35f;
        return;
    }
    if (tag == "hr") {
        c.flush();
        c.y += 10;
        c.out.shapes.push_back({ShapeCmd::Kind::RRect, left, c.y, avail, 1, 0.5f,
                                0xFFE3E6EA});
        c.y += 22;
        return;
    }

    const bool block = in_list(tag, kBlocks, std::size(kBlocks));

    // ── button: an inline-block atom ─────────────────────────────────────
    if (tag == "button") {
        const float padx = 16, pady = 9;
        c.font.set_size(s.size);
        auto words = shape_words(c.font, flatten_text(el), s.bold, s.mono);
        float content_w = 0;
        for (const auto& w : words) content_w += w.width + s.size * 0.30f;
        content_w -= s.size * 0.30f;
        const float bw = content_w + 2 * padx;
        const float bh = s.size * s.lh + 2 * pady;

        if (c.line_w > 0 && c.line_w + bw + 10 > avail) c.flush();
        if (c.line_w == 0) c.line_h = bh;
        const float bx = left + c.line_w;
        const float by = c.y;

        c.out.shapes.push_back({ShapeCmd::Kind::RRect, bx, by, bw, bh, 7,
                                s.has_block_bg ? s.block_bg : 0xFF2F6FED});
        float wx = bx + padx;
        for (const auto& w : words) {
            c.out.words.push_back({wx, by + pady, s.color, w});
            wx += w.width + s.size * 0.30f;
        }
        c.out.hits.push_back({&el, bx, by, bw, bh, true});

        c.line_w += bw + 10;
        c.line_h = std::max(c.line_h, bh);
        c.line_has_atom = true;
        return;
    }

    if (!block) {   // inline element
        layout_inline_children(c, el, s, left, avail);
        return;
    }

    // ── plain block, with the box model subset: background + padding ─────
    c.flush();
    c.align = s.align;
    c.glue_next = false;
    c.y += s.mt;
    c.line_left  = left + s.ml + s.pl;
    c.line_avail = avail - s.ml - s.pl - s.pr;

    const bool quote = tag == "blockquote";
    size_t quote_bar = 0;
    if (quote) {   // the accent bar, grown to the content height below
        quote_bar = c.out.shapes.size();
        c.out.shapes.push_back({ShapeCmd::Kind::RRect,
                                left + s.ml + 0, c.y + 2, 3, 10, 1.5f, 0xFFC9CDD4});
    }

    const float box_y = c.y;
    const float inner_left  = left + s.ml + s.pl;
    const float inner_avail = avail - s.ml - s.pl - s.pr;
    c.y += s.pt;

    const bool li        = tag == "li";
    const bool clickable = el.has_attr("id") || tag == "a";

    if (li) {
        c.out.shapes.push_back({ShapeCmd::Kind::Circle,
                                inner_left - 13, c.y + s.size * s.lh * 0.5f - 0.5f,
                                7, 7, 3.5f, 0xFF6B7280});
    }

    if (tag == "pre") {
        const float pad = 10;
        const float lh  = s.size * s.lh;
        float max_w = 0;
        const std::string text = el.text_content();
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl  = text.find('\n', start);
            size_t end = (nl == std::string::npos) ? text.size() : nl;
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            float lw = 0;
            for (const Word& w : shape_words(c.font, line, s.bold, s.mono)) {
                c.out.words.push_back({inner_left + pad + lw, c.y + pad * 0.5f,
                                       s.color, w});
                lw += w.width + s.size * 0.30f;
            }
            max_w = std::max(max_w, lw);
            c.y += lh;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        c.out.shapes.push_back({ShapeCmd::Kind::RRect, left, box_y,
                                std::min(avail, max_w + 2 * pad + s.pl + s.pr),
                                c.y - box_y + pad, 6,
                                s.has_block_bg ? s.block_bg : 0xFFF3F4F6});
        c.y += std::max(s.mb, 2.f);
        return;
    }

    if (clickable)
        c.out.hits.push_back({&el, inner_left, c.y, inner_avail, 0, false});

    layout_children(c, el, inner_left, inner_avail, s);
    c.flush();
    c.align = parent.align;

    if (clickable && !c.out.hits.empty() && c.out.hits.back().node == &el)
        c.out.hits.back().h = std::max(c.y - c.out.hits.back().y, s.size * 1.2f);

    if (s.has_block_bg)   // the box paints before every word, whatever the order
        c.out.shapes.push_back({ShapeCmd::Kind::RRect, left, box_y, avail,
                                c.y + s.pb - box_y, 4, s.block_bg});
    if (quote && quote_bar < c.out.shapes.size())
        c.out.shapes[quote_bar].h = std::max(10.f, c.y - c.out.shapes[quote_bar].y - 4);

    c.y += std::max(s.mb, 2.f);
    c.y += s.pb;
}

} // namespace

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w,
                    const std::string& page_css) {
    LayoutResult out;
    Ctx c{font, out};
    StyleResolver res(page_css);
    c.res = &res;

    c.measure = std::min(viewport_w - 2 * c.body_pad, 720.f);
    const float left0 = (viewport_w - c.measure) / 2.f;
    c.line_left  = left0;
    c.line_avail = c.measure;

    Style root_style;   // the inherited-defaults root: browser defaults
    root_style.size = 16; root_style.color = 0xFF232936; root_style.lh = 1.6f;

    layout_children(c, root, left0, c.measure, root_style);
    c.flush();

    out.content_height = c.y + c.body_pad + 30;
    return out;
}

const dom::Node* hit_test(const LayoutResult& l, float x, float y) {
    for (auto it = l.hits.rbegin(); it != l.hits.rend(); ++it) {
        if (x >= it->x && x <= it->x + it->w && y >= it->y && y <= it->y + it->h)
            return it->node;
    }
    return nullptr;
}

} // namespace luxium::render
