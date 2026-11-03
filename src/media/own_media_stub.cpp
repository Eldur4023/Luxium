// Stand-in when the FFmpeg headers are absent: elements stay empty boxes.
#include "own_media.hpp"

namespace luxium::media::own {

Scope::Scope(const void*) : prev_(nullptr) {}
Scope::~Scope() {}
Info attach(const void*, const Elem&) { return {}; }
void sweep() {}
void reset() {}
int tick() { return 0; }
std::shared_ptr<const render::ImageBitmap> frame(const void*) { return nullptr; }
State state(const void*) { return {}; }
void play(const void*) {}
void pause(const void*) {}
void toggle(const void*) {}
void seek(const void*, double) {}
void seek_fraction(const void*, double) {}

} // namespace luxium::media::own
