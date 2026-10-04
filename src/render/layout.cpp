#include "render/layout.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace luxium::render {

namespace {

struct TagStyle {
    float    size;
    bool     bold;
    uint32_t color;
    float    mt, mb, ml;   // margins: top, bottom, left
};

// The per-tag defaults.  CSS proper lands in M3; this is what "readable"
// needs and no more.
constexpr std::array<std::pair<std::string_view, TagStyle>, 12> kStyles{{
    {"body",       {16, false, 0xFF202020, 6,  6,  0}},
    {"h1",         {30, true,  0xFF101418, 14, 10, 0}},
    {"h2",         {24, true,  0xFF101418, 14, 8,  0}},
    {"h3",         {20, true,  0xFF101418, 12, 6,  0}},
    {"p",          {16, false, 0xFF202020, 6,  6,  0}},
    {"li",         {16, false, 0xFF202020, 3,  3,  22}},
    {"button",     {15, true,  0xFFF8F8F8, 6,  6,  0}},
    {"b",          {16, true,  0xFF202020, 0,  0,  0}},
    {"strong",     {16, true,  0xFF202020, 0,  0,  0}},
    {"i",          {16, false, 0xFF202020, 0,  0,  0}},
    {"em",         {16, false, 0xFF202020, 0,  0,  0}},
    {"blockquote", {15, false, 0xFF404850, 8,  8,  18}},
}};

constexpr std::string_view kSkipped[] = {
    "head", "title", "script", "style", "meta", "link", "template",
};
constexpr std::string_view kBlocks[] = {
    "body", "html", "h1", "h2", "h3", "p", "ul", "ol", "li", "pre",
    "blockquote", "button", "div", "hr", "br",
};

bool in_list(std::string_view name, const std::string_view* list, size_t n) {
    return std::find(list, list + n, name) != list + n;
}

struct Ctx {
    Font&         font;
    LayoutResult& out;

    float y        = 0;
    float line_w   = 0;
    float line_h   = 0;
    float body_pad = 14;

    void flush() {
        // Words were already emitted; the line cursor just moves.
        y += line_h;
        line_w = 0;
        line_h = 0;
    }
};

void emit_word(Ctx& c, const Word& w, float left, float avail, const TextStyle& ts) {
    const float space = ts.size * 0.28f;
    if (c.line_w > 0 && c.line_w + w.width > avail) c.flush();
    if (c.line_w == 0) c.line_h = c.font.line_height();
    c.out.draw.push_back(DrawCmd{DrawCmd::Kind::Word, left + c.line_w, c.y,
                                 w.width, c.line_h, ts.color, w});
    c.line_w += w.width + space;
    c.line_h = std::max(c.line_h, c.font.line_height());
}

// Inline text: words flow into the open line, wrapping at `avail`.
void inline_text(Ctx& c, std::string_view text, const TextStyle& ts, float left,
                 float avail) {
    if (text.empty()) return;
    c.font.set_size(ts.size);
    for (const Word& w : shape_words(c.font, text, ts.bold))
        emit_word(c, w, left, avail, ts);
}

// Inline subtree: b/i/code flow with the open line; each element's own text
// takes its own style.
void layout_inline_children(Ctx& c, const dom::Node& el, float left, float avail) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            inline_text(c, child->text, style_of(el), left, avail);
        } else if (child->is_element()) {
            layout_inline_children(c, *child, left, avail);
        }
    }
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail);

void layout_children(Ctx& c, const dom::Node& el, float left, float avail) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            inline_text(c, child->text, style_of(el), left, avail);
        } else if (child->is_element()) {
            layout_block(c, *child, left, avail);
        }
    }
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail) {
    const std::string& tag = el.name;
    if (in_list(tag, kSkipped, std::size(kSkipped))) return;
    if (el.type != dom::NodeType::Element) return;

    const TextStyle ts = style_of(el);
    c.font.set_size(ts.size);

    if (tag == "br") {
        c.flush();
        c.y += c.font.line_height() * 0.4f;
        return;
    }
    if (tag == "hr") {
        c.flush();
        c.y += 8;
        c.out.draw.push_back(DrawCmd{DrawCmd::Kind::Rect, left, c.y, avail, 2,
                                     0xFF98A0A8, {}});
        c.y += 10;
        return;
    }

    const bool block = in_list(tag, kBlocks, std::size(kBlocks));
    if (!block) {   // inline element: flow with the open line
        layout_inline_children(c, el, left, avail);
        return;
    }

    c.flush();
    c.y += ts.margin_top;

    const float inner_left = left + ts.margin_left;
    const float inner_avail = avail - ts.margin_left;

    const bool button = tag == "button";
    const bool li     = tag == "li";
    const bool clickable = button || el.has_attr("id") || tag == "a";
    if (clickable) {
        c.out.hits.push_back({&el, inner_left, c.y, 0, 0});
    }

    if (li) {   // bullet on the first line
        c.font.set_size(ts.size);
        Word bullet;
        bullet.text  = "\xE2\x80\xA2";   // U+2022
        bullet.bold  = false;
        bullet.shaped = c.font.shape(bullet.text, false);
        bullet.width = bullet.shaped.width;
        emit_word(c, bullet, inner_left - 14, inner_avail + 14, ts);
    }

    float content_y0 = c.y;
    layout_children(c, el, inner_left, inner_avail);
    c.flush();
    const float content_h = c.y - content_y0;

    if (button) {   // the rect paints before any word (rect pass first)
        float w = 0;
        for (const auto& d : c.out.draw)
            if (d.kind == DrawCmd::Kind::Word && d.y >= content_y0 - 0.5f &&
                d.y < c.y + 0.5f)
                w = std::max(w, d.x + d.w - inner_left);
        w += ts.size * 1.6f;
        const float bh = std::max(content_h, ts.size * 1.5f) + ts.size * 0.7f;
        c.out.draw.insert(
            c.out.draw.begin(),
            DrawCmd{DrawCmd::Kind::Rect, inner_left, content_y0 - ts.size * 0.35f,
                    w, bh, 0xFF3A6EA5, {}});
        // nudge the button's words inside the box: shift the words we just
        // laid out down a touch so the text centers in the rect.
        for (auto& d : c.out.draw)
            if (d.kind == DrawCmd::Kind::Word && d.y >= content_y0 - 0.5f &&
                d.y < c.y + 0.5f && d.x >= inner_left - 0.5f &&
                d.x <= inner_left + w)
                d.y += ts.size * 0.18f;
        if (clickable && !c.out.hits.empty() && c.out.hits.back().node == &el) {
            c.out.hits.back().w = w;
            c.out.hits.back().h = bh;
        }
    } else if (clickable && !c.out.hits.empty() && c.out.hits.back().node == &el) {
        c.out.hits.back().w = inner_avail;
        c.out.hits.back().h = std::max(content_h, ts.size * 1.2f);
    }

    c.y += std::max(ts.margin_bottom, 2.f);
}

} // namespace

TextStyle style_of(const dom::Node& el) {
    TextStyle t;
    for (const auto& [name, s] : kStyles) {
        if (el.name != name) continue;
        t.size = s.size;
        t.bold = s.bold;
        t.color = s.color;
        t.margin_top = s.mt;
        t.margin_bottom = s.mb;
        t.margin_left = s.ml;
        break;
    }
    // The one inline style the language demo drives: style="color: #rrggbb",
    // set with element.set_attr("style", ...) -- the M4 shape of CSS to come.
    const std::string style = el.attr_or("style");
    if (style.rfind("color:", 0) == 0) {
        std::string v = style.substr(6);
        while (!v.empty() && (v.front() == ' ' || v.front() == '#')) v.erase(v.begin());
        if (v.size() == 6) {
            unsigned long rgb = 0;
            try { rgb = std::stoul(v, nullptr, 16); } catch (...) {}
            t.color = 0xFF000000u | static_cast<uint32_t>(rgb);
        }
    }
    return t;
}

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w) {
    LayoutResult out;
    Ctx c{font, out};

    // body: pad both sides, walk it as a block.
    const float left = c.body_pad;
    const float avail = viewport_w - 2 * c.body_pad;
    layout_children(c, root, left, avail);
    c.flush();

    out.content_height = c.y + c.body_pad;
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
