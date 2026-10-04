#pragma once
// Layout: DOM + a viewport width -> shapes + words the shell paints, plus
// hit boxes it click-tests.  The style model is a per-tag design table (this
// file's kStyles): real CSS arrives in M3 (docs/ROADMAP.md); what exists now
// is a coherent default look -- type scale, line heights, a measure capped
// for reading, rounded buttons, list indents, code pills.
//
// Everything is in DOCUMENT coordinates; the shell scrolls and hit-tests.
// A mutation re-runs layout wholesale: a demo page lays out in microseconds,
// caching boxes is an M6 question if profiling ever asks.

#include <dom/node.hpp>
#include <render/text.hpp>

#include <vector>

namespace luxium::render {

struct TextStyle {
    float    size    = 15.5f;
    bool     bold    = false;
    bool     mono    = false;
    uint32_t color   = 0xFF232936;          // ink
    float    lh      = 1.55f;               // line height, in ems
    float    mt = 0, mb = 0, ml = 0;        // block margins
    uint32_t bg      = 0;                   // inline pill (code); 0 = none
};

// Filled geometry, painted before every word (a button's box under its own
// text).  alpha < 255 blends over what is already there.
struct ShapeCmd {
    enum class Kind { Rect, RRect, Circle } kind = Kind::Rect;
    float    x = 0, y = 0, w = 0, h = 0, r = 0;
    uint32_t argb = 0;
};

struct WordCmd {
    float x = 0, y = 0;
    uint32_t color = 0;
    Word  word;
};

// One clickable/mutable DOM element with its painted rect (document coords).
struct HitBox {
    const dom::Node* node = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
    bool  button = false;        // hover overlay + hand cursor apply to these
};

struct LayoutResult {
    float content_height = 0;
    std::vector<ShapeCmd> shapes;
    std::vector<WordCmd>  words;
    std::vector<HitBox>   hits;
};

// Style of a tag: the design table lives in layout.cpp, one place to extend
// when CSS support lands.
TextStyle style_of(const dom::Node& el);

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w);

// Deepest element whose hit box contains (x, y); null if none.
const dom::Node* hit_test(const LayoutResult& l, float x, float y);

} // namespace luxium::render
