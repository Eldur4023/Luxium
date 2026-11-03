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

#include <functional>
#include <string>
#include <vector>

namespace luxium::render {

// One clickable/mutable DOM element with its painted rect (document coords).
struct HitBox {
    const dom::Node* node = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
    bool  button = false;        // hover overlay + hand cursor apply to these
    int   cursor = 0;            // CSS cursor of the box: 0 auto, 1 pointer, 2 text, 3 default, 4 not-allowed, 5 grab, 6 grabbing
    // form controls (ctl >= 0: Control::Type): where the text starts and the font it is drawn with
    int   ctl = -1;
    float tx = 0, ty = 0, th = 0, px = 0;   // text origin x (before scrolling), content top, line height, font size
    int   face = 0;
    bool  password = false;
    // visible part (clipping ancestors, document coords); hits outside it are not hits
    float clip_x0 = -1e9f, clip_y0 = -1e9f, clip_x1 = 1e9f, clip_y1 = 1e9f;
    // scroll containers: how far they scroll and where the vertical scrollbar track is
    float max_sx = 0, max_sy = 0, sb_x = 0, sb_y = 0, sb_w = 0, sb_h = 0;
    int   sticky = -1;                  // index into LayoutResult::sticky when the box lives in a sticky group
    float base_y = 0, base_clip_y0 = 0, base_clip_y1 = 0;   // y of the unshifted layout (the shell moves y with the group)
    bool  in_clip(float px, float py) const { return px >= clip_x0 && px < clip_x1 && py >= clip_y0 && py < clip_y1; }
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

// A <video>/<audio> painted by this layout.  `item` is its picture item in the
// list (the shell swaps the current frame into it); the controls are drawn
// by the shell on top (layout/media_ui.cpp).
struct MediaBox {
    const dom::Node* node = nullptr;
    float x = 0, y = 0, w = 0, h = 0;           // content box, document coords
    float fx0 = 0, fy0 = 0, fx1 = 1, fy1 = 1;   // visible part of the picture, as fractions of it
    size_t item = 0;
    bool fixed = false, controls = false, audio = false;
    float radius = 0;
};

// A `position: sticky` box painted apart: the shell translates it each frame so that it sticks to the
// viewport edge (top inset) while its containing block is still in view.
struct StickyGroup {
    paint::DisplayList list;
    float top = 0, bottom = 0;           // inset from the viewport top/bottom edge
    bool  has_top = false, has_bottom = false;
    float orig_y = 0, h = 0;             // border box, document coords, unshifted
    float cb_top = 0, cb_bottom = 0;     // containing block content box (the box never leaves it)
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
    std::vector<MediaBox> media;
    std::vector<StickyGroup> sticky;   // painted after `list`, before `fixed`, shifted by the shell
};

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w,
                    const std::vector<style::Rule>& author_rules,
                    const std::string& base_dir = "",
                    float vp_y = 38.f, float vp_h = 762.f,
                    std::function<void(const dom::Node&, style::ComputedStyle&)> style_hook = nullptr);

// Own-engine media (layout/media_ui.cpp): swap the current pictures into the
// list; the controls as an overlay list to draw on top (main or fixed); a click
// at document (x, y) on a control bar acts on it and returns true.
void media_patch(LayoutResult& l);
paint::DisplayList media_overlay(const LayoutResult& l, bool fixed, const Font& font);
bool media_click(const MediaBox& m, float x, float y);

// Deepest element whose hit box contains (x, y); null if none.
const dom::Node* hit_test(const LayoutResult& l, float x, float y);

} // namespace luxium::render
