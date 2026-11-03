#pragma once
// Luxium's media engine: one Player per <video>/<audio> element.  libavformat
// demuxes (progressive files, MKV, WebM, MPEG-TS, HLS over http(s)), our own
// threads decode, an SDL audio callback is the master clock, and the video
// side hands out the NV12 frame that is due.  The owner (the CEF bridge, the
// own engine's page, the self-test) only sees this header: commands in,
// state getters, an event queue, and `frame()`.  See docs/MEDIA.md.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luxium::media {

// Which codecs/containers this build can actually play (from FFmpeg's decoders).
// mime may carry a codecs="..." parameter.  Returns "", "maybe" or "probably".
std::string can_play(const std::string& mime_with_params);
bool        built();    // false in the stub (no FFmpeg headers at build time)

// One decoded picture, NV12 8-bit: the Y plane (w*h bytes) followed by the
// interleaved UV plane (((w+1)/2*2) * ((h+1)/2) bytes), both tightly packed.
struct Frame {
    int      w = 0, h = 0;
    int      display_w = 0;       // w scaled by the sample aspect ratio
    double   pts = 0;             // seconds, media time
    int      matrix = 1;          // 0 = BT.601, 1 = BT.709, 2 = BT.2020
    bool     full_range = false;
    uint64_t seq = 0;             // grows with every presented frame
    std::vector<uint8_t> data;
};

struct Range { double start, end; };

struct Event {
    std::string type;             // "loadedmetadata", "timeupdate", "error"...
    std::string detail;           // error message, otherwise empty
};

class Player {
public:
    Player();
    ~Player();                    // closes
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // `headers` is a "Name: value\r\n" block (Cookie, Referer, Authorization).
    // allow_file lets the URL (and HLS segments) be file: -- only for pages
    // that are themselves local.
    void open(const std::string& url, const std::string& headers, bool allow_file = false);
    void close();                 // back to "empty"; the object can be reopened
    void play();
    void pause();
    void seek(double seconds);
    void set_volume(double v);    // 0..1
    void set_muted(bool m);
    void set_rate(double r);      // 0.25..4; audio pitch shifts with it
    void set_loop(bool l) { loop_ = l; }

    // State (any thread).
    double duration() const;                    // NaN until known, inf for live
    double current_time() const;
    bool   paused() const { return paused_; }
    bool   ended() const { return ended_; }
    bool   seeking() const { return seeking_; }
    int    ready_state() const { return ready_; }      // HTMLMediaElement values
    int    network_state() const { return net_state_; }
    int    error_code() const { return err_code_; }    // MediaError.code, 0 = none
    std::string error_message() const;
    int    video_width() const { return vw_; }          // display size (SAR applied)
    int    video_height() const { return vh_; }
    bool   has_video() const { return has_video_; }
    bool   has_audio() const { return has_audio_; }
    std::vector<Range> buffered() const;
    std::vector<Range> seekable() const;
    double volume() const { return volume_; }
    bool   muted() const { return muted_; }
    double rate() const { return rate_; }

    // The owner calls this regularly (>= 20 Hz): it detects ended/stalls,
    // updates readyState and returns the events produced since the last call,
    // in DOM order.
    std::vector<Event> poll();

    // Picks the frame due at the current clock; returns it if it is newer than
    // `after_seq` (null otherwise).  The returned frame is immutable.
    std::shared_ptr<const Frame> frame(uint64_t after_seq = 0);

    struct Stats { uint64_t decoded = 0, presented = 0, dropped = 0; bool hw = false; std::string codec; };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;

    std::atomic<bool>   paused_{true}, ended_{false}, seeking_{false}, muted_{false}, loop_{false};
    std::atomic<int>    ready_{0}, net_state_{0}, err_code_{0}, vw_{0}, vh_{0};
    std::atomic<bool>   has_video_{false}, has_audio_{false};
    std::atomic<double> volume_{1.0}, rate_{1.0};
};

} // namespace luxium::media
