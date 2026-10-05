#pragma once
// Layout: DOM + a viewport + stylesheets -> display list + hit boxes.
//
// The engine lives in src/layout (box tree, block/inline/flex/grid/table
// formatting, painting order); styles come from src/style (selectors,
// cascade, typed ComputedStyle).  This header is the shell's whole view of it.
//
// Everything is in DOCUMENT coordinates; the shell scrolls and hit-tests.  A
// mutation re-runs layout wholesale.

#include <dom/node.hpp>
#include <paint/display_list.hpp>
#include <render/text.hpp>
#include <style/stylesheet.hpp>

#include <string>
#include <vector>

namespace luxium::render {

// One clickable/mutable DOM element with its painted rect (document coords).
struct HitBox {
    const dom::Node* node = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
    bool  button = false;        // hover overlay + hand cursor apply to these
};

// Border-box rect of an element carrying data-t (what the css-tests probe compares).
struct BoxRect {
    std::string name;
    float x = 0, y = 0, w = 0, h = 0;
};

// One word of laid-out text (text selection, copy).
struct TextWord {
    std::string text;
    float x = 0, y = 0, w = 0, h = 0;
};

struct LayoutResult {
    float content_height = 0;
    uint32_t canvas_bg = 0xFFFFFFFF;   // viewport background (html, or body when html has none)
    paint::DisplayList list;           // scrolls with the document
    paint::DisplayList fixed;          // position: fixed, painted in viewport coordinates
    std::vector<HitBox> hits;
    std::vector<HitBox> areas;         // every element box, tree order (:hover targets)
    std::vector<BoxRect> boxes;
    std::vector<TextWord> words;
};

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w,
                    const std::vector<style::Rule>& author_rules,
                    const std::string& base_dir = "",
                    float vp_y = 38.f, float vp_h = 762.f);

// Deepest element whose hit box contains (x, y); null if none.
const dom::Node* hit_test(const LayoutResult& l, float x, float y);

} // namespace luxium::render
