// Controls and frame hand-over for <video>/<audio> in the own engine.  Layout
// leaves a MediaBox per element; the shell swaps in the current picture and
// asks for the controls as one more display list drawn on top.

#include <render/layout.hpp>
#include <media/own_media.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace luxium::render {

namespace {

constexpr float kBarH = 32, kBtnW = 32, kTimeW = 100, kPad = 8;

struct Bar { float x, y, w, h; };

Bar bar_of(const MediaBox& m) {
    if (m.audio) return {m.x, m.y, m.w, m.h};
    const float h = std::min(kBarH, m.h);
    return {m.x, m.y + m.h - h, m.w, h};
}

// Progress track: between the play button and the time label.
void track_of(const Bar& b, float& x0, float& x1) {
    x0 = b.x + kBtnW + 4;
    x1 = std::max(x0 + 10, b.x + b.w - kTimeW - kPad);
}

std::string fmt_time(double t) {
    if (!(t >= 0)) t = 0;
    const int s = int(t), h = s / 3600, m = s / 60 % 60;
    char buf[32];
    if (h) std::snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, s % 60);
    else std::snprintf(buf, sizeof buf, "%d:%02d", m, s % 60);
    return buf;
}

paint::Item rect(float x, float y, float w, float h, uint32_t argb, float r = 0) {
    paint::Item it; it.kind = paint::Item::Kind::Rect;
    it.x = x; it.y = y; it.w = w; it.h = h; it.color = argb;
    it.radii = {r, r, r, r};
    return it;
}

} // namespace

void media_patch(LayoutResult& l) {
    for (const MediaBox& m : l.media) {
        paint::DisplayList& dl = m.fixed ? l.fixed : l.list;
        if (m.item >= dl.size()) continue;
        paint::Item& it = dl[m.item];
        auto bm = media::own::frame(m.node);
        if (!bm) continue;
        it.kind = paint::Item::Kind::Image;
        it.img = std::move(bm);
        it.sx = m.fx0 * it.img->w; it.sy = m.fy0 * it.img->h;
        it.sw = (m.fx1 - m.fx0) * it.img->w; it.sh = (m.fy1 - m.fy0) * it.img->h;
    }
}

paint::DisplayList media_overlay(const LayoutResult& l, bool fixed, const Font& font) {
    paint::DisplayList dl;
    const FaceKey face = font.resolve("sans-serif", 400, false);
    for (const MediaBox& m : l.media) {
        if (m.fixed != fixed || !m.controls) continue;
        const media::own::State st = media::own::state(m.node);
        const Bar b = bar_of(m);
        paint::Item clip; clip.kind = paint::Item::Kind::ClipPush;
        clip.x = m.x; clip.y = m.y; clip.w = m.w; clip.h = m.h;
        dl.push_back(clip);
        dl.push_back(rect(b.x, b.y, b.w, b.h, m.audio ? 0xFF2B2F36 : 0xB3000000));

        auto text = [&](const std::string& s, float x, uint32_t color, float px) {
            paint::Item t; t.kind = paint::Item::Kind::Text;
            t.run = std::make_shared<const ShapedRun>(font.shape_at(s, face, px));
            t.face = face; t.px = px; t.color = color; t.x = x;
            t.baseline = b.y + b.h / 2 + px * 0.35f;
            dl.push_back(std::move(t));
        };
        const bool playing = st.known && !st.paused && !st.ended;
        const float cx = b.x + kBtnW / 2, cy = b.y + b.h / 2;
        if (playing) {
            dl.push_back(rect(cx - 5, cy - 6, 4, 12, 0xFFFFFFFF));
            dl.push_back(rect(cx + 1, cy - 6, 4, 12, 0xFFFFFFFF));
        } else {
            text("\xE2\x96\xB6", cx - 6, 0xFFFFFFFF, 14);
        }
        float x0, x1;
        track_of(b, x0, x1);
        const float ty = cy - 2;
        dl.push_back(rect(x0, ty, x1 - x0, 4, 0x55FFFFFF, 2));
        if (st.duration > 0) {
            const float tw = x1 - x0;
            dl.push_back(rect(x0, ty, tw * float(std::min(1.0, st.buffered_end / st.duration)), 4, 0x66FFFFFF, 2));
            const float pw = tw * float(std::clamp(st.time / st.duration, 0.0, 1.0));
            dl.push_back(rect(x0, ty, pw, 4, 0xFF4DA3FF, 2));
            dl.push_back(rect(x0 + pw - 5, cy - 5, 10, 10, 0xFFFFFFFF, 5));
        }
        text(fmt_time(st.time) + " / " + (st.duration > 0 ? fmt_time(st.duration) : "--:--"),
             b.x + b.w - kTimeW, 0xFFFFFFFF, 12);
        paint::Item pop; pop.kind = paint::Item::Kind::ClipPop;
        dl.push_back(pop);
    }
    return dl;
}

bool media_click(const MediaBox& m, float x, float y) {
    if (!m.controls) return false;
    const Bar b = bar_of(m);
    if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h) return false;
    float x0, x1;
    track_of(b, x0, x1);
    if (x < b.x + kBtnW) media::own::toggle(m.node);
    else if (x >= x0 - 4 && x <= x1 + 4) media::own::seek_fraction(m.node, (x - x0) / (x1 - x0));
    return true;
}

} // namespace luxium::render
