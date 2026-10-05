#pragma once
// The display list: what layout hands to the rasterizer, already in paint
// order (CSS 2.1 Appendix E, stacking contexts flattened).  Coordinates are
// document pixels; rect edges are snapped to whole pixels by the painter, as
// Blink does, so backgrounds and borders never blur.

#include <render/text.hpp>
#include <style/computed.hpp>

#include <memory>
#include <vector>

namespace luxium::render { struct ImageBitmap; }

namespace luxium::paint {

struct Radii {
    float tl = 0, tr = 0, br = 0, bl = 0;
    bool any() const { return tl > 0 || tr > 0 || br > 0 || bl > 0; }
};

struct Stop { uint32_t argb = 0; float t = 0; };   // t along the gradient line (0..1)

struct Item {
    enum class Kind {
        Rect,        // fill (rounded when radii)
        Border,      // per-side width/color/style ring
        Gradient,    // fill with linear/radial gradient
        Image,       // bitmap tile (source rect -> dest rect)
        Shadow,      // box-shadow (outer or inset)
        Text,        // shaped run
        Line,        // text decoration / list marker bar (solid rect, AA off)
        ClipPush, ClipPop,
        LayerPush, LayerPop,   // group opacity
    } kind = Kind::Rect;

    float x = 0, y = 0, w = 0, h = 0;     // doc coords (already snapped for boxes)
    Radii radii;
    uint32_t color = 0;

    // Border
    float bw[4] = {0, 0, 0, 0};            // top right bottom left
    uint32_t bc[4] = {0, 0, 0, 0};
    style::BorderStyle bs[4] = {style::BorderStyle::None, style::BorderStyle::None,
                                style::BorderStyle::None, style::BorderStyle::None};

    // Gradient: linear (dx,dy unit vector, len) or radial (center + radii)
    bool radial = false, repeating = false;
    float gdx = 0, gdy = 1, glen = 1;
    float gcx = 0, gcy = 0, grx = 1, gry = 1;
    std::vector<Stop> stops;

    // Image
    std::shared_ptr<const render::ImageBitmap> img;
    float sx = 0, sy = 0, sw = 0, sh = 0;

    // Shadow
    float blur = 0, spread = 0;
    bool  inset = false;
    float cx = 0, cy = 0, cw = 0, ch = 0;   // Shadow: the box that casts it (x,y,w,h = shadow rect)

    // Text
    std::shared_ptr<const render::ShapedRun> run;
    render::FaceKey face;
    float px = 16;
    float baseline = 0;

    // Layer
    float alpha = 1.f;
};

using DisplayList = std::vector<Item>;

} // namespace luxium::paint
