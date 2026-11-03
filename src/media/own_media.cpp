// The own engine's media registry: one media::Player per element, pictures
// converted from NV12 to the RGB bitmaps the display list already paints.
#include "own_media.hpp"
#include "player.hpp"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace luxium::media::own {

namespace {

constexpr int kMaxW = 1280;   // pictures wider than this are scaled down on conversion

struct Entry {
    std::unique_ptr<Player> p = std::make_unique<Player>();
    Elem elem;
    bool live = false;               // seen by the last layout
    bool started_auto = false;
    bool waiting = false;
    uint64_t seq = 0;
    int vw = 0, vh = 0;              // last display size reported to layout
    std::shared_ptr<const render::ImageBitmap> bmp;
    SwsContext* sws = nullptr;
    ~Entry() { if (sws) sws_freeContext(sws); p->close(); }
};

std::unordered_map<const void*, Entry>& reg() {
    static std::unordered_map<const void*, Entry> r;
    return r;
}

void convert(Entry& e, const Frame& f) {
    if (f.w <= 0 || f.h <= 0 || f.display_w <= 0) return;
    const double k = std::min(1.0, double(kMaxW) / f.display_w);
    const int ow = std::max(2, int(f.display_w * k)), oh = std::max(2, int(f.h * k));
    e.sws = sws_getCachedContext(e.sws, f.w, f.h, AV_PIX_FMT_NV12, ow, oh, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!e.sws) return;
    const int cs = f.matrix == 0 ? SWS_CS_ITU601 : f.matrix == 2 ? SWS_CS_BT2020 : SWS_CS_ITU709;
    sws_setColorspaceDetails(e.sws, sws_getCoefficients(cs), f.full_range ? 1 : 0,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    auto bm = std::make_shared<render::ImageBitmap>();
    bm->w = ow; bm->h = oh;
    bm->rgb.resize(size_t(ow) * oh * 3);
    const uint8_t* src[4] = {f.data.data(), f.data.data() + size_t(f.w) * f.h, nullptr, nullptr};
    const int sstride[4] = {f.w, (f.w + 1) / 2 * 2, 0, 0};
    uint8_t* dst[4] = {bm->rgb.data(), nullptr, nullptr, nullptr};
    const int dstride[4] = {ow * 3, 0, 0, 0};
    sws_scale(e.sws, src, sstride, 0, f.h, dst, dstride);
    e.bmp = std::move(bm);
}

} // namespace

Info attach(const void* key, const Elem& el) {
    Entry& e = reg()[key];
    if (e.elem.src != el.src || e.elem.local != el.local) {
        e.bmp.reset(); e.seq = 0; e.started_auto = false; e.vw = e.vh = 0;
        if (!el.src.empty()) e.p->open(el.src, "", el.local);
        else e.p->close();
    }
    e.elem = el;
    e.live = true;
    e.p->set_loop(el.loop);
    e.p->set_muted(el.muted);
    return {float(e.p->video_width()), float(e.p->video_height())};
}

void sweep() {
    for (auto it = reg().begin(); it != reg().end();) {
        if (!it->second.live) it = reg().erase(it);
        else { it->second.live = false; ++it; }
    }
}

void reset() { reg().clear(); }

int tick() {
    int r = 0;
    for (auto& [key, e] : reg()) {
        for (const Event& ev : e.p->poll()) {
            r |= 1;
            if (ev.type == "waiting") e.waiting = true;
            else if (ev.type == "playing" || ev.type == "canplay") e.waiting = false;
            if (ev.type == "canplay" && e.elem.autoplay && !e.started_auto) {
                e.started_auto = true;
                e.p->play();
            }
        }
        if (auto f = e.p->frame(e.seq)) {
            e.seq = f->seq;
            convert(e, *f);
            r |= 1;
        }
        if (e.p->video_width() != e.vw || e.p->video_height() != e.vh) {
            e.vw = e.p->video_width(); e.vh = e.p->video_height();
            r |= 2;
        }
        if (!e.p->paused()) r |= 1;   // the clock moves: progress bar and time
    }
    return r;
}

std::shared_ptr<const render::ImageBitmap> frame(const void* key) {
    auto it = reg().find(key);
    return it == reg().end() ? nullptr : it->second.bmp;
}

State state(const void* key) {
    State s;
    auto it = reg().find(key);
    if (it == reg().end()) return s;
    const Player& p = *it->second.p;
    s.known = true;
    s.paused = p.paused(); s.ended = p.ended(); s.seeking = p.seeking();
    s.waiting = it->second.waiting; s.error = p.error_code() != 0;
    s.time = p.current_time();
    const double d = p.duration();
    s.duration = std::isfinite(d) ? d : 0;
    for (const Range& r : p.buffered())
        if (r.start <= s.time + 0.5) s.buffered_end = std::max(s.buffered_end, r.end);
    return s;
}

static Player* find(const void* key) {
    auto it = reg().find(key);
    return it == reg().end() ? nullptr : it->second.p.get();
}

void play(const void* key) { if (Player* p = find(key)) p->play(); }
void pause(const void* key) { if (Player* p = find(key)) p->pause(); }
void toggle(const void* key) {
    if (Player* p = find(key)) { if (p->paused() || p->ended()) p->play(); else p->pause(); }
}
void seek(const void* key, double s) { if (Player* p = find(key)) p->seek(std::max(0.0, s)); }
void seek_fraction(const void* key, double f) {
    Player* p = find(key);
    if (!p) return;
    const double d = p->duration();
    if (std::isfinite(d) && d > 0) p->seek(std::clamp(f, 0.0, 1.0) * d);
}

} // namespace luxium::media::own
