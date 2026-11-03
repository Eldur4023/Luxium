// CSS transitions for opacity, translate(), background-color, color and border-color.
//
// The cascade calls style_hook with every element's final style.  The hook remembers the target values of the
// last layout; when they change and `transition` covers the change it starts a tween from what is currently
// displayed, then overwrites the style with the tween's sample on each following layout.  Layout is
// recomputed every frame while a tween runs (frame() keeps the page dirty), so nothing else needs to know.

#include "shell/shell.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace luxium::shell {

namespace {

using style::TransitionSpec;

float bezier(const float* b, float x) {   // CSS cubic-bezier(b0,b1,b2,b3) solved for y at progress x
    auto at = [](float t, float p1, float p2) { const float u = 1 - t; return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t; };
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; ++i) {
        const float cx = at(t, b[0], b[2]);
        if (std::fabs(cx - x) < 1e-4f) break;
        if (cx < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
    }
    return at(t, b[1], b[3]);
}

uint32_t mix(uint32_t a, uint32_t b, float k) {   // premultiplied interpolation of ARGB colours
    auto ch = [](uint32_t c, int sh) { return float((c >> sh) & 255); };
    const float aa = ch(a, 24), ab = ch(b, 24), ao = aa + (ab - aa) * k;
    if (ao <= 0) return 0;
    auto c = [&](int sh) { return (ch(a, sh) * aa + (ch(b, sh) * ab - ch(a, sh) * aa) * k) / ao; };
    auto q = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f) + 0.5f); };
    return (q(ao) << 24) | (q(c(16)) << 16) | (q(c(8)) << 8) | q(c(0));
}

float lerp(float a, float b, float k) { return a + (b - a) * k; }
style::Len lerp_len(const style::Len& a, const style::Len& b, float k) {
    style::Len r = b;
    r.px = lerp(a.px, b.px, k); r.pct = lerp(a.pct, b.pct, k);
    r.pct_unit = a.pct_unit || b.pct_unit;
    return r;
}

enum Group : unsigned { GOpacity = 1, GTransform = 2, GBackground = 4, GColor = 8, GBorder = 16 };

AnimVals vals_of(const style::ComputedStyle& s) {
    AnimVals v;
    v.opacity = s.opacity; v.tx = s.translate_x; v.ty = s.translate_y;
    v.bg = s.bg_color; v.color = s.color;
    for (int i = 0; i < 4; ++i) v.border[i] = s.border_color[i];
    return v;
}

unsigned diff(const AnimVals& a, const AnimVals& b) {
    unsigned g = 0;
    if (a.opacity != b.opacity) g |= GOpacity;
    if (!(a.tx == b.tx) || !(a.ty == b.ty)) g |= GTransform;
    if (a.bg != b.bg) g |= GBackground;
    if (a.color != b.color) g |= GColor;
    for (int i = 0; i < 4; ++i) if (a.border[i] != b.border[i]) g |= GBorder;
    return g;
}

AnimVals blend(const AnimVals& a, const AnimVals& b, float k, unsigned groups) {
    AnimVals r = b;
    if (groups & GOpacity) r.opacity = lerp(a.opacity, b.opacity, k);
    if (groups & GTransform) { r.tx = lerp_len(a.tx, b.tx, k); r.ty = lerp_len(a.ty, b.ty, k); }
    if (groups & GBackground) r.bg = mix(a.bg, b.bg, k);
    if (groups & GColor) r.color = mix(a.color, b.color, k);
    if (groups & GBorder) for (int i = 0; i < 4; ++i) r.border[i] = mix(a.border[i], b.border[i], k);
    return r;
}

void apply(style::ComputedStyle& s, const AnimVals& v, unsigned groups) {
    if (groups & GOpacity) s.opacity = v.opacity;
    if (groups & GTransform) { s.translate_x = v.tx; s.translate_y = v.ty; }
    if (groups & GBackground) s.bg_color = v.bg;
    if (groups & GColor) s.color = v.color;
    if (groups & GBorder) for (int i = 0; i < 4; ++i) { s.border_color[i] = v.border[i]; s.bc_current[i] = false; }
}

} // namespace

bool Shell::style_hook(const dom::Node& el, style::ComputedStyle& s) {
    auto found = anims_.find(&el);
    if (found == anims_.end() && s.transitions.empty()) return false;
    NodeAnim& a = found != anims_.end() ? found->second : anims_[&el];
    a.seen = anim_frame_;
    const AnimVals cur = vals_of(s);
    const float now = float(SDL_GetTicks());
    if (!a.has_target) { a.target = cur; a.has_target = true; return false; }   // first sight: nothing to animate from
    // border colours that follow currentcolor are not tracked separately: they change with `color`
    if (const unsigned changed = diff(a.target, cur)) {
        unsigned covered = 0;
        const TransitionSpec* spec = nullptr;
        for (const auto& t : s.transitions) {
            const unsigned g = (t.props & TransitionSpec::Opacity ? GOpacity : 0) | (t.props & TransitionSpec::Transform ? GTransform : 0) |
                               (t.props & TransitionSpec::Background ? GBackground : 0) | (t.props & TransitionSpec::Color ? GColor : 0) |
                               (t.props & TransitionSpec::Border ? GBorder : 0);
            if ((g & changed) && t.duration + t.delay > 0) { covered |= g & changed; if (!spec) spec = &t; }
        }
        if (spec && covered) {
            // start from what is on screen now (a half-way tween keeps going from where it is)
            AnimVals shown = a.target;
            if (a.active) {
                const float k = a.dur > 0 ? std::clamp((now - a.t0 - a.delay) / a.dur, 0.f, 1.f) : 1.f;
                shown = blend(a.from, a.target, bezier(a.bez, k), a.groups);
            }
            a.from = shown; a.t0 = now; a.dur = spec->duration; a.delay = spec->delay;
            std::copy(spec->bez, spec->bez + 4, a.bez);
            a.groups = covered; a.active = true;
        } else a.active = false;
        a.target = cur;
    }
    if (!a.active) return false;
    const float k = a.dur > 0 ? (now - a.t0 - a.delay) / a.dur : 1.f;
    if (k >= 1.f) { a.active = false; return false; }
    apply(s, blend(a.from, a.target, bezier(a.bez, std::clamp(k, 0.f, 1.f)), a.groups), a.groups);
    return true;
}

} // namespace luxium::shell
