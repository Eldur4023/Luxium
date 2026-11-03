// Stand-in when the FFmpeg headers are absent (LUXIUM_MEDIA=OFF): media
// elements simply fail with MEDIA_ERR_SRC_NOT_SUPPORTED.
#include "player.hpp"

namespace luxium::media {

struct Player::Impl { std::vector<Event> ev; };

Player::Player() : d(std::make_unique<Impl>()) {}
Player::~Player() = default;
void Player::open(const std::string&, const std::string&, bool) {
    d->ev.push_back({"loadstart", {}});
    err_code_ = 4;
    d->ev.push_back({"error", "built without FFmpeg"});
}
void Player::close() {}
void Player::play() {}
void Player::pause() {}
void Player::seek(double) {}
void Player::set_volume(double) {}
void Player::set_muted(bool) {}
void Player::set_rate(double) {}
double Player::duration() const { return 0.0 / 0.0; }
double Player::current_time() const { return 0; }
std::string Player::error_message() const { return "built without FFmpeg"; }
std::vector<Range> Player::buffered() const { return {}; }
std::vector<Range> Player::seekable() const { return {}; }
std::vector<Event> Player::poll() { auto e = std::move(d->ev); d->ev.clear(); return e; }
std::shared_ptr<const Frame> Player::frame(uint64_t) { return nullptr; }
Player::Stats Player::stats() const { return {}; }
std::string can_play(const std::string&) { return ""; }
bool built() { return false; }
std::string capabilities_json() { return "{\"video\":[],\"audio\":[],\"subtitle\":[],\"containers\":[]}"; }
std::vector<TrackInfo> Player::audio_tracks() const { return {}; }
std::vector<TrackInfo> Player::subtitle_tracks() const { return {}; }
bool Player::select_audio(int) { return false; }
std::vector<SubCue> Player::sub_cues(int, size_t, size_t& total) const { total = 0; return {}; }

} // namespace luxium::media
