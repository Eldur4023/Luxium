#pragma once
// Layout: DOM + a viewport width + stylesheets -> shapes + words the shell
// paints, plus hit boxes it click-tests.
//
// Styles come from the cascade in css/css.hpp: the browser's default look is
// its own stylesheet (kUAStylesheet in layout.cpp, parsed with the same
// parser as page CSS), then <style>/<link> rules of the page by
// specificity, then style="" attributes.  Inheritance per property, as CSS
// does: color/size/line-height/family/align/weight flow down; boxes do not.
//
// Everything is in DOCUMENT coordinates; the shell scrolls and hit-tests.
// A mutation re-runs layout wholesale: a demo page lays out in microseconds,
// caching boxes is an M6 question if profiling ever asks.

#include <css/css.hpp>
#include <dom/node.hpp>
#include <render/text.hpp>

#include <vector>

namespace luxium::render {

// Filled geometry, painted before every word (a button's box under its own
// text).  alpha < 255 blends over what is already there.
struct GradStop {
    uint32_t argb = 0;
    float    t = 0;     // 0..1 a lo largo del eje del degradado
};

struct ImageBitmap;   // decodificada en render/images (compartida, cacheada)

struct ShapeCmd {
    enum class Kind { Rect, RRect, Circle, Image } kind = Kind::Rect;
    float    x = 0, y = 0, w = 0, h = 0, r = 0;
    uint32_t argb = 0;
    // degradado: si stops no esta vacio, argb se ignora y el color se
    // interpola por pixel (lineal segun gdx/gdy/glen, o radial)
    bool     grad = false;
    bool     grad_radial = false;
    std::vector<GradStop> stops;
    float    gdx = 0, gdy = 0, glen = 1;          // lineal: direccion + largo
    float    gcx = 0, gcy = 0, grx = 1, gry = 1;  // radial: centro y radios (doc)
    // imagen: rect destino (x,y,w,h) y rect fuente en pixeles de bitmap
    std::shared_ptr<const ImageBitmap> img;
    float    sx = 0, sy = 0, sw = 0, sh = 0;
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

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w,
                    const std::string& page_css,
                    const std::string& base_dir = "");

// Deepest element whose hit box contains (x, y); null if none.
const dom::Node* hit_test(const LayoutResult& l, float x, float y);

} // namespace luxium::render
