#include "image_decode.hpp"

namespace luxium::media {
bool decode_still_image(const uint8_t*, size_t, int&, int&, std::vector<uint8_t>&, std::vector<uint8_t>&) { return false; }
bool decode_animation(const uint8_t*, size_t, int&, int&, std::vector<std::vector<uint8_t>>&, std::vector<int>&) { return false; }
}
