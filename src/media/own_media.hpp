#pragma once
// <video>/<audio> for the own engine (non-JS pages).  A registry of
// media::Players keyed by the DOM element: layout asks `attach` for the
// intrinsic size, the shell calls `tick` every frame and paints `frame`, and
// clicks/the script hook drive play/pause/seek.  See docs/MEDIA-OWN-ENGINE.md.
//
// The key is an opaque pointer (the dom::Node); nothing here dereferences it.

#include <render/images.hpp>

#include <memory>
#include <string>

namespace luxium::media::own {

struct Elem {                      // the element's attributes, read at layout time
    std::string src;               // resolved: http(s) URL or file path
    bool local = false;            // file path (the page itself is local)
    bool autoplay = false, loop = false, muted = false;
};

struct Info {                      // what layout needs for the replaced box
    float w = 0, h = 0;            // display size of the video, 0 = unknown yet
};

struct State {
    bool   known = false;          // a player exists for the key
    bool   paused = true, ended = false, seeking = false, waiting = false, error = false;
    double time = 0, duration = 0; // duration 0 until known
    double buffered_end = 0;
    int    audio_count = 0, audio_cur = 0;   // audio streams in the file, 1-based position of the one playing
    int    sub_count = 0, sub_cur = 0;       // text subtitle streams, 1-based position shown (0 = off)
    std::string audio_label, sub_label;      // "ac3 eng 5.1", "eng"
    std::string cue;                         // text of the subtitle cue active now ("" = none), '\n' between lines
};

// Creates or updates the player for `key` (src changes reopen it).  Called by
// layout on every build; marks the key as live for `sweep`.
Info attach(const void* key, const Elem& e);
// Closes the players whose element did not show up in the last layout.
void sweep();
// Navigation: closes everything.
void reset();

// Per frame.  Returns bit 1 = a new picture or a state change (repaint),
// bit 2 = the video size became known/changed (relayout).
int tick();
// Latest picture as RGB (null before the first frame).
std::shared_ptr<const render::ImageBitmap> frame(const void* key);
State state(const void* key);

void play(const void* key);
void pause(const void* key);
void toggle(const void* key);      // click: pause <-> play (restarts when ended)
void seek(const void* key, double seconds);
void seek_fraction(const void* key, double f);   // 0..1 of the duration
void cycle_audio(const void* key);               // next audio stream of the file (re-sync at the same time)
void cycle_subtitle(const void* key);           // off -> track 1 -> ... -> off

} // namespace luxium::media::own
