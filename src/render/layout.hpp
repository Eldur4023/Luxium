#pragma once
// Layout: DOM + a viewport width -> box list the shell can paint and hit-
// test.  The M-demo style model is per-tag defaults (h1..h3, p, li, button,
// ...): real CSS arrives in M3 (docs/ROADMAP.md); this is what "readable"
// needs and no more.
//
// Everything is positioned in DOCUMENT coordinates; the shell scrolls by
// offset and hit-tests against the element map.  A mutation (set_text) sets
// the page dirty and the shell re-runs layout wholesale -- a full relayout
// of a demo page is microseconds; caching boxes is M6 if profiling says so.

#include <dom/node.hpp>
#include <render/text.hpp>

#include <unordered_map>
#include <vector>

namespace luxium::render {

struct TextStyle {
    float size    = 16;
    bool  bold    = false;
    uint32_t color = 0xFF202020;
    // Block metrics (CSS-ish, per tag below).
    float margin_top = 0, margin_bottom = 0, margin_left = 0;
};

struct DrawCmd {
    enum class Kind { Word, Rect } kind = Kind::Word;
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t argb = 0;
    // Rects paint before words (a button's box under its own text), so the
    // shell walks the list twice: once for rects, once for words.
    Word word;
};

// One clickable/mutable DOM element with its painted rect (document coords).
struct HitBox {
    const dom::Node* node = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
};

struct LayoutResult {
    float            content_height = 0;
    std::vector<DrawCmd>  draw;
    std::vector<HitBox>   hits;      // elements that look clickable (button, [id], <a>)
};

// Style of a tag: size / bold / color / margins.  One place to extend when
// CSS support lands.
TextStyle style_of(const dom::Node& el);

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w);

// Deepest element whose hit box contains (x, y); null if none.
const dom::Node* hit_test(const LayoutResult& l, float x, float y);

} // namespace luxium::render
