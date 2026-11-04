#pragma once
// <iframe> without a browsing context: the framed document (srcdoc, or src when it is HTML/SVG) laid out at the
// frame's size and drawn once into a bitmap -- what the frame shows, not a live page (no scripts, no clicks).
#include <dom/node.hpp>
#include <render/images.hpp>
#include <render/text.hpp>

#include <memory>
#include <string>

namespace luxium::layout {

// Null while the document is still on its way (the shell relayouts when it lands) or cannot be shown.
std::shared_ptr<const render::ImageBitmap> render_frame(render::Font& font, const dom::Node& iframe,
                                                        const std::string& base_dir, int w, int h);

} // namespace luxium::layout
