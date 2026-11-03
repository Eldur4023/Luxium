#include "shell/media_glue.hpp"

#include <media/own_media.hpp>

namespace luxium::shell {

MediaState OwnMediaApi::state(const dom::Node* n) {
    const media::own::State s = media::own::state(n);
    MediaState m;
    m.known = s.known;
    m.paused = s.paused; m.ended = s.ended; m.seeking = s.seeking; m.waiting = s.waiting; m.error = s.error;
    m.time = s.time; m.duration = s.duration;
    m.audio_count = s.audio_count; m.audio_cur = s.audio_cur;
    m.sub_count = s.sub_count; m.sub_cur = s.sub_cur;
    m.audio_label = s.audio_label; m.sub_label = s.sub_label; m.cue = s.cue;
    return m;
}
void OwnMediaApi::play(const dom::Node* n)  { media::own::play(n); }
void OwnMediaApi::pause(const dom::Node* n) { media::own::pause(n); }
void OwnMediaApi::seek(const dom::Node* n, double t) { media::own::seek(n, t); }
void OwnMediaApi::cycle_audio(const dom::Node* n)    { media::own::cycle_audio(n); }
void OwnMediaApi::cycle_subtitle(const dom::Node* n) { media::own::cycle_subtitle(n); }

} // namespace luxium::shell
