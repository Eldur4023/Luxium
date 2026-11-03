// CSS transitions and @keyframes animations: opacity, transform, colours and lengths.
//
// The cascade calls style_hook with every element's final style.  The hook remembers the target values of the
// last layout; when they change and `transition` covers the change it starts a tween from what is currently
// displayed, then overwrites the style with the tween's sample on each following layout.  Layout is
// recomputed every frame while a tween runs (frame() keeps the page dirty), so nothing else needs to know.

#include "shell/shell.hpp"

#include <style/properties.hpp>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
    if (a.is_auto || b.is_auto || a.fn || b.fn) return k < 0.5f ? a : b;   // not interpolable: flip half-way
    style::Len r = b;
    r.px = lerp(a.px, b.px, k); r.pct = lerp(a.pct, b.pct, k);
    r.pct_unit = a.pct_unit || b.pct_unit;
    return r;
}

enum Group : unsigned { GOpacity = 1, GTransform = 2, GBackground = 4, GColor = 8, GBorder = 16, GLength = 32 };

bool same_fn(const style::TfFn& a, const style::TfFn& b) {
    return a.kind == b.kind && a.x == b.x && a.y == b.y && a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d && a.e == b.e && a.f == b.f;
}
bool same_tfn(const std::vector<style::TfFn>& a, const std::vector<style::TfFn>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), same_fn);
}

style::TfFn identity_of(char kind) {
    style::TfFn f; f.kind = kind;
    if (kind == 'S') f.a = f.b = 1;
    if (kind == 'M') f.a = f.d = 1;
    return f;
}

// Interpolates function by function when both lists have the same shape (a missing list is the identity of the
// other's shape: `none` -> rotate(360deg) turns), otherwise flips half-way.
std::vector<style::TfFn> blend_tfn(const std::vector<style::TfFn>& a, const std::vector<style::TfFn>& b, float k) {
    std::vector<style::TfFn> A = a, B = b;
    if (A.empty()) for (const auto& f : b) A.push_back(identity_of(f.kind));
    if (B.empty()) for (const auto& f : a) B.push_back(identity_of(f.kind));
    if (A.size() != B.size()) return k < 0.5f ? a : b;
    for (size_t i = 0; i < A.size(); ++i) if (A[i].kind != B[i].kind) return k < 0.5f ? a : b;
    std::vector<style::TfFn> r = B;
    for (size_t i = 0; i < r.size(); ++i) {
        const style::TfFn &x = A[i], &y = B[i];
        style::TfFn& o = r[i];
        o.x = lerp_len(x.x, y.x, k); o.y = lerp_len(x.y, y.y, k);
        o.a = lerp(x.a, y.a, k); o.b = lerp(x.b, y.b, k); o.c = lerp(x.c, y.c, k);
        o.d = lerp(x.d, y.d, k); o.e = lerp(x.e, y.e, k); o.f = lerp(x.f, y.f, k);
    }
    return r;
}

AnimVals vals_of(const style::ComputedStyle& s) {
    AnimVals v;
    v.opacity = s.opacity; v.tfn = s.tfn;
    v.bg = s.bg_color; v.color = s.color;
    for (int i = 0; i < 4; ++i) { v.border[i] = s.border_color[i]; v.inset[i] = s.inset[i]; v.margin[i] = s.margin[i]; v.padding[i] = s.padding[i]; }
    v.width = s.width; v.height = s.height;
    return v;
}

unsigned diff(const AnimVals& a, const AnimVals& b) {
    unsigned g = 0;
    if (a.opacity != b.opacity) g |= GOpacity;
    if (!same_tfn(a.tfn, b.tfn)) g |= GTransform;
    if (a.bg != b.bg) g |= GBackground;
    if (a.color != b.color) g |= GColor;
    if (!(a.width == b.width) || !(a.height == b.height)) g |= GLength;
    for (int i = 0; i < 4; ++i) {
        if (a.border[i] != b.border[i]) g |= GBorder;
        if (!(a.inset[i] == b.inset[i]) || !(a.margin[i] == b.margin[i]) || !(a.padding[i] == b.padding[i])) g |= GLength;
    }
    return g;
}

AnimVals blend(const AnimVals& a, const AnimVals& b, float k, unsigned groups) {
    AnimVals r = b;
    if (groups & GOpacity) r.opacity = lerp(a.opacity, b.opacity, k);
    if (groups & GTransform) r.tfn = blend_tfn(a.tfn, b.tfn, k);
    if (groups & GBackground) r.bg = mix(a.bg, b.bg, k);
    if (groups & GColor) r.color = mix(a.color, b.color, k);
    if (groups & GBorder) for (int i = 0; i < 4; ++i) r.border[i] = mix(a.border[i], b.border[i], k);
    if (groups & GLength) {
        r.width = lerp_len(a.width, b.width, k); r.height = lerp_len(a.height, b.height, k);
        for (int i = 0; i < 4; ++i) { r.inset[i] = lerp_len(a.inset[i], b.inset[i], k); r.margin[i] = lerp_len(a.margin[i], b.margin[i], k); r.padding[i] = lerp_len(a.padding[i], b.padding[i], k); }
    }
    return r;
}

void apply(style::ComputedStyle& s, const AnimVals& v, unsigned groups) {
    if (groups & GOpacity) s.opacity = v.opacity;
    if (groups & GTransform) { s.tfn = v.tfn; s.rebuild_transform(); }
    if (groups & GBackground) s.bg_color = v.bg;
    if (groups & GColor) s.color = v.color;
    if (groups & GBorder) for (int i = 0; i < 4; ++i) { s.border_color[i] = v.border[i]; s.bc_current[i] = false; }
    if (groups & GLength) {
        s.width = v.width; s.height = v.height;
        for (int i = 0; i < 4; ++i) { s.inset[i] = v.inset[i]; s.margin[i] = v.margin[i]; s.padding[i] = v.padding[i]; }
    }
}

// Which AnimVals groups a CSS property name feeds.
unsigned group_of(const std::string& n) {
    if (n == "opacity") return GOpacity;
    if (n == "transform") return GTransform;
    if (n == "background" || n == "background-color") return GBackground;
    if (n == "color") return GColor;
    if (n == "border" || n == "border-color" || n.starts_with("border-") && n.ends_with("-color")) return GBorder;
    if (n == "width" || n == "height" || n == "top" || n == "left" || n == "right" || n == "bottom" || n == "inset" ||
        n.starts_with("margin") || n.starts_with("padding")) return GLength;
    return 0;
}

// Solves the animation timing function at x (0..1).
float ease(const style::AnimationSpec& sp, float x) {
    if (sp.steps > 0) {
        const float n = float(sp.steps);
        const float j = sp.steps_start ? std::ceil(x * n) : std::floor(x * n);
        return std::clamp(j / n, 0.f, 1.f);
    }
    return bezier(sp.bez, x);
}

// var(--x[, fallback]) from the element's custom properties, so keyframes can use them.
std::string subst_vars(const std::string& v, const style::ComputedStyle& s) {
    std::string out = v;
    for (int guard = 0; guard < 8; ++guard) {
        const size_t p = out.find("var(");
        if (p == std::string::npos) break;
        int d = 0; size_t e = p + 3;
        for (; e < out.size(); ++e) { if (out[e] == '(') ++d; else if (out[e] == ')' && --d == 0) break; }
        if (e >= out.size()) break;
        const std::string inner = out.substr(p + 4, e - p - 4);
        const size_t comma = inner.find(',');
        const std::string key = style::trim_ws(inner.substr(0, comma));
        auto it = s.custom.find(key);
        std::string rep = it != s.custom.end() ? it->second : comma == std::string::npos ? "" : style::trim_ws(inner.substr(comma + 1));
        out.replace(p, e - p + 1, rep);
    }
    return out;
}

// LUXIUM_ANIM_TIME=ms freezes every @keyframes animation at that time since its start (deterministic frame dumps).
const float* frozen_anim_time() {
    static float t = -1;
    static bool init = false;
    if (!init) { init = true; if (const char* e = std::getenv("LUXIUM_ANIM_TIME")) t = float(std::atof(e)); }
    return t >= 0 ? &t : nullptr;
}

} // namespace

const std::vector<Shell::Keyframe>* Shell::find_keyframes(const std::string& name) {
    const auto& rules = page_.rules();
    if (kf_src_ != rules.data() || kf_n_ != rules.size()) {
        kf_src_ = rules.data(); kf_n_ = rules.size();
        keyframes_.clear();
        for (const auto& r : rules) {
            std::string n; float off = 0;
            if (!r.keyframe(&n, &off)) continue;
            auto& v = keyframes_[n];
            auto at = std::upper_bound(v.begin(), v.end(), off, [](float o, const Keyframe& k) { return o < k.offset; });
            v.insert(at, Keyframe{off, r.decls});   // equal offsets keep source order: the later one wins
        }
    }
    auto it = keyframes_.find(name);
    return it == keyframes_.end() ? nullptr : &it->second;
}

// Overlays the @keyframes animations on `s` (after transitions).  Returns true when the style changed.
bool Shell::animate_keyframes(NodeAnim& a, style::ComputedStyle& s) {
    const float now = float(SDL_GetTicks());
    const float* frozen = frozen_anim_time();
    if (a.kf.size() != s.animations.size()) a.kf.assign(s.animations.size(), {});
    a.kf_active = false;
    bool changed = false;
    const AnimVals base = vals_of(s);
    style::Env env;
    env.em = s.font_size; env.vw = float(win_w_); env.vh = float(win_h_);
    for (size_t i = 0; i < s.animations.size(); ++i) {
        const style::AnimationSpec& sp = s.animations[i];
        NodeAnim::Kf& st = a.kf[i];
        if (sp.name.empty()) continue;
        const auto* frames = find_keyframes(sp.name);
        if (!frames) continue;
        if (st.name != sp.name) { st = {}; st.name = sp.name; st.t0 = frozen ? 0.f : now; }
        // pause: the clock stops, resuming shifts the origin by the time spent paused
        if (sp.paused && st.paused_at < 0) st.paused_at = now;
        if (!sp.paused && st.paused_at >= 0) { st.t0 += now - st.paused_at; st.paused_at = -1; }
        const float clock = frozen ? *frozen : (st.paused_at >= 0 ? st.paused_at : now) - st.t0;
        const float elapsed = clock - sp.delay;
        const float dur = std::max(sp.duration, 0.f);
        const float total = dur * sp.iterations;
        float iter_pos;                      // position in iterations since start
        if (elapsed < 0) {
            if (!(sp.fill & 2)) { if (!frozen && !sp.paused) a.kf_active = true; continue; }
            iter_pos = 0;
        } else if (dur <= 0 || elapsed >= total) {
            if (!(sp.fill & 1)) continue;
            iter_pos = std::isinf(sp.iterations) ? 0.f : sp.iterations;   // finished: stays at the last iteration's end
        } else {
            iter_pos = elapsed / dur;
            if (!frozen && !sp.paused) a.kf_active = true;
        }
        float cur_iter = std::floor(iter_pos), p = iter_pos - cur_iter;
        if (iter_pos > 0 && p == 0 && !std::isinf(sp.iterations) && iter_pos >= sp.iterations) { cur_iter -= 1; p = 1; }
        const bool rev = sp.direction == 1 || (sp.direction == 2 && int(cur_iter) % 2 == 1) || (sp.direction == 3 && int(cur_iter) % 2 == 0);
        if (rev) p = 1 - p;

        // per group: the keyframes that declare it (+ the underlying value at 0%/100% when they don't)
        struct Stop { float off; AnimVals v; };
        std::vector<Stop> per[6];
        static constexpr unsigned kGroups[6] = {GOpacity, GTransform, GBackground, GColor, GBorder, GLength};
        for (const auto& kf : *frames) {
            style::ComputedStyle tmp = s;
            unsigned declared = 0;
            for (const auto& d : kf.decls) {
                const unsigned g = group_of(d.name);
                if (g && style::apply_property(tmp, env, d.name, subst_vars(d.value, s))) declared |= g;
            }
            if (!declared) continue;
            const AnimVals v = vals_of(tmp);
            for (int g = 0; g < 6; ++g) if (declared & kGroups[g]) {
                auto& vec = per[g];
                if (!vec.empty() && vec.back().off == kf.offset) vec.back().v = v; else vec.push_back({kf.offset, v});
            }
        }
        for (int g = 0; g < 6; ++g) {
            auto& vec = per[g];
            if (vec.empty()) continue;
            if (vec.front().off > 0) vec.insert(vec.begin(), {0.f, base});
            if (vec.back().off < 1) vec.push_back({1.f, base});
            size_t hi = 1;
            while (hi + 1 < vec.size() && vec[hi].off < p) ++hi;
            const Stop &lo = vec[hi - 1], &up = vec[hi];
            const float span = up.off - lo.off;
            const float local = span > 0 ? std::clamp((p - lo.off) / span, 0.f, 1.f) : 1.f;
            apply(s, blend(lo.v, up.v, ease(sp, local), kGroups[g]), kGroups[g]);
            changed = true;
        }
    }
    return changed;
}

bool Shell::style_hook(const dom::Node& el, style::ComputedStyle& s, int pseudo) {
    // ::before/::after animate separately from their element: the key is the node address nudged by the pseudo id
    const dom::Node* key = reinterpret_cast<const dom::Node*>(reinterpret_cast<uintptr_t>(&el) + uintptr_t(pseudo));
    auto found = anims_.find(key);
    if (found == anims_.end() && s.transitions.empty() && s.animations.empty()) return false;
    NodeAnim& a = found != anims_.end() ? found->second : anims_[key];
    a.seen = anim_frame_;
    bool changed = false;
    const AnimVals cur = vals_of(s);
    const float now = float(SDL_GetTicks());
    if (!a.has_target) { a.target = cur; a.has_target = true; }   // first sight: nothing to transition from
    else {
        // border colours that follow currentcolor are not tracked separately: they change with `color`
        if (const unsigned changed_groups = diff(a.target, cur)) {
            unsigned covered = 0;
            const TransitionSpec* spec = nullptr;
            for (const auto& t : s.transitions) {
                const unsigned g = (t.props & TransitionSpec::Opacity ? GOpacity : 0) | (t.props & TransitionSpec::Transform ? GTransform : 0) |
                                   (t.props & TransitionSpec::Background ? GBackground : 0) | (t.props & TransitionSpec::Color ? GColor : 0) |
                                   (t.props & TransitionSpec::Border ? GBorder : 0) | (t.props & TransitionSpec::Length ? GLength : 0);
                if ((g & changed_groups) && t.duration + t.delay > 0) { covered |= g & changed_groups; if (!spec) spec = &t; }
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
        if (a.active) {
            const float k = a.dur > 0 ? (now - a.t0 - a.delay) / a.dur : 1.f;
            if (k >= 1.f) a.active = false;
            else { apply(s, blend(a.from, a.target, bezier(a.bez, std::clamp(k, 0.f, 1.f)), a.groups), a.groups); changed = true; }
        }
    }
    if (!s.animations.empty()) changed = animate_keyframes(a, s) || changed;
    else a.kf_active = false;
    return changed;
}

} // namespace luxium::shell
