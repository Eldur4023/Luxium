#pragma once
// Still images the built-in decoders do not cover (GIF, BMP, ICO, TIFF, AVIF...), through FFmpeg: the first
// frame of the file, as RGB (opaque) or straight-alpha RGBA.  False if FFmpeg is not built in or the bytes
// are not an image it knows.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace luxium::media {

bool decode_still_image(const uint8_t* data, size_t size, int& w, int& h,
                        std::vector<uint8_t>& rgb, std::vector<uint8_t>& rgba);

// Every frame of an animated picture (GIF): full-canvas straight-alpha RGBA frames and how long each shows, in ms.
// False unless there are at least two frames.
bool decode_animation(const uint8_t* data, size_t size, int& w, int& h,
                      std::vector<std::vector<uint8_t>>& frames, std::vector<int>& delays_ms);

} // namespace luxium::media
