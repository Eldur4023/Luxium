// Intrinsic metrics of the form controls the engine draws itself.  The
// constants are measured from Chromium (css-tests/forms/t-forms-default):
// a text field is `size` average characters plus (max - avg) char width of
// its font, a textarea cols*avg plus a 15px scrollbar gutter, checkboxes 13x13.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>

namespace luxium::layout {

namespace {

float text_w(Env& env, const std::string& t, const ComputedStyle& s) {
    if (t.empty()) return 0;
    return env.font.shape_at(t, face_of(env, s), s.font_size).width;
}

} // namespace

std::string password_mask(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
        if ((static_cast<unsigned char>(v[i]) & 0xC0) != 0x80) out += "\xE2\x80\xA2";   // U+2022
    return out;
}

void ctl_metrics(Env& env, Box& b) {
    if (!b.ctl || b.nat_h > 0) return;
    using T = Control::Type;
    const ComputedStyle& s = *b.st;
    const float fs = s.font_size;
    const render::FaceMetrics m = env.font.metrics(face_of(env, s), fs);
    const float line = line_height_px(env, s);
    const float avg = std::round(m.avg_char > 0 ? m.avg_char : text_w(env, "0", s));
    const float extra = m.max_char > avg ? m.max_char - avg : 0.f;   // Blink adds maxCharWidth - avgCharWidth
    switch (b.ctl->type) {
    case T::Text: case T::Password:
        b.nat_w = std::floor(std::ceil(b.ctl->size * avg) + extra);
        b.nat_h = line;
        b.ctl_baseline = m.ascent + (line - (m.ascent + m.descent)) / 2;
        break;
    case T::Textarea:
        b.nat_w = std::ceil(b.ctl->cols * avg) + 15;
        b.nat_h = b.ctl->rows * line;
        b.ctl_baseline = -1;   // no baseline: aligns by its bottom margin edge
        break;
    case T::Checkbox: case T::Radio: b.nat_w = b.nat_h = 13; b.ctl_baseline = 13; break;
    case T::Range: b.nat_w = 129; b.nat_h = 16; b.ctl_baseline = 16; break;
    case T::File: {
        b.nat_w = std::round(fs * 23.025f);   // Chromium: button + label in a ~23em box
        b.nat_h = line + 6;
        b.ctl_baseline = 3 + m.ascent;
        break;
    }
    case T::Select: {
        float w = 0;
        for (const auto& o : b.ctl->options) w = std::max(w, text_w(env, o, s));
        const bool native = !s.appearance_none;   // the native look adds the arrow gutter and inner padding
        b.nat_w = std::ceil(w) + (native ? 20 : 0);
        b.nat_h = line + (native ? 2 : 0);
        b.ctl_baseline = (native ? 1 : 0) + m.ascent;
        break;
    }
    }
}

} // namespace luxium::layout
