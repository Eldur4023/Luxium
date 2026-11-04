#pragma once
// The display list: what layout hands to the rasterizer, already in paint
// order (CSS 2.1 Appendix E, stacking contexts flattened).  Coordinates are
// document pixels; rect edges are snapped to whole pixels by the painter, as
// Blink does, so backgrounds and borders never blur.

#include <render/text.hpp>
#include <style/computed.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace luxium::render { struct ImageBitmap; }

namespace luxium::paint {

struct Radii {
    float tl = 0, tr = 0, br = 0, bl = 0;           // horizontal radii
    float tlv = -1, trv = -1, brv = -1, blv = -1;   // vertical radii; < 0: same as the horizontal one (circular)
    bool any() const { return tl > 0 || tr > 0 || br > 0 || bl > 0; }
    float vtl() const { return tlv < 0 ? tl : tlv; }
    float vtr() const { return trv < 0 ? tr : trv; }
    float vbr() const { return brv < 0 ? br : brv; }
    float vbl() const { return blv < 0 ? bl : blv; }
    bool elliptical() const { return tlv >= 0 || trv >= 0 || brv >= 0 || blv >= 0; }
    float max_r() const { return std::max({tl, tr, br, bl, vtl(), vtr(), vbr(), vbl()}); }
    // the radii of the inner edge of a border / padding box: each side's width taken off the matching axis
    Radii inset(float l, float t, float r, float b) const {
        Radii o;
        o.tl = std::max(0.f, tl - l); o.tlv = std::max(0.f, vtl() - t);
        o.tr = std::max(0.f, tr - r); o.trv = std::max(0.f, vtr() - t);
        o.br = std::max(0.f, br - r); o.brv = std::max(0.f, vbr() - b);
        o.bl = std::max(0.f, bl - l); o.blv = std::max(0.f, vbl() - b);
        return o;
    }
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
        ShiftPush, ShiftPop,   // translate everything in between by (x, y): transform: translate()
        TransformPush, TransformPop,   // general 2D affine `m` (doc -> doc) over everything in between; x,y,w,h = its source bounds
        Poly,          // filled polygon `pts` (x,y pairs, doc coords), anti-aliased; color = fill (clip-path mask)
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
    bool radial = false, repeating = false, conic = false;   // conic: angle `from` (radians) in gdx, centre in gcx/gcy
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

    // Layer (group opacity, and the effects that need the group: filter, backdrop-filter, mask).
    // cx..ch + radii = the element's border box (backdrop region; mask area).
    float alpha = 1.f;
    std::vector<style::FilterFn> filters, backdrop;
    std::shared_ptr<const std::vector<Item>> mask;   // painted in doc coords; its alpha multiplies the group

    // TransformPush: x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5
    float m[6] = {1, 0, 0, 1, 0, 0};

    // Poly
    std::vector<float> pts;
    bool evenodd = false;

    // Layer: clip-path shape, painted like `mask` (a Poly item); its alpha multiplies the group too
    std::shared_ptr<const std::vector<Item>> clip;
};

using DisplayList = std::vector<Item>;

} // namespace luxium::paint
