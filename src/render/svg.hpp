#pragma once
// A compact SVG rasteriser: paths (all commands, arcs included), basic shapes, fill/stroke with caps, joins and
// dashes, opacity, <g>/<use>/transforms, linear/radial gradients, viewBox + preserveAspectRatio, a tiny <style>.
// Sources: standalone SVG text (file, data: URL) and inline <svg> elements of the DOM.  Output is straight-alpha RGBA.

#include <dom/node.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace luxium::render {
struct ImageBitmap;
}

namespace luxium::render::svg {

struct Doc;

// Parses standalone SVG text; null when it is not an SVG document.  `current` is what `currentColor` means.
std::shared_ptr<const Doc> parse(const std::string& xml, uint32_t current = 0xFF000000);
// An inline <svg> element (its subtree is read as is).
std::shared_ptr<const Doc> from_dom(const dom::Node& svg, uint32_t current);

// Intrinsic size: width/height attributes, else the viewBox, else 300x150.
void intrinsic_size(const Doc& d, float& w, float& h);
// Rasterises at w x h pixels (cached by content, size and colour).
std::shared_ptr<const ImageBitmap> raster(const std::shared_ptr<const Doc>& d, int w, int h);

} // namespace luxium::render::svg
