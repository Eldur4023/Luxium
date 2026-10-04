#include "render/layout.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace luxium::render {

namespace {

// ── The design table ────────────────────────────────────────────────────────
// One row per tag: size, bold, mono, ink color, line height, margins.
// Palette: ink #232936, muted #6B7280, accent #2F6FED, paper #FAFAF7 (the
// shell owns the paper; everything text-colored lives here).
constexpr uint32_t kInk     = 0xFF232936;
constexpr uint32_t kMuted   = 0xFF646B76;
constexpr uint32_t kHeading = 0xFF141926;
constexpr uint32_t kAccent  = 0xFF2F6FED;
constexpr uint32_t kCodeInk = 0xFF933B32;

struct TagStyle {
    const char* tag;
    TextStyle   s;
};

TextStyle row(float size, bool bold, uint32_t color, float lh, float mt, float mb,
              float ml = 0, bool mono = false, uint32_t bg = 0) {
    return TextStyle{size, bold, mono, color, lh, mt, mb, ml, bg};
}

const std::array<TagStyle, 15> kStyles{{
    {"body",       row(15.5f, false, kInk,     1.6f,  6,  6)},
    {"h1",         row(31,    true,  kHeading, 1.25f, 6, 16)},
    {"h2",         row(22,    true,  kHeading, 1.3f,  26, 10)},
    {"h3",         row(18,    true,  kHeading, 1.3f,  20, 8)},
    {"p",          row(15.5f, false, kInk,     1.6f,  0,  12)},
    {"li",         row(15.5f, false, kInk,     1.55f, 0,  7,  26)},
    {"b",          row(15.5f, true,  kInk,     1.6f,  0,  0)},
    {"strong",     row(15.5f, true,  kInk,     1.6f,  0,  0)},
    {"i",          row(15.5f, false, kInk,     1.6f,  0,  0)},
    {"em",         row(15.5f, false, kInk,     1.6f,  0,  0)},
    {"a",          row(15.5f, false, kAccent,  1.6f,  0,  0)},
    {"code",       row(13.5f, false, kCodeInk, 1.6f,  0,  0,  0, true, 0xFFEDEFF3)},
    {"blockquote", row(15,    false, kMuted,   1.6f,  10, 10, 18)},
    {"button",     row(14.5f, true,  0xFFFFFFFF, 1.2f, 0,  0)},
    {"pre",        row(13,    false, 0xFF37424E, 1.5f, 10, 12, 0, true)},
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

// ── Layout context ──────────────────────────────────────────────────────────

struct Ctx {
    Font&         font;
    LayoutResult& out;

    float y      = 0;
    float line_w = 0;
    float line_h = 0;

    float body_pad  = 26;    // page padding
    float measure   = 720;   // reading column width (capped, then centered)
    float left0     = 0;     // where the column starts (centered)

    void flush() {
        y += line_h;
        line_w = 0;
        line_h = 0;
    }
};

void emit_word(Ctx& c, const Word& w, float left, float avail, const TextStyle& ts,
               std::vector<WordCmd>* sink = nullptr) {
    const float space = ts.size * 0.30f;
    if (c.line_w > 0 && c.line_w + w.width > avail) c.flush();
    if (c.line_w == 0) c.line_h = ts.size * ts.lh;
    WordCmd wc{left + c.line_w, c.y, ts.color, w};
    if (sink) sink->push_back(std::move(wc));
    else      c.out.words.push_back(std::move(wc));
    if (ts.bg) {
        // inline code pill: a shape, so it paints under every word no matter
        // when it was emitted
        c.out.shapes.push_back({ShapeCmd::Kind::RRect,
                                left + c.line_w - 3, c.y + 1.5f,
                                w.width + 6, ts.size * ts.lh - 3, 3, ts.bg});
    }
    c.line_w += w.width + space;
    c.line_h = std::max(c.line_h, ts.size * ts.lh);
}

void inline_text(Ctx& c, std::string_view text, const TextStyle& ts, float left,
                 float avail, std::vector<WordCmd>* sink = nullptr) {
    if (text.empty()) return;
    c.font.set_size(ts.size);
    for (const Word& w : shape_words(c.font, text, ts.bold, ts.mono))
        emit_word(c, w, left, avail, ts, sink);
}

// Inline subtree (b/i/code/a): flows with the open line, each element takes
// its own style for its own text.
void layout_inline_children(Ctx& c, const dom::Node& el, float left, float avail,
                            std::vector<WordCmd>* sink = nullptr) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            inline_text(c, child->text, style_of(el), left, avail, sink);
        } else if (child->is_element()) {
            layout_inline_children(c, *child, left, avail, sink);
        }
    }
}

// All text of a subtree, flattened (buttons are laid out as one atom).
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
    if (el.type != dom::NodeType::Element) return;
    if (in_list(tag, kSkipped, std::size(kSkipped))) return;

    const TextStyle ts = style_of(el);
    c.font.set_size(ts.size);

    if (tag == "br") {
        c.flush();
        c.y += ts.size * 0.35f;
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
    // It flows into the open line like a word (so two buttons sit side by
    // side), wraps as a whole, and paints its own rounded box under its text.
    if (tag == "button") {
        const float padx = 16, pady = 9;
        c.font.set_size(ts.size);
        auto words = shape_words(c.font, flatten_text(el), ts.bold, ts.mono);
        float content_w = 0;
        for (const auto& w : words) content_w += w.width + ts.size * 0.30f;
        content_w -= ts.size * 0.30f;   // no trailing space
        const float bw = content_w + 2 * padx;
        const float bh = ts.size * ts.lh + 2 * pady;

        if (c.line_w > 0 && c.line_w + bw + 8 > avail) c.flush();
        if (c.line_w == 0) c.line_h = bh;
        const float bx = left + c.line_w;
        const float by = c.y;

        c.out.shapes.push_back({ShapeCmd::Kind::RRect, bx, by, bw, bh, 7, kAccent});
        float wx = bx + padx;
        for (const auto& w : words) {
            c.out.words.push_back({wx, by + pady, ts.color, w});
            wx += w.width + ts.size * 0.30f;
        }

        c.out.hits.push_back({&el, bx, by, bw, bh, true});

        c.line_w += bw + 10;               // 10px gap: the next button breathes
        c.line_h = std::max(c.line_h, bh);
        return;
    }

    if (!block) {   // inline element
        layout_inline_children(c, el, left, avail);
        return;
    }

    // ── plain block ──────────────────────────────────────────────────────
    c.flush();
    c.y += ts.mt;

    const float inner_left  = left + ts.ml;
    const float inner_avail = avail - ts.ml;

    const bool li        = tag == "li";
    const bool quote     = tag == "blockquote";
    const bool pre       = tag == "pre";
    const bool clickable = el.has_attr("id") || tag == "a";

    if (li) {   // bullet, vertically centered on the first line
        c.out.shapes.push_back({ShapeCmd::Kind::Circle,
                                inner_left - 15, c.y + ts.size * ts.lh * 0.5f - 2.5f,
                                5, 5, 2.5f, 0xFF9AA3AE});
    }
    if (quote) {   // the accent bar, grown to the content height below
        const size_t bar = c.out.shapes.size();
        c.out.shapes.push_back({ShapeCmd::Kind::RRect,
                                inner_left - 14, c.y + 2, 3, 10, 1.5f, 0xFFC9CDD4});
        const float y0 = c.y;
        layout_children(c, el, inner_left, inner_avail);
        c.flush();
        c.out.shapes[bar].h = std::max(10.f, c.y - y0 - 4);
        c.y += std::max(ts.mb, 2.f);
        return;
    }

    if (pre) {   // white-space: pre -- mono, one source line per row, pill bg
        const float pad = 10;
        const float y0  = c.y;
        const float lh  = ts.size * ts.lh;
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
            for (const Word& w : shape_words(c.font, line, false, true)) {
                c.out.words.push_back({inner_left + pad + lw, c.y + pad * 0.5f,
                                       ts.color, w});
                lw += w.width + ts.size * 0.30f;
            }
            max_w = std::max(max_w, lw);
            c.y += lh;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        c.out.shapes.push_back({ShapeCmd::Kind::RRect, inner_left, y0,
                                std::min(inner_avail, max_w + 2 * pad),
                                c.y - y0 + pad, 6, 0xFFF3F4F6});
        c.y += std::max(ts.mb, 2.f);
        return;
    }

    if (clickable) {
        c.out.hits.push_back({&el, inner_left, c.y, inner_avail, 0, false});
    }

    layout_children(c, el, inner_left, inner_avail);
    c.flush();

    if (clickable && !c.out.hits.empty() && c.out.hits.back().node == &el)
        c.out.hits.back().h = std::max(c.y - c.out.hits.back().y, ts.size * 1.2f);

    c.y += std::max(ts.mb, 2.f);
}

} // namespace

TextStyle style_of(const dom::Node& el) {
    for (const auto& t : kStyles) {
        if (el.name == t.tag) return t.s;
    }
    return {};   // unknown tags: body-ish defaults
}

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w) {
    LayoutResult out;
    Ctx c{font, out};

    c.measure = std::min(viewport_w - 2 * c.body_pad, 720.f);
    c.left0   = (viewport_w - c.measure) / 2.f;

    layout_children(c, root, c.left0, c.measure);
    c.flush();

    out.content_height = c.y + c.body_pad + 30;   // breathing room at the end
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
