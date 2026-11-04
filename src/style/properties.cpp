#include <cstdlib>
#include "style/properties.hpp"
#include "style/stylesheet.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace luxium::render { float image_pixel_ratio(); }

namespace luxium::style {

namespace {

using Tokens = std::vector<std::string>;

bool one_of(const std::string& v, std::initializer_list<const char*> kws) {
    for (const char* k : kws) if (v == k) return true;
    return false;
}

// 1..4 values -> top/right/bottom/left
bool parse_edges(const Tokens& t, const Env& env, Len out[4], bool allow_auto, bool nonneg) {
    if (t.empty() || t.size() > 4) return false;
    Len v[4];
    for (size_t i = 0; i < t.size(); ++i) {
        if (!parse_length(t[i], env, v[i], allow_auto)) return false;
        if (nonneg && !v[i].is_auto && (v[i].px < 0 || v[i].pct < 0)) return false;
    }
    out[0] = v[0];
    out[1] = t.size() > 1 ? v[1] : v[0];
    out[2] = t.size() > 2 ? v[2] : v[0];
    out[3] = t.size() > 3 ? v[3] : (t.size() > 1 ? v[1] : v[0]);
    return true;
}

bool parse_bstyle(const std::string& v, BorderStyle& out) {
    static const std::unordered_map<std::string, BorderStyle> m = {
        {"none", BorderStyle::None}, {"hidden", BorderStyle::Hidden}, {"solid", BorderStyle::Solid},
        {"dashed", BorderStyle::Dashed}, {"dotted", BorderStyle::Dotted}, {"double", BorderStyle::Double},
        {"groove", BorderStyle::Groove}, {"ridge", BorderStyle::Ridge}, {"inset", BorderStyle::Inset},
        {"outset", BorderStyle::Outset}};
    auto it = m.find(v);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

bool parse_bwidth(const std::string& v, const Env& env, float& w) {
    if (v == "thin") { w = 1; return true; }
    if (v == "medium") { w = 3; return true; }
    if (v == "thick") { w = 5; return true; }
    Len l;
    if (!parse_length(v, env, l) || l.has_pct() || l.px < 0) return false;
    w = l.px;
    return true;
}

int side_index(const std::string& n) {   // "top" "right" "bottom" "left"
    if (n == "top") return 0;
    if (n == "right") return 1;
    if (n == "bottom") return 2;
    if (n == "left") return 3;
    return -1;
}

uint32_t color_or(const std::string& t, uint32_t cur, bool& ok) {
    uint32_t c = 0; bool isc = false;
    ok = parse_color(t, c, &isc);
    return isc ? cur : c;
}

// sets a border side color; `currentcolor` (or no color at all) keeps following `color`
bool set_bcolor(ComputedStyle& s, int i, const std::string& tok) {
    uint32_t c = 0; bool isc = false;
    if (!parse_color(tok, c, &isc)) return false;
    s.bc_current[i] = isc;
    s.border_color[i] = c;
    return true;
}

Align parse_align(std::string v, bool& ok) {
    ok = true;
    if (v.starts_with("safe ") || v.starts_with("unsafe ")) v = trim_ws(v.substr(v.find(' ')));
    if (v.starts_with("first ")) v = trim_ws(v.substr(6));
    if (v == "auto") return Align::Auto;
    if (v == "normal") return Align::Normal;
    if (v == "stretch") return Align::Stretch;
    if (v == "start" || v == "self-start" || v == "left") return Align::Start;
    if (v == "end" || v == "self-end" || v == "right") return Align::End;
    if (v == "center") return Align::Center;
    if (v == "flex-start") return Align::FlexStart;
    if (v == "flex-end") return Align::FlexEnd;
    if (v == "baseline") return Align::Baseline;
    if (v == "space-between") return Align::SpaceBetween;
    if (v == "space-around") return Align::SpaceAround;
    if (v == "space-evenly") return Align::SpaceEvenly;
    ok = false;
    return Align::Normal;
}

// grid track
bool parse_track(const std::string& raw, const Env& env, Track& t) {
    const std::string v = lower_ascii(trim_ws(raw));
    t = Track{};
    if (v == "auto") { t.kind = Track::Kind::Auto; return true; }
    if (v == "min-content") { t.kind = Track::Kind::MinContent; return true; }
    if (v == "max-content") { t.kind = Track::Kind::MaxContent; return true; }
    if (v.ends_with("fr")) {
        float n;
        if (!parse_number(v.substr(0, v.size() - 2), n) || n < 0) return false;
        t.kind = Track::Kind::Fr; t.fr = n;
        return true;
    }
    auto minmax_side = [&](const std::string& x, int& kind, Len& len, float& fr) {
        const std::string s = lower_ascii(trim_ws(x));
        if (s == "auto") { kind = 2; return true; }
        if (s == "min-content") { kind = 3; return true; }
        if (s == "max-content") { kind = 4; return true; }
        if (s.ends_with("fr")) { kind = 1; return parse_number(s.substr(0, s.size() - 2), fr); }
        kind = 0;
        return parse_length(s, env, len);
    };
    if (v.starts_with("minmax(") && v.back() == ')') {
        const auto args = split_commas(v.substr(7, v.size() - 8));
        if (args.size() != 2) return false;
        t.kind = Track::Kind::MinMax;
        float dummy = 0;
        return minmax_side(args[0], t.min_kind, t.min_len, dummy) &&
               minmax_side(args[1], t.max_kind, t.max_len, t.max_fr);
    }
    if (v.starts_with("fit-content(") && v.back() == ')') {   // ~ minmax(auto, len)
        Len l;
        if (!parse_length(v.substr(12, v.size() - 13), env, l)) return false;
        t.kind = Track::Kind::MinMax; t.min_kind = 2; t.max_kind = 0; t.max_len = l;
        return true;
    }
    Len l;
    if (!parse_length(v, env, l)) return false;
    t.kind = Track::Kind::Len; t.len = l;
    return true;
}

bool parse_tracks(const std::string& value, const Env& env, std::vector<Track>& out,
                  int* autorep = nullptr, std::vector<Track>* pattern = nullptr, size_t* pos = nullptr) {
    out.clear();
    if (autorep) *autorep = 0;
    const std::string lv = lower_ascii(trim_ws(value));
    if (lv == "none") return true;
    for (const auto& tok : tokens(value)) {
        const std::string t = lower_ascii(tok);
        if (t.empty() || t[0] == '[') continue;   // named lines
        if (t.starts_with("repeat(") && t.back() == ')') {
            const std::string inner = t.substr(7, t.size() - 8);
            const auto args = split_commas(inner);
            if (args.size() != 2) return false;
            std::vector<Track> pat;
            for (const auto& pt : tokens(args[1])) {
                if (pt[0] == '[') continue;
                Track tr;
                if (!parse_track(pt, env, tr)) return false;
                pat.push_back(tr);
            }
            if (args[0] == "auto-fill" || args[0] == "auto-fit") {
                if (!autorep || !pattern || !pos) return false;
                *autorep = args[0] == "auto-fill" ? 1 : 2;
                *pattern = pat;
                *pos = out.size();
            } else {
                float n;
                if (!parse_number(args[0], n) || n < 1 || n > 1000) return false;
                for (int i = 0; i < int(n); ++i) out.insert(out.end(), pat.begin(), pat.end());
            }
            continue;
        }
        Track tr;
        if (!parse_track(t, env, tr)) return false;
        out.push_back(tr);
    }
    return true;
}

bool parse_gridline(const Tokens& t, GridLine& g) {
    g = GridLine{};
    if (t.empty()) return false;
    if (t.size() == 1 && t[0] == "auto") return true;
    g.auto_ = false;
    for (size_t i = 0; i < t.size(); ++i) {
        float n;
        if (t[i] == "span") {
            if (i + 1 < t.size() && parse_number(t[i + 1], n)) { g.span = int(n); ++i; }
            else g.span = 1;
        } else if (parse_number(t[i], n)) {
            if (i > 0 && t[i - 1] == "span") continue;
            g.line = int(n);
        } else g.name = t[i];
    }
    return true;
}

// splits at '/' tokens
std::vector<Tokens> split_slash(const Tokens& t) {
    std::vector<Tokens> out(1);
    for (const auto& x : t) {
        if (x == "/") out.emplace_back();
        else out.back().push_back(x);
    }
    return out;
}

bool parse_position_pair(const Tokens& t, const Env& env, Len& x, Len& y) {
    auto kwpos = [](const std::string& w, Len& l) {
        if (w == "left" || w == "top") { l = Len::Pct(0); return true; }
        if (w == "right" || w == "bottom") { l = Len::Pct(100); return true; }
        if (w == "center") { l = Len::Pct(50); return true; }
        return false;
    };
    if (t.empty() || t.size() > 4) return false;
    auto is_v = [](const std::string& w) { return w == "top" || w == "bottom"; };
    auto is_h = [](const std::string& w) { return w == "left" || w == "right"; };
    if (t.size() == 1) {
        if (is_v(t[0])) { x = Len::Pct(50); return kwpos(t[0], y); }
        y = Len::Pct(50);
        if (kwpos(t[0], x)) return true;
        return parse_length(t[0], env, x);
    }
    if (t.size() == 2) {
        const bool swap = is_v(t[0]) || is_h(t[1]);
        const std::string& a = swap ? t[1] : t[0];
        const std::string& b = swap ? t[0] : t[1];
        if (!kwpos(a, x) && !parse_length(a, env, x)) return false;
        if (!kwpos(b, y) && !parse_length(b, env, y)) return false;
        return true;
    }
    // 3-4 values: keyword [offset] keyword [offset]
    Len* cur = nullptr;
    x = Len::Pct(50); y = Len::Pct(50);
    for (size_t i = 0; i < t.size(); ++i) {
        const std::string& w = t[i];
        if (is_h(w) || is_v(w) || w == "center") {
            cur = (w == "left" || w == "right") ? &x : (is_v(w) ? &y : (i == 0 ? &x : &y));
            kwpos(w, *cur);
        } else if (cur) {
            Len off;
            if (!parse_length(w, env, off)) return false;
            const bool from_end = (cur == &x ? t[i - 1] == "right" : t[i - 1] == "bottom");
            cur->px += from_end ? -off.px : off.px;
            cur->pct += from_end ? -off.pct : off.pct;
        } else return false;
    }
    return true;
}

// clip-path: <basic-shape> || <geometry-box> | none.  path()/url()/shape() are not supported (invalid).
bool parse_clip_path(const std::string& value, const Env& env, ClipPath& out) {
    out = ClipPath{};
    const auto toks = tokens(value);
    if (toks.empty() || toks.size() > 2) return false;
    bool have_shape = false, have_box = false;
    for (const auto& raw_tk : toks) {
        const std::string tk = lower_ascii(raw_tk);
        if (tk == "border-box" || tk == "padding-box" || tk == "content-box" || tk == "margin-box") {
            if (have_box) return false;
            have_box = true;
            out.box = tk == "border-box" ? 0 : tk == "padding-box" ? 1 : tk == "content-box" ? 2 : 3;
            continue;
        }
        if (tk == "fill-box" || tk == "stroke-box" || tk == "view-box") { if (have_box) return false; have_box = true; continue; }
        const size_t open = tk.find('(');
        if (have_shape || open == std::string::npos || tk.back() != ')') return false;
        have_shape = true;
        const std::string fn = tk.substr(0, open);
        const std::string arg = trim_ws(tk.substr(open + 1, tk.size() - open - 2));
        Tokens t = tokens(arg);
        if (fn == "inset") {
            out.kind = ClipPath::Kind::Inset;
            size_t i = 0;
            std::vector<Len> e;
            for (; i < t.size() && t[i] != "round"; ++i) { Len l; if (!parse_length(t[i], env, l)) return false; e.push_back(l); }
            if (e.empty() || e.size() > 4) return false;
            const size_t n = e.size();   // top right bottom left, the border-width shorthand expansion
            out.edge[0] = e[0]; out.edge[1] = e[n > 1 ? 1 : 0]; out.edge[2] = e[n > 2 ? 2 : 0]; out.edge[3] = e[n > 3 ? 3 : n > 1 ? 1 : 0];
            for (int k = 0; k < 4; ++k) { out.rx[k] = out.ry[k] = Len::Px(0); }
            if (i < t.size()) {
                std::vector<Len> h, v;
                bool slash = false;
                for (++i; i < t.size(); ++i) {
                    if (t[i] == "/") { if (slash) return false; slash = true; continue; }
                    Len l;
                    if (!parse_length(t[i], env, l) || l.px < 0 || l.pct < 0) return false;
                    (slash ? v : h).push_back(l);
                }
                if (h.empty() || h.size() > 4 || (slash && (v.empty() || v.size() > 4))) return false;
                if (!slash) v = h;
                auto expand = [](const std::vector<Len>& a, Len* o) {
                    const size_t m = a.size();
                    o[0] = a[0]; o[1] = a[m > 1 ? 1 : 0]; o[2] = a[m > 2 ? 2 : 0]; o[3] = a[m > 3 ? 3 : m > 1 ? 1 : 0];
                };
                expand(h, out.rx); expand(v, out.ry);
            }
            continue;
        }
        if (fn == "circle" || fn == "ellipse") {
            const bool circ = fn == "circle";
            out.kind = circ ? ClipPath::Kind::Circle : ClipPath::Kind::Ellipse;
            size_t at = t.size();
            for (size_t i = 0; i < t.size(); ++i) if (t[i] == "at") { at = i; break; }
            const size_t nr = circ ? 1 : 2;
            if (at != 0 && at != nr) return false;
            for (size_t i = 0; i < at; ++i) {
                if (t[i] == "closest-side") out.rkw[i] = 1;
                else if (t[i] == "farthest-side") out.rkw[i] = 2;
                else if (!parse_length(t[i], env, out.r[i]) || out.r[i].px < 0 || out.r[i].pct < 0) return false;
            }
            if (at == 0) out.rkw[0] = out.rkw[1] = 1;   // default: closest-side
            if (at < t.size()) {
                Tokens pos(t.begin() + at + 1, t.end());
                if (!parse_position_pair(pos, env, out.cx, out.cy)) return false;
            }
            continue;
        }
        if (fn == "polygon") {
            out.kind = ClipPath::Kind::Polygon;
            for (const auto& item : split_commas(arg)) {
                Tokens p = tokens(item);
                if (p.size() == 1 && out.pts.empty() && (p[0] == "nonzero" || p[0] == "evenodd")) { out.evenodd = p[0] == "evenodd"; continue; }
                Len x, y;
                if (p.size() != 2 || !parse_length(p[0], env, x) || !parse_length(p[1], env, y)) return false;
                out.pts.emplace_back(x, y);
            }
            if (out.pts.empty()) return false;
            continue;
        }
        return false;
    }
    if (!have_shape) {   // a lone <geometry-box> clips to that box's edge
        out.kind = ClipPath::Kind::Inset;
        for (auto& l : out.edge) l = Len::Px(0);
        for (int k = 0; k < 4; ++k) out.rx[k] = out.ry[k] = Len::Px(0);
    }
    return true;
}

void set_list_style(ComputedStyle& s, const std::string& v, bool& ok) {
    if (v == "disc") s.list_style = ListStyle::Disc;
    else if (v == "circle") s.list_style = ListStyle::Circle;
    else if (v == "square") s.list_style = ListStyle::Square;
    else if (v == "decimal") s.list_style = ListStyle::Decimal;
    else if (v == "decimal-leading-zero") s.list_style = ListStyle::DecimalLeadingZero;
    else if (v == "lower-alpha" || v == "lower-latin") s.list_style = ListStyle::LowerAlpha;
    else if (v == "upper-alpha" || v == "upper-latin") s.list_style = ListStyle::UpperAlpha;
    else if (v == "lower-roman") s.list_style = ListStyle::LowerRoman;
    else if (v == "upper-roman") s.list_style = ListStyle::UpperRoman;
    else if (v == "none") s.list_style = ListStyle::None;
    else ok = false;
}

float parse_len_px(const std::string& v, const Env& env, bool& ok, float fallback = 0) {
    Len l;
    if (!parse_length(v, env, l) || l.has_pct()) { ok = false; return fallback; }
    return l.px;
}

// background layer from the shorthand tokens
bool parse_bg_layer(const Tokens& t, const Env& env, uint32_t current, BgLayer& L, uint32_t* color) {
    L = BgLayer{};
    size_t i = 0;
    Tokens pos_tokens, size_tokens;
    bool after_slash = false;
    for (; i < t.size(); ++i) {
        const std::string w = lower_ascii(t[i]);
        if (w == "/") { after_slash = true; continue; }
        if (after_slash) {
            size_tokens.push_back(w);
            if (size_tokens.size() == 2 || one_of(w, {"cover", "contain"})) after_slash = false;
            continue;
        }
        uint32_t c; bool isc = false;
        if (w.starts_with("url(")) {
            std::string u = trim_ws(t[i].substr(4, t[i].size() - 5));
            if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u = u.substr(1);
            if (!u.empty() && (u.back() == '"' || u.back() == '\'')) u.pop_back();
            L.kind = BgLayer::Kind::Url; L.url = u;
        } else if (w.starts_with("image-set(") || w.starts_with("-webkit-image-set(")) {
            // image-set(url(a.png) 1x, "b.png" 2x): the smallest candidate sharp enough for this display
            const size_t open = t[i].find('(');
            const size_t close = t[i].rfind(')');
            if (close == std::string::npos || close <= open) return false;
            const float want = luxium::render::image_pixel_ratio();
            std::string best; float best_d = 0;
            for (const std::string& cand : split_commas(t[i].substr(open + 1, close - open - 1))) {
                std::string c = trim_ws(cand), u; float d = 1.f;
                if (c.starts_with("url(")) {
                    const size_t e = c.find(')');
                    if (e == std::string::npos) continue;
                    u = trim_ws(c.substr(4, e - 4));
                    c = trim_ws(c.substr(e + 1));
                } else if (!c.empty() && (c[0] == '"' || c[0] == '\'')) {
                    const size_t e = c.find(c[0], 1);
                    if (e == std::string::npos) continue;
                    u = c.substr(0, e + 1);
                    c = trim_ws(c.substr(e + 1));
                } else continue;
                if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u = u.substr(1);
                if (!u.empty() && (u.back() == '"' || u.back() == '\'')) u.pop_back();
                if (!c.empty() && c.back() == 'x') d = std::strtof(c.c_str(), nullptr);
                if (d <= 0) d = 1.f;
                const bool sharp = d >= want, best_sharp = best_d >= want;
                if (best.empty() || (sharp && (!best_sharp || d < best_d)) || (!sharp && !best_sharp && d > best_d)) { best = u; best_d = d; }
            }
            if (best.empty()) return false;
            L.kind = BgLayer::Kind::Url; L.url = best;
        } else if (w.find("gradient(") != std::string::npos) {
            L.kind = BgLayer::Kind::Gradient;
            if (!parse_gradient(t[i], env, current, L.grad)) return false;
        } else if (w == "none") {
        } else if (w == "repeat-x") { L.repeat_x = BgRepeat::Repeat; L.repeat_y = BgRepeat::NoRepeat; }
        else if (w == "repeat-y") { L.repeat_x = BgRepeat::NoRepeat; L.repeat_y = BgRepeat::Repeat; }
        else if (one_of(w, {"repeat", "no-repeat", "space", "round"})) {
            const BgRepeat r = w == "repeat" ? BgRepeat::Repeat : w == "no-repeat" ? BgRepeat::NoRepeat
                               : w == "space" ? BgRepeat::Space : BgRepeat::Round;
            if (i + 1 < t.size() && one_of(lower_ascii(t[i + 1]), {"repeat", "no-repeat", "space", "round"})) {
                const std::string w2 = lower_ascii(t[++i]);
                L.repeat_x = r;
                L.repeat_y = w2 == "repeat" ? BgRepeat::Repeat : w2 == "no-repeat" ? BgRepeat::NoRepeat
                             : w2 == "space" ? BgRepeat::Space : BgRepeat::Round;
            } else L.repeat_x = L.repeat_y = r;
        } else if (one_of(w, {"scroll", "fixed", "local", "border-box", "content-box"})) {
        } else if (w == "padding-box") { L.clip_padding = true; }
        else if (parse_color(w, c, &isc)) { if (color) *color = isc ? current : c; }
        else pos_tokens.push_back(w);   // position keywords/lengths
    }
    if (!pos_tokens.empty() && !parse_position_pair(pos_tokens, env, L.pos_x, L.pos_y)) return false;
    if (!size_tokens.empty()) {
        if (size_tokens[0] == "cover") L.size_mode = BgSizeMode::Cover;
        else if (size_tokens[0] == "contain") L.size_mode = BgSizeMode::Contain;
        else {
            L.size_mode = BgSizeMode::Explicit;
            if (!parse_length(size_tokens[0], env, L.size_w, true)) return false;
            L.size_h = Len::Auto();
            if (size_tokens.size() > 1 && !parse_length(size_tokens[1], env, L.size_h, true)) return false;
        }
    }
    return true;
}

int weight_from(const std::string& v, int parent, bool& ok) {
    if (v == "normal") return 400;
    if (v == "bold") return 700;
    if (v == "bolder") return parent < 350 ? 400 : parent < 550 ? 700 : 900;
    if (v == "lighter") return parent < 550 ? 100 : parent < 750 ? 400 : 700;
    float n;
    if (parse_number(v, n) && n >= 1 && n <= 1000) return int(n);
    ok = false;
    return 400;
}

bool is_mono_name(const std::string& n) {
    return one_of(n, {"monospace", "courier", "courier new", "consolas", "monaco", "menlo", "dejavu sans mono",
                      "lucida console", "ui-monospace", "sfmono-regular", "liberation mono", "source code pro",
                      "fira code", "fira mono", "jetbrains mono", "roboto mono", "ubuntu mono", "andale mono"});
}
bool is_serif_name(const std::string& n) {
    return one_of(n, {"serif", "georgia", "times", "times new roman", "palatino", "garamond", "cambria",
                      "book antiqua", "dejavu serif", "liberation serif", "ui-serif"});
}

void set_family(ComputedStyle& s, const std::string& value) {
    s.family_name = value;
    s.family = FontFamily::Sans;
    for (auto name : split_commas(value)) {
        name = lower_ascii(name);
        if (!name.empty() && (name.front() == '"' || name.front() == '\'')) name = name.substr(1);
        if (!name.empty() && (name.back() == '"' || name.back() == '\'')) name.pop_back();
        // first family we can honour decides: our faces are DejaVu Sans/Mono
        if (is_mono_name(name)) { s.family = FontFamily::Mono; return; }
        if (name == "dejavu sans" || name == "sans-serif" || name == "arial" || name == "helvetica" ||
            name == "verdana" || name == "system-ui" || name == "-apple-system" || name == "segoe ui" ||
            name == "roboto" || name == "inter" || name == "ubuntu" || name == "liberation sans" ||
            name == "blinkmacsystemfont" || name == "tahoma" || name == "trebuchet ms" || name == "open sans") {
            s.family = FontFamily::Sans; return;
        }
        if (is_serif_name(name)) { s.family = FontFamily::Serif; return; }
    }
}

bool parse_font_size(const std::string& v, const Env& env, float& out) {
    const float parent_fs = env.parent ? env.parent->font_size : 16.f;
    static const std::unordered_map<std::string, float> kw = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18},
        {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
    auto it = kw.find(v);
    if (it != kw.end()) { out = it->second; return true; }
    if (v == "larger") { out = parent_fs * 1.2f; return true; }
    if (v == "smaller") { out = parent_fs / 1.2f; return true; }
    Len l;
    if (!parse_length(v, env, l)) return false;
    out = l.px + l.pct * parent_fs / 100.f;
    return out >= 0;
}

bool apply_text_decoration_line(ComputedStyle& s, const std::string& v, const Env&) {
    if (v == "none") { s.underline = s.line_through = s.overline = false; return true; }
    bool u = false, l = false, o = false;
    for (const auto& t : tokens(v)) {
        if (t == "underline") u = true;
        else if (t == "line-through") l = true;
        else if (t == "overline") o = true;
        else if (t == "blink" || t == "solid" || t == "double" || t == "dotted" || t == "dashed" || t == "wavy") {}
        else return false;
    }
    s.underline = u; s.line_through = l; s.overline = o;
    return true;
}

} // namespace

bool is_font_size_property(const std::string& n) { return n == "font-size" || n == "font"; }

bool property_inherits(const std::string& n) {
    static const std::unordered_set<std::string> inh = {
        "color", "font-size", "font-weight", "font-style", "font-family", "font", "line-height",
        "text-align", "text-transform", "letter-spacing", "word-spacing", "text-indent", "white-space",
        "visibility", "cursor", "list-style", "list-style-type", "list-style-position", "list-style-image", "border-collapse",
        "border-spacing", "overflow-wrap", "word-wrap", "word-break", "pointer-events", "text-decoration-color", "accent-color", "text-shadow"};
    return inh.count(n) > 0;
}

namespace {

// Logical properties for horizontal-tb / ltr: inline = left/right, block = top/bottom.  Returns true (and the
// physical declarations) when `name` is one of them.
bool expand_logical(const std::string& name, const std::string& value, std::vector<std::pair<std::string, std::string>>& out) {
    struct Ax { const char* inline_start; const char* inline_end; const char* block_start; const char* block_end; };
    auto two = [&](const char* a, const char* b) {   // "x" or "x y" -> a gets x, b gets y (or x)
        const auto t = tokens(value);
        if (t.empty() || t.size() > 2) return false;
        out.emplace_back(a, t[0]);
        out.emplace_back(b, t.size() == 2 ? t[1] : t[0]);
        return true;
    };
    static const std::vector<std::pair<std::string, std::string>> kSize = {
        {"inline-size", "width"}, {"block-size", "height"}, {"min-inline-size", "min-width"}, {"min-block-size", "min-height"},
        {"max-inline-size", "max-width"}, {"max-block-size", "max-height"}};
    for (const auto& [l, p] : kSize) if (name == l) { out.emplace_back(p, value); return true; }
    if (name == "overflow-inline") { out.emplace_back("overflow-x", value); return true; }
    if (name == "overflow-block") { out.emplace_back("overflow-y", value); return true; }
    static const std::pair<const char*, const char*> kCorner[] = {
        {"border-start-start-radius", "border-top-left-radius"}, {"border-start-end-radius", "border-top-right-radius"},
        {"border-end-start-radius", "border-bottom-left-radius"}, {"border-end-end-radius", "border-bottom-right-radius"}};
    for (const auto& [l, p] : kCorner) if (name == l) { out.emplace_back(p, value); return true; }

    // <prefix>-(inline|block)[-start|-end][-<suffix>]
    for (const std::string prefix : {"margin", "padding", "inset", "border", "scroll-margin", "scroll-padding"}) {
        if (!name.starts_with(prefix + "-inline") && !name.starts_with(prefix + "-block")) continue;
        const bool inl = name.compare(prefix.size() + 1, 6, "inline") == 0;
        std::string rest = name.substr(prefix.size() + 1 + (inl ? 6 : 5));   // "", "-start", "-end", "-width", "-start-width"...
        std::string edge;                                                      // "", start, end
        if (rest.starts_with("-start")) { edge = "start"; rest = rest.substr(6); }
        else if (rest.starts_with("-end")) { edge = "end"; rest = rest.substr(4); }
        const char* first = inl ? "left" : "top";
        const char* last = inl ? "right" : "bottom";
        auto phys = [&](const char* side) {
            if (prefix == "inset") return std::string(side);
            if (prefix == "border") return "border-" + std::string(side) + rest;           // border-left, border-left-width
            return prefix + "-" + side;
        };
        if (prefix == "inset" && !rest.empty()) return false;
        if (prefix != "border" && !rest.empty()) return false;
        if (edge.empty()) {   // both edges: one value or two
            if (prefix == "border" && rest.empty()) {   // border-inline: width style color -> both sides
                out.emplace_back(phys(first), value);
                out.emplace_back(phys(last), value);
                return true;
            }
            return two(phys(first).c_str(), phys(last).c_str()) ? true : false;
        }
        out.emplace_back(phys(edge == "start" ? first : last), value);
        return true;
    }
    return false;
}

} // namespace

bool apply_property(ComputedStyle& s, const Env& env, const std::string& name, const std::string& raw0) {
    if (name == "-webkit-background-clip") return apply_property(s, env, "background-clip", raw0);
    if (name.find("-inline") != std::string::npos || name.find("-block") != std::string::npos ||
        name.ends_with("-size") || name.starts_with("border-start-") || name.starts_with("border-end-") || name.starts_with("overflow-in") || name.starts_with("overflow-bl")) {
        std::vector<std::pair<std::string, std::string>> phys;
        if (expand_logical(name, raw0, phys)) {
            bool any = false;
            for (const auto& [n, v] : phys) any = apply_property(s, env, n, v) || any;
            return any;
        }
    }
    // color-mix(... currentColor ...): the mix needs the value, so it is spelled out (for `color` itself this is
    // still the inherited colour, as the spec wants)
    std::string mixed;
    if (raw0.find("color-mix(") != std::string::npos) {
        const std::string l = lower_ascii(raw0);
        if (l.find("currentcolor") != std::string::npos) {
            char rgba[64];
            std::snprintf(rgba, sizeof rgba, "rgba(%u, %u, %u, %.4f)", (s.color >> 16) & 255, (s.color >> 8) & 255, s.color & 255, float(s.color >> 24) / 255.f);
            mixed = raw0;
            for (size_t p = l.find("currentcolor"), shift = 0; p != std::string::npos; p = l.find("currentcolor", p + 12)) {
                mixed.replace(p + shift, 12, rgba);
                shift += std::strlen(rgba) - 12;
            }
        }
    }
    const std::string& raw = mixed.empty() ? raw0 : mixed;
    const std::string v = lower_ascii(trim_ws(raw));
    const Tokens t = tokens(v);
    if (v.empty()) return false;
    bool ok = true;

    // ── display & positioning ──────────────────────────────────────────────
    if (name == "display") {
        static const std::unordered_map<std::string, Display> m = {
            {"none", Display::None}, {"contents", Display::Contents}, {"block", Display::Block},
            {"inline", Display::Inline}, {"inline-block", Display::InlineBlock}, {"flex", Display::Flex},
            {"inline-flex", Display::InlineFlex}, {"grid", Display::Grid}, {"inline-grid", Display::InlineGrid},
            {"table", Display::Table}, {"inline-table", Display::InlineTable}, {"table-row", Display::TableRow},
            {"table-cell", Display::TableCell}, {"table-row-group", Display::TableRowGroup},
            {"table-header-group", Display::TableHeaderGroup}, {"table-footer-group", Display::TableFooterGroup},
            {"table-column", Display::TableColumn}, {"table-column-group", Display::TableColumnGroup},
            {"table-caption", Display::TableCaption}, {"list-item", Display::ListItem},
            {"flow-root", Display::FlowRoot}};
        s.webkit_box = v == "-webkit-box" || v == "-webkit-inline-box";
        if (s.webkit_box) { s.display = v == "-webkit-box" ? Display::FlowRoot : Display::InlineBlock; return true; }
        auto it = m.find(v);
        if (it == m.end()) {
            // two-value syntax: "block flex", "inline flow-root" ...
            if (t.size() == 2) {
                const bool inl = t[0] == "inline";
                if (t[1] == "flex") { s.display = inl ? Display::InlineFlex : Display::Flex; return true; }
                if (t[1] == "grid") { s.display = inl ? Display::InlineGrid : Display::Grid; return true; }
                if (t[1] == "flow-root") { s.display = inl ? Display::InlineBlock : Display::FlowRoot; return true; }
                if (t[1] == "flow") { s.display = inl ? Display::Inline : Display::Block; return true; }
            }
            return false;
        }
        s.display = it->second;
        return true;
    }
    if (name == "position") {
        if (v == "static") s.position = Position::Static;
        else if (v == "relative") s.position = Position::Relative;
        else if (v == "absolute") s.position = Position::Absolute;
        else if (v == "fixed") s.position = Position::Fixed;
        else if (v == "sticky" || v == "-webkit-sticky") s.position = Position::Sticky;
        else return false;
        return true;
    }
    if (name == "float") {
        if (v == "none") s.float_ = Float::None;
        else if (v == "left" || v == "inline-start") s.float_ = Float::Left;
        else if (v == "right" || v == "inline-end") s.float_ = Float::Right;
        else return false;
        return true;
    }
    if (name == "clear") {
        if (v == "none") s.clear = Clear::None;
        else if (v == "left" || v == "inline-start") s.clear = Clear::Left;
        else if (v == "right" || v == "inline-end") s.clear = Clear::Right;
        else if (v == "both") s.clear = Clear::Both;
        else return false;
        return true;
    }
    if (name == "box-sizing") {
        if (v == "content-box") s.box_sizing = BoxSizing::Content;
        else if (v == "border-box") s.box_sizing = BoxSizing::Border;
        else return false;
        return true;
    }
    if (name == "overflow" || name == "overflow-x" || name == "overflow-y") {
        auto one = [&](const std::string& w, Overflow& o) {
            if (w == "visible") o = Overflow::Visible; else if (w == "hidden") o = Overflow::Hidden;
            else if (w == "scroll") o = Overflow::Scroll; else if (w == "auto") o = Overflow::Auto;
            else if (w == "clip") o = Overflow::Clip; else return false;
            return true;
        };
        Overflow a, b;
        if (name == "overflow-x") { if (!one(v, a)) return false; s.overflow_x = a; return true; }
        if (name == "overflow-y") { if (!one(v, a)) return false; s.overflow_y = a; return true; }
        if (t.empty() || t.size() > 2 || !one(t[0], a) || !one(t.back(), b)) return false;
        s.overflow_x = a; s.overflow_y = b;
        return true;
    }
    if (name == "visibility") {
        if (v == "visible") s.visibility = Visibility::Visible;
        else if (v == "hidden") s.visibility = Visibility::Hidden;
        else if (v == "collapse") s.visibility = Visibility::Collapse;
        else return false;
        return true;
    }
    if (name == "writing-mode") {
        if (v == "horizontal-tb" || v == "lr" || v == "lr-tb" || v == "rl" || v == "rl-tb") s.writing_mode = 0;
        else if (v == "vertical-rl" || v == "sideways-rl" || v == "tb" || v == "tb-rl") s.writing_mode = 1;
        else if (v == "vertical-lr" || v == "sideways-lr" || v == "tb-lr") s.writing_mode = 2;
        else return false;
        return true;
    }
    if (name == "text-orientation") return v == "mixed" || v == "upright" || v == "sideways" || v == "sideways-right" || v == "use-glyph-orientation";
    if (name == "direction") {
        if (v != "ltr" && v != "rtl") return false;
        s.rtl = v == "rtl";
        return true;
    }
    if (name == "unicode-bidi") return v == "normal" || v == "embed" || v == "isolate" || v == "bidi-override" || v == "isolate-override" || v == "plaintext";
    if (name == "mix-blend-mode" || name == "background-blend-mode") {
        static const char* const names[] = {"normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn",
                                            "hard-light", "soft-light", "difference", "exclusion"};
        auto code = [&](const std::string& w) { for (int i = 0; i < 12; ++i) if (w == names[i]) return i; return -1; };
        if (name == "mix-blend-mode") { const int c = code(v); if (c < 0) return false; s.mix_blend = c; return true; }
        std::vector<int> list;
        size_t from = 0;
        while (from <= v.size()) {
            size_t comma = v.find(',', from);
            if (comma == std::string::npos) comma = v.size();
            const size_t a = v.find_first_not_of(" \t", from), z = v.find_last_not_of(" \t", comma ? comma - 1 : 0);
            const int c = a == std::string::npos || a > z ? -1 : code(v.substr(a, z - a + 1));
            if (c < 0) return false;
            list.push_back(c);
            from = comma + 1;
        }
        s.bg_blend = list;
        return true;
    }
    if (name == "opacity") {
        float n;
        if (v.back() == '%') { if (!parse_number(v.substr(0, v.size() - 1), n)) return false; n /= 100.f; }
        else if (!parse_number(v, n)) return false;
        s.opacity = std::clamp(n, 0.f, 1.f);
        return true;
    }
    if (name == "z-index") {
        float n;
        if (v == "auto") { s.z_auto = true; s.z_index = 0; return true; }
        if (!parse_number(v, n)) return false;
        s.z_auto = false; s.z_index = int(n);
        return true;
    }

    // ── box ────────────────────────────────────────────────────────────────
    if (name == "width" || name == "height" || name == "min-width" || name == "min-height" ||
        name == "max-width" || name == "max-height") {
        Len l;
        const bool is_min = name.starts_with("min-"), is_max = name.starts_with("max-");
        if (v == "auto" || v == "none" || v == "fit-content" || v == "max-content" || v == "min-content" ||
            v == "-webkit-fill-available" || v == "stretch") {
            if (is_min) l = Len::Px(0); else l = Len::Auto();
            if (is_max && v != "none") l = Len::Auto();
            if (name == "width" || name == "height") l.kw = v == "fit-content" ? 1 : v == "max-content" ? 2 : v == "min-content" ? 3 : 0;   // sized by content
        } else {
            if (!parse_length(v, env, l)) return false;
            if (l.px < 0 && !l.has_pct()) return false;
        }
        Len& dst = name == "width" ? s.width : name == "height" ? s.height :
                   name == "min-width" ? s.min_width : name == "min-height" ? s.min_height :
                   name == "max-width" ? s.max_width : s.max_height;
        dst = l;
        return true;
    }
    if (name == "margin") {
        Len e[4];
        if (!parse_edges(t, env, e, true, false)) return false;
        for (int i = 0; i < 4; ++i) s.margin[i] = e[i];
        return true;
    }
    if (name == "padding") {
        Len e[4];
        if (!parse_edges(t, env, e, false, true)) return false;
        for (int i = 0; i < 4; ++i) s.padding[i] = e[i];
        return true;
    }
    if (name.starts_with("margin-") || name.starts_with("padding-")) {
        const bool mar = name[0] == 'm';
        int si = side_index(name.substr(mar ? 7 : 8));
        if (si < 0) {   // logical properties (LTR)
            const std::string x = name.substr(mar ? 7 : 8);
            si = x == "inline-start" ? 3 : x == "inline-end" ? 1 : x == "block-start" ? 0 : x == "block-end" ? 2 : -1;
        }
        if (si < 0) return false;
        Len l;
        if (!parse_length(v, env, l, mar)) return false;
        if (!mar && (l.px < 0 || l.pct < 0)) return false;
        (mar ? s.margin : s.padding)[si] = l;
        return true;
    }
    if (name == "inset" || name == "top" || name == "right" || name == "bottom" || name == "left") {
        if (name == "inset") {
            Len e[4];
            if (!parse_edges(t, env, e, true, false)) return false;
            for (int i = 0; i < 4; ++i) s.inset[i] = e[i];
            return true;
        }
        Len l;
        if (!parse_length(v, env, l, true)) return false;
        s.inset[side_index(name)] = l;
        return true;
    }

    // borders
    if (name == "border" || name == "border-top" || name == "border-right" || name == "border-bottom" ||
        name == "border-left") {
        float w = 3; BorderStyle bs = BorderStyle::None; std::string coltok;
        bool have_w = false, have_s = false, have_c = false;
        if (v == "none" || v == "0") { bs = BorderStyle::None; w = 0; have_w = have_s = true; }
        else
            for (const auto& tk : t) {
                BorderStyle b2; float w2; bool cok;
                if (parse_bstyle(tk, b2)) { bs = b2; have_s = true; }
                else if (parse_bwidth(tk, env, w2)) { w = w2; have_w = true; }
                else { uint32_t c; if (!parse_color(tk, c)) return false; coltok = tk; have_c = true; (void)cok; }
            }
        (void)have_w; (void)have_s;
        for (int i = 0; i < 4; ++i) {
            if (name != "border" && side_index(name.substr(7)) != i) continue;
            s.border_w[i] = w; s.border_style[i] = bs;
            if (have_c) set_bcolor(s, i, coltok); else { s.bc_current[i] = true; }
        }
        return true;
    }
    if (name == "border-width" || name == "border-style" || name == "border-color") {
        if (t.empty() || t.size() > 4) return false;
        for (int i = 0; i < 4; ++i) {
            // top right bottom left from 1..4 values
            const size_t idx = t.size() == 1 ? 0 : t.size() == 2 ? (i % 2) : t.size() == 3 ? (i == 3 ? 1 : i) : i;
            const std::string& tk = t[idx];
            if (name == "border-width") { float w; if (!parse_bwidth(tk, env, w)) return false; s.border_w[i] = w; }
            else if (name == "border-style") { BorderStyle b; if (!parse_bstyle(tk, b)) return false; s.border_style[i] = b; }
            else if (!set_bcolor(s, i, tk)) return false;
        }
        return true;
    }
    if (name.starts_with("border-") && (name.ends_with("-width") || name.ends_with("-style") || name.ends_with("-color"))) {
        std::string mid = name.substr(7, name.size() - 7 - (name.ends_with("-width") ? 6 : name.ends_with("-style") ? 6 : 6));
        int si = side_index(mid);
        if (si < 0) return false;   // border-top-left-radius handled below
        if (name.ends_with("-width")) { float w; if (!parse_bwidth(v, env, w)) return false; s.border_w[si] = w; }
        else if (name.ends_with("-style")) { BorderStyle b; if (!parse_bstyle(v, b)) return false; s.border_style[si] = b; }
        else if (!set_bcolor(s, si, v)) return false;
        return true;
    }
    if (name == "border-radius") {
        const auto parts = split_slash(t);
        Len e[4];
        if (!parse_edges(parts[0], env, e, false, true)) return false;
        // order: tl tr br bl  (same expansion as margin: top=tl right=tr bottom=br left=bl)
        for (int i = 0; i < 4; ++i) { s.radius[i] = e[i]; s.radiusv[i] = e[i]; }
        s.radius_ell = false;
        if (parts.size() > 1) {
            Len v[4];
            if (!parse_edges(parts[1], env, v, false, true)) return false;
            for (int i = 0; i < 4; ++i) s.radiusv[i] = v[i];
            s.radius_ell = true;
        }
        return true;
    }
    if (name == "border-top-left-radius" || name == "border-top-right-radius" ||
        name == "border-bottom-right-radius" || name == "border-bottom-left-radius") {
        Len l, lv;
        if (t.size() > 2 || !parse_length(t[0], env, l)) return false;
        if (t.size() == 2) { if (!parse_length(t[1], env, lv)) return false; } else lv = l;
        const int i = name == "border-top-left-radius" ? 0 : name == "border-top-right-radius" ? 1 :
                      name == "border-bottom-right-radius" ? 2 : 3;
        s.radius[i] = l; s.radiusv[i] = lv;
        if (t.size() == 2) s.radius_ell = true;
        return true;
    }

    // ── painting ───────────────────────────────────────────────────────────
    if (name == "background-color") {
        bool cok;
        const uint32_t c = color_or(v, s.color, cok);
        if (!cok) return false;
        s.bg_color = c;
        return true;
    }
    if (name == "border-image" || name == "border-image-source" || name == "border-image-slice" || name == "border-image-width" ||
        name == "border-image-outset" || name == "border-image-repeat") {
        const bool all = name == "border-image";
        BorderImage bi = s.border_img;
        if (all) bi = BorderImage{};
        std::string text = raw;
        {   // a `/` outside parentheses separates slice / width / outset; inside (url(a/b), calc()) it is part of the value
            std::string sp; int depth = 0;
            for (char ch : text) {
                if (ch == '(') ++depth; else if (ch == ')') --depth;
                if (ch == '/' && depth == 0) sp += " / "; else sp += ch;
            }
            text = sp;
        }
        int stage = name == "border-image-width" ? 1 : name == "border-image-outset" ? 2 : 0;
        std::vector<std::string> nums;   // the numbers of the current stage
        auto flush = [&]() -> bool {
            if (stage == 0 && !nums.empty() && nums.size() <= 4) {
                for (int i = 0; i < 4; ++i) {
                    const std::string& w = nums[i < int(nums.size()) ? (size_t)i : (nums.size() == 1 ? 0 : nums.size() == 2 ? (size_t)(i % 2) : nums.size() == 3 ? (size_t)(i == 3 ? 1 : i) : (size_t)i)];
                    bi.slice_pct[i] = w.back() == '%';
                    char* e = nullptr; bi.slice[i] = std::strtof(w.c_str(), &e);
                    if (*e && *e != '%') return false;
                }
            } else if (stage == 1 && !nums.empty() && nums.size() <= 4) {
                for (int i = 0; i < 4; ++i) {
                    const std::string& w = nums[i < int(nums.size()) ? (size_t)i : (nums.size() == 1 ? 0 : nums.size() == 2 ? (size_t)(i % 2) : nums.size() == 3 ? (size_t)(i == 3 ? 1 : i) : (size_t)i)];
                    bi.width_auto[i] = w == "auto";
                    if (w == "auto") continue;
                    char* e = nullptr; const float n = std::strtof(w.c_str(), &e);
                    if (!*e) { bi.width[i] = n; bi.width_num[i] = true; }
                    else { Len l; if (!parse_length(w, env, l)) return false; bi.width[i] = l.px; bi.width_num[i] = false; }
                }
            }
            nums.clear();
            return true;
        };
        for (const auto& tk : tokens(text)) {
            const std::string w = lower_ascii(tk);
            if (w == "/") { if (!flush()) return false; ++stage; continue; }
            if (w == "fill") { bi.fill = true; continue; }
            if (w == "stretch" || w == "repeat" || w == "round" || w == "space") continue;   // edges always stretch
            if (w.starts_with("url(") || w.find("gradient(") != std::string::npos || w == "none") {
                if (w == "none") { bi.src = BgLayer{}; bi.set = false; continue; }
                BgLayer L;
                if (!parse_bg_layer(Tokens{tk}, env, s.color, L, nullptr)) return false;
                bi.src = L; bi.set = true;
                continue;
            }
            nums.push_back(w);
        }
        if (!flush()) return false;
        s.border_img = bi;
        return true;
    }
    if (name == "background" || name == "background-image") {
        std::vector<BgLayer> layers;
        uint32_t color = s.bg_color;
        bool color_set = false;
        const auto items = split_commas(raw);
        for (size_t i = 0; i < items.size(); ++i) {
            BgLayer L;
            uint32_t c = 0;
            if (name == "background-image") {
                const std::string w = lower_ascii(trim_ws(items[i]));
                if (w == "none") { layers.push_back(L); continue; }
                Tokens one{trim_ws(items[i])};
                if (!parse_bg_layer(one, env, s.color, L, nullptr)) return false;
                layers.push_back(L);
                continue;
            }
            if (!parse_bg_layer(tokens(items[i]), env, s.color, L, &c)) return false;
            if (i + 1 == items.size()) {   // the color belongs to the last layer
                uint32_t tmp; bool isc = false;
                for (const auto& tk : tokens(lower_ascii(items[i])))
                    if (parse_color(tk, tmp, &isc)) { color = isc ? s.color : tmp; color_set = true; }
            }
            layers.push_back(L);
        }
        if (name == "background") { s.bg_color = color_set ? color : kTransparent; }
        // keep other per-layer properties set earlier when only the image changes
        if (name == "background-image") {
            for (size_t i = 0; i < layers.size(); ++i) {
                if (i < s.bg.size()) {
                    layers[i].repeat_x = s.bg[i].repeat_x; layers[i].repeat_y = s.bg[i].repeat_y;
                    layers[i].pos_x = s.bg[i].pos_x; layers[i].pos_y = s.bg[i].pos_y;
                    layers[i].size_mode = s.bg[i].size_mode;
                    layers[i].size_w = s.bg[i].size_w; layers[i].size_h = s.bg[i].size_h;
                }
            }
        }
        s.bg = layers;
        return true;
    }
    if (name == "background-repeat" || name == "background-position" || name == "background-size" ||
        name == "background-clip" || name == "background-origin" || name == "background-attachment") {
        if (name == "background-clip" || name == "background-origin" || name == "background-attachment") {
            if (name == "background-clip") {
                s.bg_clip_text = v.find("text") != std::string::npos;
                for (auto& L : s.bg) L.clip_padding = v.find("padding-box") != std::string::npos;
            }
            return true;
        }
        const auto items = split_commas(v);
        if (s.bg.size() < items.size()) s.bg.resize(items.size());
        for (size_t i = 0; i < s.bg.size(); ++i) {
            const Tokens it = tokens(items[i % items.size()]);
            BgLayer& L = s.bg[i];
            if (name == "background-repeat") {
                BgLayer tmp;
                Tokens x = it;
                if (!parse_bg_layer(x, env, s.color, tmp, nullptr)) return false;
                L.repeat_x = tmp.repeat_x; L.repeat_y = tmp.repeat_y;
            } else if (name == "background-position") {
                if (!parse_position_pair(it, env, L.pos_x, L.pos_y)) return false;
            } else {
                if (it.empty()) return false;
                if (it[0] == "cover") L.size_mode = BgSizeMode::Cover;
                else if (it[0] == "contain") L.size_mode = BgSizeMode::Contain;
                else {
                    L.size_mode = BgSizeMode::Explicit;
                    if (!parse_length(it[0], env, L.size_w, true)) return false;
                    L.size_h = Len::Auto();
                    if (it.size() > 1 && !parse_length(it[1], env, L.size_h, true)) return false;
                }
            }
        }
        return true;
    }
    if (name == "box-shadow") {
        std::vector<Shadow> out;
        if (!parse_shadow_list(raw, env, s.color, out)) return false;
        s.box_shadow = out;
        return true;
    }
    if (name == "text-shadow") {
        std::vector<Shadow> out;
        if (v != "none" && !parse_shadow_list(raw, env, s.color, out)) return false;
        for (const auto& sh : out) if (sh.inset || sh.spread != 0) return false;   // text-shadow has neither
        s.text_shadow = out;
        return true;
    }
    if (name == "shape-margin") { Len l; if (!parse_length(v, env, l)) return false; s.shape_margin = l; return true; }
    if (name == "shape-outside") {
        if (v == "none") { s.shape_outside = ClipPath{}; return true; }
        ClipPath cp;
        if (v == "margin-box" || v == "border-box" || v == "padding-box" || v == "content-box") {   // the box itself (its rounded corners are not followed)
            cp.kind = ClipPath::Kind::Inset;
            cp.box = v == "border-box" ? 0 : v == "padding-box" ? 1 : v == "content-box" ? 2 : 3;
            s.shape_outside = cp;
            return true;
        }
        if (v.starts_with("url(") || v.find("gradient(") != std::string::npos) return true;   // image shapes: accepted, the float stays a box
        if (!parse_clip_path(raw, env, cp)) return false;
        if (v.find("-box") == std::string::npos) cp.box = 3;   // the reference box of shape-outside is the margin box
        s.shape_outside = cp;
        return true;
    }
    if (name == "clip-path") {
        if (v == "none") { s.clip_path = ClipPath{}; return true; }
        ClipPath cp;
        if (!parse_clip_path(raw, env, cp)) return false;
        s.clip_path = cp;
        return true;
    }
    if (name == "filter" || name == "backdrop-filter") {   // -webkit-backdrop-filter is Safari-only: Chromium ignores it
        std::vector<FilterFn> out;
        if (v != "none" && !parse_filter_list(raw, env, s.color, out)) return false;
        (name == "filter" ? s.filter : s.backdrop_filter) = out;
        return true;
    }
    if (name == "mask-image" || name == "-webkit-mask-image" || name == "mask" || name == "-webkit-mask") {
        std::vector<BgLayer> layers;
        bool any = false;
        for (const auto& item : split_commas(raw)) {
            BgLayer L;
            L.repeat_x = L.repeat_y = BgRepeat::Repeat;
            if (lower_ascii(trim_ws(item)) == "none") { layers.push_back(L); continue; }
            Tokens tk = name.ends_with("mask") ? tokens(item) : Tokens{trim_ws(item)};
            if (!parse_bg_layer(tk, env, s.color, L, nullptr)) return false;
            any = any || L.kind != BgLayer::Kind::None;
            layers.push_back(L);
        }
        s.mask = any ? layers : std::vector<BgLayer>{};
        return true;
    }
    if (name == "mask-size" || name == "-webkit-mask-size" || name == "mask-position" || name == "-webkit-mask-position" ||
        name == "mask-repeat" || name == "-webkit-mask-repeat") {
        const auto items = split_commas(v);
        for (size_t i = 0; i < s.mask.size(); ++i) {
            const Tokens it = tokens(items[i % items.size()]);
            BgLayer& L = s.mask[i];
            if (name.ends_with("repeat")) {
                BgLayer tmp;
                Tokens x = it;
                if (!parse_bg_layer(x, env, s.color, tmp, nullptr)) return false;
                L.repeat_x = tmp.repeat_x; L.repeat_y = tmp.repeat_y;
            } else if (name.ends_with("position")) {
                if (!parse_position_pair(it, env, L.pos_x, L.pos_y)) return false;
            } else {
                if (it.empty()) return false;
                if (it[0] == "cover") L.size_mode = BgSizeMode::Cover;
                else if (it[0] == "contain") L.size_mode = BgSizeMode::Contain;
                else {
                    L.size_mode = BgSizeMode::Explicit;
                    if (!parse_length(it[0], env, L.size_w, true)) return false;
                    L.size_h = Len::Auto();
                    if (it.size() > 1 && !parse_length(it[1], env, L.size_h, true)) return false;
                }
            }
        }
        return true;
    }
    if (name == "-webkit-line-clamp") {
        if (v == "none") { s.line_clamp = 0; return true; }
        float n;
        if (!parse_number(v, n) || n < 1 || n != std::floor(n)) return false;
        s.line_clamp = int(n);
        return true;
    }
    if (name == "-webkit-box-orient") { s.box_orient_v = v == "vertical" || v == "block-axis"; return true; }
    if (name == "object-position") return parse_position_pair(t, env, s.object_pos_x, s.object_pos_y);
    if (name == "object-fit") {
        if (v == "fill") s.object_fit = ObjectFit::Fill; else if (v == "contain") s.object_fit = ObjectFit::Contain;
        else if (v == "cover") s.object_fit = ObjectFit::Cover; else if (v == "none") s.object_fit = ObjectFit::None;
        else if (v == "scale-down") s.object_fit = ObjectFit::ScaleDown; else return false;
        return true;
    }

    // ── text ───────────────────────────────────────────────────────────────
    if (name == "color") {
        bool cok;
        const uint32_t c = color_or(v, s.color, cok);
        if (!cok) return false;
        s.color = c;
        return true;
    }
    if (name == "font-size") {
        float px;
        if (!parse_font_size(v, env, px)) return false;
        s.font_size = px;
        return true;
    }
    if (name == "font-weight") {
        const int w = weight_from(v, env.parent ? env.parent->font_weight : 400, ok);
        if (!ok) return false;
        s.font_weight = w;
        return true;
    }
    if (name == "font-style") {
        if (v == "normal") s.italic = false; else if (v == "italic" || v.starts_with("oblique")) s.italic = true;
        else return false;
        return true;
    }
    if (name == "font-family") { set_family(s, raw); return true; }
    if (name == "font") {
        if (one_of(v, {"caption", "icon", "menu", "message-box", "small-caption", "status-bar"})) return true;
        // [style|variant|weight|stretch]* size[/line-height] family
        size_t i = 0;
        bool italic = false; int weight = 400;
        for (; i < t.size(); ++i) {
            bool wok = true;
            if (t[i] == "normal") continue;
            if (t[i] == "italic" || t[i] == "oblique") { italic = true; continue; }
            if (t[i] == "small-caps") continue;
            const int w = weight_from(t[i], env.parent ? env.parent->font_weight : 400, wok);
            if (wok && (t[i] == "bold" || t[i] == "bolder" || t[i] == "lighter" ||
                        (!t[i].empty() && std::isdigit(static_cast<unsigned char>(t[i][0])) && t[i].find_first_not_of("0123456789") == std::string::npos))) {
                weight = w; continue;
            }
            break;
        }
        if (i >= t.size()) return false;
        float px;
        if (!parse_font_size(t[i], env, px)) return false;
        ++i;
        int lh_mode = 0; float lh_val = 0;
        if (i + 1 < t.size() && t[i] == "/") {
            const std::string lt = t[i + 1];
            float n; Len l;
            if (lt == "normal") lh_mode = 0;
            else if (parse_number(lt, n)) { lh_mode = 1; lh_val = n; }
            else if (parse_length(lt, Env{env.parent, px, env.root_em, env.vw, env.vh}, l)) {
                lh_mode = 2; lh_val = l.px + l.pct * px / 100.f;
            } else return false;
            i += 2;
        }
        if (i >= t.size()) return false;
        std::string fam;
        for (size_t k = i; k < t.size(); ++k) fam += (k > i ? " " : "") + t[k];
        s.font_size = px; s.font_weight = weight; s.italic = italic;
        s.lh_mode = lh_mode; s.lh_val = lh_val;
        set_family(s, fam);
        return true;
    }
    if (name == "line-height") {
        if (v == "normal") { s.lh_mode = 0; return true; }
        float n; Len l;
        if (parse_number(v, n)) { if (n < 0) return false; s.lh_mode = 1; s.lh_val = n; return true; }
        if (!parse_length(v, env, l)) return false;
        s.lh_mode = 2; s.lh_val = l.px + l.pct * s.font_size / 100.f;
        return true;
    }
    if (name == "text-align") {
        if (v == "left" || v == "-webkit-left") s.text_align = TextAlign::Left;
        else if (v == "right" || v == "-webkit-right") s.text_align = TextAlign::Right;
        else if (v == "center" || v == "-webkit-center") s.text_align = TextAlign::Center;
        else if (v == "justify") s.text_align = TextAlign::Justify;
        else if (v == "start") s.text_align = TextAlign::Start;
        else if (v == "end") s.text_align = TextAlign::End;
        else return false;
        return true;
    }
    if (name == "text-decoration" || name == "text-decoration-line") {
        if (!apply_text_decoration_line(s, v, env)) {
            // shorthand may carry a color: "underline red"
            if (name != "text-decoration") return false;
            bool u = false, l = false, o = false;
            for (const auto& tk : t) {
                uint32_t c; bool isc = false;
                if (tk == "underline") u = true; else if (tk == "line-through") l = true;
                else if (tk == "overline") o = true;
                else if (one_of(tk, {"solid", "double", "dotted", "dashed", "wavy", "none"})) {}
                else if (parse_color(tk, c, &isc)) s.decoration_color = isc ? 0 : c;
                else return false;
            }
            s.underline = u; s.line_through = l; s.overline = o;
        } else if (name == "text-decoration") {
            for (const auto& tk : t) {
                uint32_t c; bool isc = false;
                if (parse_color(tk, c, &isc)) s.decoration_color = isc ? 0 : c;
            }
        }
        return true;
    }
    if (name == "text-decoration-color") {
        uint32_t c; bool isc = false;
        if (!parse_color(v, c, &isc)) return false;
        s.decoration_color = isc ? 0 : c;
        return true;
    }
    if (name == "text-transform") {
        if (v == "none") s.text_transform = TextTransform::None;
        else if (v == "uppercase") s.text_transform = TextTransform::Uppercase;
        else if (v == "lowercase") s.text_transform = TextTransform::Lowercase;
        else if (v == "capitalize") s.text_transform = TextTransform::Capitalize;
        else return false;
        return true;
    }
    if (name == "letter-spacing" || name == "word-spacing") {
        float px = 0;
        if (v == "normal") px = 0;
        else { px = parse_len_px(v, env, ok); if (!ok) return false; }
        (name == "letter-spacing" ? s.letter_spacing : s.word_spacing) = px;
        return true;
    }
    if (name == "text-indent") {
        s.text_indent = parse_len_px(t[0], env, ok);
        return ok;
    }
    if (name == "white-space") {
        if (v == "normal") s.white_space = WhiteSpace::Normal;
        else if (v == "nowrap") s.white_space = WhiteSpace::Nowrap;
        else if (v == "pre") s.white_space = WhiteSpace::Pre;
        else if (v == "pre-wrap") s.white_space = WhiteSpace::PreWrap;
        else if (v == "pre-line") s.white_space = WhiteSpace::PreLine;
        else if (v == "break-spaces") s.white_space = WhiteSpace::BreakSpaces;
        else return false;
        return true;
    }
    if (name == "text-overflow") { s.text_overflow_ellipsis = v.find("ellipsis") != std::string::npos; return true; }
    if (name == "overflow-wrap" || name == "word-wrap") {
        if (v == "normal") s.overflow_wrap = OverflowWrap::Normal;
        else if (v == "break-word") s.overflow_wrap = OverflowWrap::BreakWord;
        else if (v == "anywhere") s.overflow_wrap = OverflowWrap::Anywhere;
        else return false;
        return true;
    }
    if (name == "word-break") {
        if (v == "normal") s.word_break = WordBreak::Normal; else if (v == "break-all") s.word_break = WordBreak::BreakAll;
        else if (v == "keep-all") s.word_break = WordBreak::KeepAll; else if (v == "break-word") s.word_break = WordBreak::BreakWord;
        else return false;
        return true;
    }
    if (name == "vertical-align") {
        static const std::unordered_map<std::string, VerticalAlign> m = {
            {"baseline", VerticalAlign::Baseline}, {"sub", VerticalAlign::Sub}, {"super", VerticalAlign::Super},
            {"top", VerticalAlign::Top}, {"text-top", VerticalAlign::TextTop}, {"middle", VerticalAlign::Middle},
            {"bottom", VerticalAlign::Bottom}, {"text-bottom", VerticalAlign::TextBottom}};
        auto it = m.find(v);
        if (it != m.end()) { s.vertical_align = it->second; return true; }
        Len l;
        if (!parse_length(v, env, l)) return false;
        s.vertical_align = VerticalAlign::Length; s.vertical_align_len = l;
        return true;
    }
    if (name == "list-style-image") {
        if (v == "none") { s.list_image.clear(); return true; }
        if (!v.starts_with("url(")) return false;
        std::string u = trim_ws(raw.substr(raw.find('(') + 1, raw.rfind(')') - raw.find('(') - 1));
        if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u = u.substr(1);
        if (!u.empty() && (u.back() == '"' || u.back() == '\'')) u.pop_back();
        s.list_image = u;
        return true;
    }
    if (name == "list-style" || name == "list-style-type" || name == "list-style-position") {
        for (const auto& tk : t) {
            if (tk == "inside") s.list_inside = true;
            else if (tk == "outside") s.list_inside = false;
            else if (tk.starts_with("url(") && name == "list-style") {
                std::string u = trim_ws(tk.substr(4, tk.size() - 5));
                if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u = u.substr(1);
                if (!u.empty() && (u.back() == '"' || u.back() == '\'')) u.pop_back();
                s.list_image = u;
            }
            else if (name != "list-style-position" && !tk.starts_with("url(")) set_list_style(s, tk, ok);
        }
        return ok;
    }

    // ── flex ───────────────────────────────────────────────────────────────
    if (name == "flex-direction") {
        if (v == "row") s.flex_direction = FlexDirection::Row; else if (v == "row-reverse") s.flex_direction = FlexDirection::RowReverse;
        else if (v == "column") s.flex_direction = FlexDirection::Column; else if (v == "column-reverse") s.flex_direction = FlexDirection::ColumnReverse;
        else return false;
        return true;
    }
    if (name == "flex-wrap") {
        if (v == "nowrap") s.flex_wrap = FlexWrap::Nowrap; else if (v == "wrap") s.flex_wrap = FlexWrap::Wrap;
        else if (v == "wrap-reverse") s.flex_wrap = FlexWrap::WrapReverse; else return false;
        return true;
    }
    if (name == "flex-flow") {
        for (const auto& tk : t) {
            if (!apply_property(s, env, "flex-direction", tk) && !apply_property(s, env, "flex-wrap", tk)) return false;
        }
        return true;
    }
    if (name == "order") { float n; if (!parse_number(v, n)) return false; s.order = int(n); return true; }
    if (name == "flex-grow" || name == "flex-shrink") {
        float n; if (!parse_number(v, n) || n < 0) return false;
        (name == "flex-grow" ? s.flex_grow : s.flex_shrink) = n;
        return true;
    }
    if (name == "flex-basis") {
        if (v == "auto" || v == "content") { s.flex_basis = Len::Auto(); return true; }
        return parse_length(v, env, s.flex_basis);
    }
    if (name == "flex") {
        if (v == "none") { s.flex_grow = 0; s.flex_shrink = 0; s.flex_basis = Len::Auto(); return true; }
        if (v == "auto") { s.flex_grow = 1; s.flex_shrink = 1; s.flex_basis = Len::Auto(); return true; }
        if (v == "initial") { s.flex_grow = 0; s.flex_shrink = 1; s.flex_basis = Len::Auto(); return true; }
        float g = 1, sh = 1; Len b = Len::Pct(0);
        std::vector<float> nums;
        bool have_basis = false;
        for (const auto& tk : t) {
            float n;
            if (parse_number(tk, n)) { nums.push_back(n); continue; }
            if (tk == "auto") { b = Len::Auto(); have_basis = true; continue; }
            if (!parse_length(tk, env, b)) return false;
            have_basis = true;
        }
        if (nums.empty() && !have_basis) return false;
        if (!nums.empty()) g = nums[0];
        if (nums.size() > 1) sh = nums[1];
        s.flex_grow = g; s.flex_shrink = sh; s.flex_basis = b;
        return true;
    }
    if (name == "justify-content" || name == "align-content" || name == "align-items" || name == "align-self" ||
        name == "justify-items" || name == "justify-self") {
        const Align a = parse_align(v, ok);
        if (!ok) return false;
        if (name == "justify-content") s.justify_content = a; else if (name == "align-content") s.align_content = a;
        else if (name == "align-items") s.align_items = a; else if (name == "align-self") s.align_self = a;
        else if (name == "justify-items") s.justify_items = a; else s.justify_self = a;
        return true;
    }
    if (name == "place-items" || name == "place-content" || name == "place-self") {
        const std::string a = t[0], b = t.size() > 1 ? t[1] : t[0];
        const std::string base = name.substr(6);
        const bool c = name == "place-content";
        return apply_property(s, env, "align-" + base, a) &&
               apply_property(s, env, "justify-" + base, c ? b : b);
    }
    if (name == "gap" || name == "grid-gap") {
        if (t.empty() || t.size() > 2) return false;
        Len a, b;
        if (t[0] == "normal") a = Len::Px(0); else if (!parse_length(t[0], env, a)) return false;
        if (t.size() == 1) b = a;
        else if (t[1] == "normal") b = Len::Px(0); else if (!parse_length(t[1], env, b)) return false;
        s.row_gap = a; s.column_gap = b; s.column_gap_set = t.size() == 1 ? t[0] != "normal" : t[1] != "normal"; s.row_gap_set = t[0] != "normal";
        return true;
    }
    if (name == "row-gap" || name == "column-gap" || name == "grid-row-gap" || name == "grid-column-gap") {
        Len l;
        if (v == "normal") l = Len::Px(0); else if (!parse_length(v, env, l)) return false;
        (name.find("row") != std::string::npos ? s.row_gap : s.column_gap) = l;
        if (name.find("row") == std::string::npos) s.column_gap_set = v != "normal"; else s.row_gap_set = v != "normal";
        return true;
    }
    if (name == "column-count") {
        if (v == "auto") { s.column_count = 0; return true; }
        char* e = nullptr; const long n = std::strtol(v.c_str(), &e, 10);
        if (*e || n < 1) return false;
        s.column_count = int(std::min(n, 1000L));
        return true;
    }
    if (name == "column-width") {
        if (v == "auto") { s.column_width = Len::Auto(); return true; }
        Len l; if (!parse_length(v, env, l)) return false;
        s.column_width = l;
        return true;
    }
    if (name == "columns") {
        int cnt = 0; Len w = Len::Auto();
        for (const auto& tk : t) {
            if (tk == "auto") continue;
            char* e = nullptr; const long n = std::strtol(tk.c_str(), &e, 10);
            if (!*e && !tk.empty()) { if (n < 1) return false; cnt = int(std::min(n, 1000L)); }
            else if (!parse_length(tk, env, w)) return false;
        }
        s.column_count = cnt; s.column_width = w;
        return true;
    }
    if (name == "column-rule" || name == "column-rule-width" || name == "column-rule-style" || name == "column-rule-color") {
        if (name == "column-rule-width") { float w; if (!parse_bwidth(v, env, w)) return false; s.column_rule_w = w; return true; }
        if (name == "column-rule-style") { BorderStyle b; if (!parse_bstyle(v, b)) return false; s.column_rule_style = b; return true; }
        auto color = [&](const std::string& tk) {
            if (tk == "currentcolor") { s.column_rule_cur = true; return true; }
            uint32_t c; if (!parse_color(tk, c)) return false;
            s.column_rule_color = c; s.column_rule_cur = false; return true;
        };
        if (name == "column-rule-color") return color(v);
        float w = 3; BorderStyle bs = BorderStyle::None; s.column_rule_cur = true;
        if (v == "none") { s.column_rule_w = 0; s.column_rule_style = BorderStyle::None; return true; }
        for (const auto& tk : t) {
            BorderStyle b2; float w2;
            if (parse_bstyle(tk, b2)) bs = b2;
            else if (parse_bwidth(tk, env, w2)) w = w2;
            else if (!color(tk)) return false;
        }
        s.column_rule_w = w; s.column_rule_style = bs;
        return true;
    }

    // ── grid ───────────────────────────────────────────────────────────────
    if ((name == "grid-template-columns" || name == "grid-template-rows") && v.starts_with("subgrid")) {
        (name == "grid-template-columns" ? s.grid_cols_sub : s.grid_rows_sub) = true;
        (name == "grid-template-columns" ? s.grid_template_columns : s.grid_template_rows).clear();
        return true;
    }
    if (name == "grid-template-columns" || name == "grid-template-rows") (name == "grid-template-columns" ? s.grid_cols_sub : s.grid_rows_sub) = false;
    if (name == "grid-template-columns") {
        std::vector<Track> tr;
        std::vector<Track> pat; int ar = 0; size_t pos = 0;
        if (!parse_tracks(raw, env, tr, &ar, &pat, &pos)) return false;
        s.grid_template_columns = tr; s.grid_col_autorepeat = ar; s.grid_col_auto_pattern = pat; s.grid_col_auto_pos = pos;
        return true;
    }
    if (name == "grid-template-rows") {
        std::vector<Track> tr;
        if (!parse_tracks(raw, env, tr)) return false;
        s.grid_template_rows = tr;
        return true;
    }
    if (name == "grid-auto-columns" || name == "grid-auto-rows") {
        std::vector<Track> tr;
        if (!parse_tracks(raw, env, tr)) return false;
        (name == "grid-auto-columns" ? s.grid_auto_columns : s.grid_auto_rows) = tr;
        return true;
    }
    if (name == "grid-auto-flow") {
        s.grid_auto_flow_column = v.find("column") != std::string::npos;
        s.grid_auto_flow_dense = v.find("dense") != std::string::npos;
        return true;
    }
    if (name == "grid-template-areas") {
        s.grid_template_areas.clear();
        if (v == "none") return true;
        size_t i = 0;
        while (i < raw.size()) {
            const size_t a = raw.find_first_of("\"'", i);
            if (a == std::string::npos) break;
            const size_t b = raw.find(raw[a], a + 1);
            if (b == std::string::npos) return false;
            s.grid_template_areas.push_back(tokens(raw.substr(a + 1, b - a - 1)));
            i = b + 1;
        }
        return true;
    }
    if (name == "grid-column" || name == "grid-row") {
        const auto parts = split_slash(t);
        GridLine a, b;
        if (!parse_gridline(parts[0], a)) return false;
        if (parts.size() > 1) { if (!parse_gridline(parts[1], b)) return false; }
        else if (a.span == 0 && !a.auto_ && a.name.empty()) b = GridLine{};   // single line: end auto
        const bool col = name == "grid-column";
        (col ? s.grid_column_start : s.grid_row_start) = a;
        (col ? s.grid_column_end : s.grid_row_end) = b;
        return true;
    }
    if (name == "grid-column-start" || name == "grid-column-end" || name == "grid-row-start" || name == "grid-row-end") {
        GridLine g;
        if (!parse_gridline(t, g)) return false;
        if (name == "grid-column-start") s.grid_column_start = g; else if (name == "grid-column-end") s.grid_column_end = g;
        else if (name == "grid-row-start") s.grid_row_start = g; else s.grid_row_end = g;
        return true;
    }
    if (name == "grid-area") {
        const auto parts = split_slash(t);
        if (parts.size() == 1 && parts[0].size() == 1) {
            float n;
            if (!parse_number(parts[0][0], n) && parts[0][0] != "auto") { s.grid_area_name = parts[0][0]; return true; }
        }
        GridLine g[4];
        for (size_t i = 0; i < parts.size() && i < 4; ++i) if (!parse_gridline(parts[i], g[i])) return false;
        s.grid_row_start = g[0];
        s.grid_column_start = parts.size() > 1 ? g[1] : GridLine{};
        s.grid_row_end = parts.size() > 2 ? g[2] : GridLine{};
        s.grid_column_end = parts.size() > 3 ? g[3] : GridLine{};
        return true;
    }

    // ── table ──────────────────────────────────────────────────────────────
    if (name == "border-collapse") {
        if (v == "collapse") s.border_collapse = true; else if (v == "separate") s.border_collapse = false;
        else return false;
        return true;
    }
    if (name == "border-spacing") {
        if (t.empty() || t.size() > 2) return false;
        const float h = parse_len_px(t[0], env, ok);
        const float vv = t.size() > 1 ? parse_len_px(t[1], env, ok) : h;
        if (!ok) return false;
        s.border_spacing_h = h; s.border_spacing_v = vv;
        return true;
    }
    if (name == "table-layout") { s.table_fixed = v == "fixed"; return true; }

    // ── generated content / misc ───────────────────────────────────────────
    if (name == "content") {
        if (v == "none" || v == "normal") { s.has_content = false; s.content.clear(); s.content_items.clear(); return true; }
        std::string out;
        for (const auto& tk : t) {
            if (tk.size() >= 2 && (tk.front() == '"' || tk.front() == '\'')) {
                // keep the original case: re-read from raw
            }
        }
        // strings and counter()/counters() in order (attr() is resolved by the cascade beforehand)
        size_t i = 0;
        bool any = false, has_counter = false;
        std::vector<ContentItem> items;
        auto close_paren = [&](size_t from) {   // matching ')' of the '(' at `from`, skipping quoted strings
            int depth = 0;
            for (size_t k = from; k < raw.size(); ++k) {
                const char ch = raw[k];
                if (ch == '"' || ch == '\'') { const size_t e = raw.find(ch, k + 1); if (e == std::string::npos) return std::string::npos; k = e; }
                else if (ch == '(') ++depth;
                else if (ch == ')' && --depth == 0) return k;
            }
            return std::string::npos;
        };
        while (i < raw.size()) {
            const char c = raw[i];
            const bool cs = lower_ascii(raw.substr(i, 9)) == "counters(";
            if (cs || lower_ascii(raw.substr(i, 8)) == "counter(") {
                const size_t open = raw.find('(', i), close = close_paren(open);
                if (close == std::string::npos) return false;
                const auto args = split_commas(raw.substr(open + 1, close - open - 1));
                ContentItem ci;
                ci.kind = cs ? ContentItem::Kind::Counters : ContentItem::Kind::Counter;
                const size_t nargs = cs ? 3 : 2;
                if (args.empty() || args.size() > nargs || (cs && args.size() < 2)) return false;
                ci.name = trim_ws(args[0]);
                if (ci.name.empty()) return false;
                if (cs) {
                    const std::string sp = trim_ws(args[1]);
                    if (sp.size() < 2 || (sp.front() != '"' && sp.front() != '\'') || sp.back() != sp.front()) return false;
                    ci.sep = sp.substr(1, sp.size() - 2);
                }
                if (args.size() == nargs) {
                    ComputedStyle tmp;
                    bool ok = true;
                    set_list_style(tmp, lower_ascii(trim_ws(args[nargs - 1])), ok);
                    if (!ok) return false;
                    ci.style = tmp.list_style;
                }
                items.push_back(std::move(ci));
                has_counter = any = true;
                i = close + 1;
            } else if (c == '"' || c == '\'') {
                const size_t e = raw.find(c, i + 1);
                if (e == std::string::npos) break;
                std::string seg = raw.substr(i + 1, e - i - 1);
                // CSS escapes: \A newline, \201C unicode
                std::string dec;
                for (size_t k = 0; k < seg.size(); ++k) {
                    if (seg[k] == '\\' && k + 1 < seg.size()) {
                        size_t h = k + 1, n = 0;
                        unsigned cp = 0;
                        while (h < seg.size() && n < 6 && std::isxdigit(static_cast<unsigned char>(seg[h]))) {
                            cp = cp * 16 + (std::isdigit(static_cast<unsigned char>(seg[h])) ? seg[h] - '0' : (std::tolower(seg[h]) - 'a' + 10));
                            ++h; ++n;
                        }
                        if (n > 0) {
                            if (cp < 0x80) dec += char(cp);
                            else if (cp < 0x800) { dec += char(0xC0 | (cp >> 6)); dec += char(0x80 | (cp & 0x3F)); }
                            else if (cp < 0x10000) { dec += char(0xE0 | (cp >> 12)); dec += char(0x80 | ((cp >> 6) & 0x3F)); dec += char(0x80 | (cp & 0x3F)); }
                            else { dec += char(0xF0 | (cp >> 18)); dec += char(0x80 | ((cp >> 12) & 0x3F)); dec += char(0x80 | ((cp >> 6) & 0x3F)); dec += char(0x80 | (cp & 0x3F)); }
                            if (h < seg.size() && seg[h] == ' ') ++h;
                            k = h - 1;
                            continue;
                        }
                        dec += seg[k + 1]; ++k;
                        continue;
                    }
                    dec += seg[k];
                }
                out += dec;
                ContentItem ci; ci.str = dec;
                items.push_back(std::move(ci));
                any = true;
                i = e + 1;
            } else ++i;
        }
        s.content_items = has_counter ? items : std::vector<ContentItem>{};
        s.has_content = any || v == "\"\"" || v == "''";
        s.content = out;
        return true;
    }
    if (name == "counter-reset" || name == "counter-increment" || name == "counter-set") {
        std::vector<std::pair<std::string, int>> out;
        const int def = name == "counter-increment" ? 1 : 0;
        if (v != "none") {
            if (t.empty()) return false;
            for (size_t i = 0; i < t.size(); ++i) {
                const std::string& id = t[i];
                if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0])) || id[0] == '-' || id[0] == '+' || id == "none") return false;
                int n = def;
                if (i + 1 < t.size() && (std::isdigit(static_cast<unsigned char>(t[i + 1][0])) || t[i + 1][0] == '-' || t[i + 1][0] == '+')) {
                    char* end = nullptr;
                    const long l = std::strtol(t[i + 1].c_str(), &end, 10);
                    if (*end) return false;
                    n = int(l);
                    ++i;
                }
                out.emplace_back(id, n);
            }
        }
        (name == "counter-reset" ? s.counter_reset : name == "counter-increment" ? s.counter_inc : s.counter_set) = out;
        return true;
    }
    if (name == "cursor") {
        auto has = [&](const char* k) { return v.find(k) != std::string::npos; };
        s.cursor = has("pointer") ? 1 : has("text") ? 2 : has("default") ? 3 : has("not-allowed") ? 4 :
                   has("grabbing") ? 6 : has("grab") ? 5 : 0;
        return true;
    }
    if (name == "transform") {
        std::vector<TfFn> fns;
        if (v != "none") {
            size_t i = 0;
            while (i < v.size()) {
                while (i < v.size() && v[i] == ' ') ++i;
                if (i >= v.size()) break;
                const size_t open = v.find('(', i), close = v.find(')', open);
                if (open == std::string::npos || close == std::string::npos) return false;
                const std::string fn = lower_ascii(trim_ws(v.substr(i, open - i)));
                const std::vector<std::string> a = split_commas(v.substr(open + 1, close - open - 1));
                auto num = [&](size_t k, float& out) { return k < a.size() && parse_number(trim_ws(a[k]), out); };
                auto ang = [&](size_t k, float& out) { return k < a.size() && parse_angle(trim_ws(a[k]), out); };
                TfFn f;
                if (a.empty()) return false;
                if (fn == "translate" || fn == "translate3d") {
                    if (!parse_length(trim_ws(a[0]), env, f.x, false)) return false;
                    if (a.size() > 1 && !parse_length(trim_ws(a[1]), env, f.y, false)) return false;
                    if (a.size() > 2) { Len z; if (!parse_length(trim_ws(a[2]), env, z, false)) return false; f.a = z.px; }
                } else if (fn == "translatex") { if (!parse_length(trim_ws(a[0]), env, f.x, false)) return false; }
                else if (fn == "translatey") { if (!parse_length(trim_ws(a[0]), env, f.y, false)) return false; }
                else if (fn == "scale" || fn == "scale3d") {
                    f.kind = 'S';
                    if (!num(0, f.a)) return false;
                    f.b = f.a;
                    if (a.size() > 1 && !num(1, f.b)) return false;
                } else if (fn == "scalex") { f.kind = 'S'; f.b = 1; if (!num(0, f.a)) return false; }
                else if (fn == "scaley") { f.kind = 'S'; f.a = 1; if (!num(0, f.b)) return false; }
                else if (fn == "rotate" || fn == "rotatez") { f.kind = 'R'; if (!ang(0, f.a)) return false; }
                else if (fn == "skew") { f.kind = 'K'; if (!ang(0, f.a)) return false; if (a.size() > 1 && !ang(1, f.b)) return false; }
                else if (fn == "skewx") { f.kind = 'K'; if (!ang(0, f.a)) return false; }
                else if (fn == "skewy") { f.kind = 'K'; if (!ang(0, f.b)) return false; }
                else if (fn == "matrix") {
                    if (a.size() != 6) return false;
                    float m[6];
                    for (int k = 0; k < 6; ++k) if (!num(size_t(k), m[k])) return false;
                    f.kind = 'M'; f.a = m[0]; f.b = m[1]; f.c = m[2]; f.d = m[3]; f.e = m[4]; f.f = m[5];
                } else if (fn == "translatez" || fn == "perspective") {
                    Len l; if (!parse_length(trim_ws(a[0]), env, l, false)) return false;
                    f.kind = fn == "perspective" ? 'P' : 'Z'; f.a = l.px;
                } else if (fn == "rotatex" || fn == "rotatey") { f.kind = fn == "rotatex" ? 'X' : 'Y'; if (!ang(0, f.a)) return false; }
                else if (fn == "rotate3d") {
                    if (a.size() != 4 || !num(0, f.b) || !num(1, f.c) || !num(2, f.d) || !ang(3, f.a)) return false;
                    f.kind = '3';
                } else if (fn == "scalez") { i = close + 1; continue; }   // no effect on a flat element
                else if (fn == "matrix3d") {
                    // the 2D part only: a b c d e f = m[0] m[1] m[4] m[5] m[12] m[13]
                    if (a.size() != 16) return false;
                    float m[16];
                    for (int k = 0; k < 16; ++k) if (!num(size_t(k), m[k])) return false;
                    f.kind = 'M'; f.a = m[0]; f.b = m[1]; f.c = m[4]; f.d = m[5]; f.e = m[12]; f.f = m[13];
                } else return false;
                fns.push_back(f);
                i = close + 1;
            }
        }
        s.tfn = std::move(fns);
        s.rebuild_transform();
        return true;
    }
    if (name == "perspective") {
        if (v == "none") { s.persp = 0; return true; }
        Len l; if (!parse_length(v, env, l, false) || l.px < 0) return false;
        s.persp = l.px;
        return true;
    }
    if (name == "perspective-origin") {
        Len x, y;
        if (!parse_position_pair(t, env, x, y)) return false;
        s.persp_x = x; s.persp_y = y;
        return true;
    }
    if (name == "transform-style") return v == "flat" || v == "preserve-3d";
    if (name == "backface-visibility") { if (v != "visible" && v != "hidden") return false; s.backface_hidden = v == "hidden"; return true; }
    if (name == "transform-origin") {
        Len x, y;
        Tokens tk = t;
        if (tk.size() == 3) tk.pop_back();   // z offset
        if (!parse_position_pair(tk, env, x, y)) return false;
        s.origin_x = x; s.origin_y = y;
        return true;
    }
    if (name.starts_with("transition")) {
        auto time_ms = [](const std::string& tk, float& ms) {
            if (tk.size() < 2 || !(std::isdigit(static_cast<unsigned char>(tk[0])) || tk[0] == '.' || tk[0] == '-' || tk[0] == '+')) return false;
            const bool is_ms = tk.ends_with("ms");
            if (!is_ms && !tk.ends_with("s")) return false;
            try { ms = std::stof(tk) * (is_ms ? 1.f : 1000.f); } catch (...) { return false; }
            return true;
        };
        auto prop_bits = [](const std::string& tk) -> unsigned {
            if (tk == "all") return TransitionSpec::All;
            if (tk == "opacity") return TransitionSpec::Opacity;
            if (tk == "transform") return TransitionSpec::Transform;
            if (tk == "background" || tk == "background-color") return TransitionSpec::Background;
            if (tk == "color") return TransitionSpec::Color;
            if (tk == "border-color" || tk == "border") return TransitionSpec::Border;
            if (tk == "width" || tk == "height" || tk == "left" || tk == "right" || tk == "top" || tk == "bottom" || tk == "inset" ||
                tk.starts_with("margin") || tk.starts_with("padding") || tk == "min-width" || tk == "max-width" || tk == "min-height" || tk == "max-height")
                return TransitionSpec::Length;
            return 0;   // other properties (box-shadow...) are accepted and simply not animated
        };
        auto timing = [](const std::string& tk, float* bez) {
            if (tk == "linear") { bez[0] = 0; bez[1] = 0; bez[2] = 1; bez[3] = 1; }
            else if (tk == "ease-in") { bez[0] = .42f; bez[1] = 0; bez[2] = 1; bez[3] = 1; }
            else if (tk == "ease-out") { bez[0] = 0; bez[1] = 0; bez[2] = .58f; bez[3] = 1; }
            else if (tk == "ease-in-out") { bez[0] = .42f; bez[1] = 0; bez[2] = .58f; bez[3] = 1; }
            else if (tk == "ease") { bez[0] = .25f; bez[1] = .1f; bez[2] = .25f; bez[3] = 1; }
            else if (tk.starts_with("cubic-bezier(")) { if (std::sscanf(tk.c_str(), "cubic-bezier(%f,%f,%f,%f)", &bez[0], &bez[1], &bez[2], &bez[3]) != 4) return false; }
            else return false;
            return true;
        };
        const auto items = split_commas(v);
        auto at = [&](std::vector<TransitionSpec>& vec, size_t i) -> TransitionSpec& { if (vec.size() <= i) vec.resize(i + 1); return vec[i]; };
        if (name == "transition") {
            s.transitions.clear();
            if (v == "none") return true;
            for (const auto& it : items) {
                TransitionSpec sp;
                int times = 0;
                for (const auto& tk : tokens(trim_ws(it))) {
                    float ms = 0;
                    if (time_ms(tk, ms)) (times++ == 0 ? sp.duration : sp.delay) = ms;
                    else if (timing(tk, sp.bez)) {}
                    else sp.props |= prop_bits(tk);
                }
                s.transitions.push_back(sp);
            }
            return true;
        }
        // longhands: list i of each longhand feeds entry i, repeating when a list is shorter than the property list
        if (name == "transition-property") {
            s.transitions.resize(items.size());
            for (size_t i = 0; i < items.size(); ++i) { s.transitions[i].props = v == "none" ? 0 : prop_bits(trim_ws(items[i])); }
            return true;
        }
        if (name == "transition-duration" || name == "transition-delay" || name == "transition-timing-function") {
            if (s.transitions.empty()) s.transitions.resize(1);
            const size_t n = std::max(items.size(), s.transitions.size());
            if (s.transitions.size() < n) { const size_t m = s.transitions.size(); for (size_t i = m; i < n; ++i) s.transitions.push_back(s.transitions[i % m]); }
            for (size_t i = 0; i < n; ++i) {
                TransitionSpec& sp = at(s.transitions, i);
                const std::string tk = trim_ws(items[i % items.size()]);
                float ms = 0;
                if (name == "transition-timing-function") { if (!timing(tk, sp.bez)) return false; }
                else { if (!time_ms(tk, ms)) return false; (name == "transition-duration" ? sp.duration : sp.delay) = ms; }
            }
            return true;
        }
        return false;
    }
    if (name.starts_with("animation")) {
        auto time_ms = [](const std::string& tk, float& ms) {
            if (tk.size() < 2 || !(std::isdigit(static_cast<unsigned char>(tk[0])) || tk[0] == '.' || tk[0] == '-' || tk[0] == '+')) return false;
            const bool is_ms = tk.ends_with("ms");
            if (!is_ms && !tk.ends_with("s")) return false;
            try { ms = std::stof(tk) * (is_ms ? 1.f : 1000.f); } catch (...) { return false; }
            return true;
        };
        auto timing = [](const std::string& tk, AnimationSpec& sp) {
            if (!(tk == "linear" || tk.starts_with("ease") || tk.starts_with("cubic-bezier(") || tk.starts_with("step"))) return false;
            sp.steps = 0;
            auto set = [&](float a, float b, float c, float d) { sp.bez[0] = a; sp.bez[1] = b; sp.bez[2] = c; sp.bez[3] = d; };
            if (tk == "linear") set(0, 0, 1, 1);
            else if (tk == "ease-in") set(.42f, 0, 1, 1);
            else if (tk == "ease-out") set(0, 0, .58f, 1);
            else if (tk == "ease-in-out") set(.42f, 0, .58f, 1);
            else if (tk == "ease") set(.25f, .1f, .25f, 1);
            else if (tk == "step-start") { sp.steps = 1; sp.steps_start = true; }
            else if (tk == "step-end") { sp.steps = 1; sp.steps_start = false; }
            else if (tk.starts_with("cubic-bezier(")) { if (std::sscanf(tk.c_str(), "cubic-bezier(%f,%f,%f,%f)", &sp.bez[0], &sp.bez[1], &sp.bez[2], &sp.bez[3]) != 4) return false; }
            else if (tk.starts_with("steps(")) {
                int n = 0;
                if (std::sscanf(tk.c_str(), "steps(%d", &n) != 1 || n < 1) return false;
                sp.steps = n; sp.steps_start = tk.find("start") != std::string::npos || tk.find("jump-start") != std::string::npos;
            } else return false;
            return true;
        };
        auto direction = [](const std::string& tk, int& d) {
            if (tk == "normal") d = 0; else if (tk == "reverse") d = 1; else if (tk == "alternate") d = 2; else if (tk == "alternate-reverse") d = 3; else return false;
            return true;
        };
        auto fill = [](const std::string& tk, int& f) {
            if (tk == "none") f = 0; else if (tk == "forwards") f = 1; else if (tk == "backwards") f = 2; else if (tk == "both") f = 3; else return false;
            return true;
        };
        auto iters = [](const std::string& tk, float& n) {
            if (tk == "infinite") { n = INFINITY; return true; }
            return parse_number(tk, n) && n >= 0;
        };
        auto state = [](const std::string& tk, bool& paused) {
            if (tk == "paused") paused = true; else if (tk == "running") paused = false; else return false;
            return true;
        };
        const auto items = split_commas(v);
        if (name == "animation") {
            s.animations.clear();
            if (v == "none") return true;
            for (const auto& it : items) {
                AnimationSpec sp;
                int times = 0;
                bool named = false;
                for (const auto& tk : tokens(trim_ws(it))) {
                    float x = 0;
                    if (time_ms(tk, x)) (times++ == 0 ? sp.duration : sp.delay) = x;
                    else if (timing(tk, sp)) {}
                    else if (direction(tk, sp.direction) || fill(tk, sp.fill) || state(tk, sp.paused)) {}
                    else if (iters(tk, sp.iterations)) {}
                    else if (!named && tk != "none") { sp.name = tk; named = true; }
                }
                if (named) s.animations.push_back(sp);
            }
            return true;
        }
        if (name == "animation-name") {
            std::vector<AnimationSpec> old = std::move(s.animations);
            s.animations.clear();
            for (size_t i = 0; i < items.size(); ++i) {
                AnimationSpec sp = old.empty() ? AnimationSpec{} : old[i % old.size()];
                sp.name = trim_ws(items[i]);
                if (sp.name != "none") s.animations.push_back(sp);
            }
            return true;
        }
        if (s.animations.empty()) s.animations.resize(items.size());   // longhand before animation-name: nameless slots, named later
        for (size_t i = 0; i < s.animations.size(); ++i) {
            AnimationSpec& sp = s.animations[i];
            const std::string tk = trim_ws(items[i % items.size()]);
            float x = 0;
            if (name == "animation-duration") { if (!time_ms(tk, sp.duration)) return false; }
            else if (name == "animation-delay") { if (!time_ms(tk, sp.delay)) return false; }
            else if (name == "animation-timing-function") { if (!timing(tk, sp)) return false; }
            else if (name == "animation-iteration-count") { if (!iters(tk, sp.iterations)) return false; }
            else if (name == "animation-direction") { if (!direction(tk, sp.direction)) return false; }
            else if (name == "animation-fill-mode") { if (!fill(tk, sp.fill)) return false; }
            else if (name == "animation-play-state") { if (!state(tk, sp.paused)) return false; }
            else return false;
            (void)x;
        }
        return true;
    }
    if (name == "scrollbar-width") { s.scrollbar_thin = v == "thin"; s.scrollbar_none = v == "none"; return v == "thin" || v == "auto" || v == "none"; }
    if (name == "scrollbar-color") {
        if (v == "auto") { s.scrollbar_set = false; return true; }
        if (t.size() != 2) return false;
        uint32_t a, b;
        if (!parse_color(t[0], a) || !parse_color(t[1], b)) return false;
        s.scrollbar_thumb = a; s.scrollbar_track = b; s.scrollbar_set = true;
        return true;
    }
    if (name == "accent-color") {
        uint32_t c = 0;
        if (v != "auto" && !parse_color(v, c)) return false;
        s.accent = v == "auto" ? 0 : c;
        return true;
    }
    if (name == "appearance" || name == "-webkit-appearance" || name == "-moz-appearance") {
        s.appearance_none = v == "none";
        return true;
    }
    if (name == "container-type") {
        if (v == "normal") s.container_type = 0;
        else if (v == "inline-size") s.container_type = 1;
        else if (v == "size") s.container_type = 2;
        else return false;
        return true;
    }
    if (name == "container-name") { s.container_name = v == "none" ? "" : v; return true; }
    if (name == "container") {   // <name> [/ <type>]
        const size_t slash = v.find('/');
        const std::string nm = trim_ws(v.substr(0, slash));
        s.container_name = nm == "none" ? "" : nm;
        s.container_type = 0;
        if (slash != std::string::npos) {
            const std::string ty = trim_ws(v.substr(slash + 1));
            if (ty == "inline-size") s.container_type = 1;
            else if (ty == "size") s.container_type = 2;
            else if (ty != "normal") return false;
        }
        return true;
    }
    if (name == "aspect-ratio") {
        float r = 0;
        for (const auto& tk : t) {
            if (tk == "auto" || tk == "/") continue;
            char* e = nullptr;
            const float a = std::strtof(tk.c_str(), &e);
            if (e == tk.c_str()) return false;
            if (r == 0) r = a; else if (a > 0) r /= a;
            if (*e == '/') { const float b = std::strtof(e + 1, nullptr); if (b > 0) r /= b; }
        }
        s.aspect_ratio = v == "auto" ? 0 : r;
        return true;
    }
    if (name == "outline" || name == "outline-style" || name == "outline-width" || name == "outline-color" ||
        name == "outline-offset") {
        if (name == "outline-offset") { Len l; if (!parse_length(v, env, l, false)) return false; s.outline_offset = l.px; return true; }
        if (name == "outline") { s.outline_style = BorderStyle::None; s.outline_auto = false; s.outline_w = 3; s.outline_current = true; }
        for (const auto& tk : t) {
            BorderStyle b2; float w2; uint32_t c;
            if (tk == "auto") { s.outline_auto = true; s.outline_style = BorderStyle::Solid; }
            else if (name != "outline-width" && name != "outline-color" && parse_bstyle(tk, b2)) { s.outline_style = b2; s.outline_auto = false; }
            else if (name != "outline-style" && name != "outline-color" && parse_bwidth(tk, env, w2)) s.outline_w = w2;
            else if (name != "outline-style" && name != "outline-width" && tk == "currentcolor") s.outline_current = true;
            else if (name != "outline-style" && name != "outline-width" && parse_color(tk, c)) { s.outline_color = c; s.outline_current = false; }
            else if (tk == "invert") {}
            else return false;
        }
        return true;
    }
    if (name == "pointer-events") { s.pointer_events = v != "none"; return true; }

    return false;   // unknown property
}

// ── inherit / initial / unset ───────────────────────────────────────────────

bool copy_property(ComputedStyle& d, const ComputedStyle& src, const std::string& n) {
#define CP(prop, member) if (n == prop) { d.member = src.member; return true; }
    // shorthands first
    static const std::unordered_map<std::string, std::vector<std::string>> sh = {
        {"margin", {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
        {"padding", {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
        {"border", {"border-width", "border-style", "border-color"}},
        {"border-top", {"border-top-width", "border-top-style", "border-top-color"}},
        {"border-right", {"border-right-width", "border-right-style", "border-right-color"}},
        {"border-bottom", {"border-bottom-width", "border-bottom-style", "border-bottom-color"}},
        {"border-left", {"border-left-width", "border-left-style", "border-left-color"}},
        {"border-width", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}},
        {"border-style", {"border-top-style", "border-right-style", "border-bottom-style", "border-left-style"}},
        {"border-color", {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"}},
        {"border-radius", {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}},
        {"inset", {"top", "right", "bottom", "left"}},
        {"background", {"background-color", "background-image", "background-repeat", "background-position", "background-size"}},
        {"font", {"font-style", "font-weight", "font-size", "line-height", "font-family"}},
        {"flex", {"flex-grow", "flex-shrink", "flex-basis"}},
        {"flex-flow", {"flex-direction", "flex-wrap"}},
        {"gap", {"row-gap", "column-gap"}}, {"grid-gap", {"row-gap", "column-gap"}},
        {"overflow", {"overflow-x", "overflow-y"}},
        {"list-style", {"list-style-type", "list-style-position", "list-style-image"}},
        {"text-decoration", {"text-decoration-line", "text-decoration-color"}},
        {"place-items", {"align-items", "justify-items"}},
        {"grid-column", {"grid-column-start", "grid-column-end"}},
        {"grid-row", {"grid-row-start", "grid-row-end"}},
    };
    if (auto it = sh.find(n); it != sh.end()) {
        for (const auto& l : it->second) copy_property(d, src, l);
        return true;
    }
    CP("display", display) CP("position", position) CP("float", float_) CP("clear", clear)
    CP("box-sizing", box_sizing) CP("overflow-x", overflow_x) CP("overflow-y", overflow_y)
    CP("visibility", visibility) CP("opacity", opacity) CP("mix-blend-mode", mix_blend) CP("background-blend-mode", bg_blend)
    if (n == "z-index") { d.z_auto = src.z_auto; d.z_index = src.z_index; return true; }
    CP("width", width) CP("height", height) CP("min-width", min_width) CP("min-height", min_height)
    CP("max-width", max_width) CP("max-height", max_height)
    CP("margin-top", margin[0]) CP("margin-right", margin[1]) CP("margin-bottom", margin[2]) CP("margin-left", margin[3])
    CP("padding-top", padding[0]) CP("padding-right", padding[1]) CP("padding-bottom", padding[2]) CP("padding-left", padding[3])
    CP("top", inset[0]) CP("right", inset[1]) CP("bottom", inset[2]) CP("left", inset[3])
    CP("border-top-width", border_w[0]) CP("border-right-width", border_w[1]) CP("border-bottom-width", border_w[2]) CP("border-left-width", border_w[3])
    CP("border-top-style", border_style[0]) CP("border-right-style", border_style[1]) CP("border-bottom-style", border_style[2]) CP("border-left-style", border_style[3])
    if (n == "border-top-color") { d.border_color[0] = src.border_color[0]; d.bc_current[0] = src.bc_current[0]; return true; }
    if (n == "border-right-color") { d.border_color[1] = src.border_color[1]; d.bc_current[1] = src.bc_current[1]; return true; }
    if (n == "border-bottom-color") { d.border_color[2] = src.border_color[2]; d.bc_current[2] = src.bc_current[2]; return true; }
    if (n == "border-left-color") { d.border_color[3] = src.border_color[3]; d.bc_current[3] = src.bc_current[3]; return true; }
    CP("border-top-left-radius", radius[0]) CP("border-top-right-radius", radius[1]) CP("border-bottom-right-radius", radius[2]) CP("border-bottom-left-radius", radius[3])
    CP("background-color", bg_color) CP("background-image", bg) CP("background-repeat", bg) CP("background-position", bg) CP("background-size", bg)
    CP("box-shadow", box_shadow) CP("text-shadow", text_shadow) CP("clip-path", clip_path) CP("counter-reset", counter_reset) CP("counter-increment", counter_inc) CP("counter-set", counter_set) CP("filter", filter) CP("backdrop-filter", backdrop_filter) CP("mask-image", mask) CP("-webkit-mask-image", mask) CP("object-fit", object_fit)
    if (n == "display") { d.display = src.display; d.webkit_box = src.webkit_box; return true; }
    CP("-webkit-line-clamp", line_clamp) CP("-webkit-box-orient", box_orient_v)
    if (n == "object-position") { d.object_pos_x = src.object_pos_x; d.object_pos_y = src.object_pos_y; return true; }
    CP("color", color) CP("font-size", font_size) CP("font-weight", font_weight) CP("font-style", italic)
    if (n == "font-family") { d.family = src.family; d.family_name = src.family_name; return true; }
    if (n == "line-height") { d.lh_mode = src.lh_mode; d.lh_val = src.lh_val; return true; }
    CP("text-align", text_align)
    if (n == "text-decoration-line") { d.underline = src.underline; d.line_through = src.line_through; d.overline = src.overline; return true; }
    CP("text-decoration-color", decoration_color)
    CP("direction", rtl) CP("writing-mode", writing_mode) CP("text-transform", text_transform) CP("letter-spacing", letter_spacing) CP("word-spacing", word_spacing)
    CP("text-indent", text_indent) CP("white-space", white_space) CP("text-overflow", text_overflow_ellipsis)
    CP("overflow-wrap", overflow_wrap) CP("word-wrap", overflow_wrap) CP("word-break", word_break)
    if (n == "vertical-align") { d.vertical_align = src.vertical_align; d.vertical_align_len = src.vertical_align_len; return true; }
    CP("list-style-type", list_style) CP("list-style-position", list_inside) CP("list-style-image", list_image)
    CP("flex-direction", flex_direction) CP("flex-wrap", flex_wrap) CP("order", order)
    CP("flex-grow", flex_grow) CP("flex-shrink", flex_shrink) CP("flex-basis", flex_basis)
    CP("justify-content", justify_content) CP("align-content", align_content) CP("align-items", align_items)
    CP("align-self", align_self) CP("justify-items", justify_items) CP("justify-self", justify_self)
    CP("row-gap", row_gap) CP("column-gap", column_gap)
    CP("grid-template-columns", grid_template_columns) CP("grid-template-rows", grid_template_rows)
    CP("grid-auto-columns", grid_auto_columns) CP("grid-auto-rows", grid_auto_rows)
    CP("grid-column-start", grid_column_start) CP("grid-column-end", grid_column_end)
    CP("grid-row-start", grid_row_start) CP("grid-row-end", grid_row_end)
    CP("border-collapse", border_collapse) CP("table-layout", table_fixed)
    if (n == "border-spacing") { d.border_spacing_h = src.border_spacing_h; d.border_spacing_v = src.border_spacing_v; return true; }
    CP("cursor", cursor) CP("pointer-events", pointer_events)
    CP("transition", transitions) CP("accent-color", accent) CP("aspect-ratio", aspect_ratio) CP("container-type", container_type) CP("container-name", container_name) CP("outline-offset", outline_offset)
    if (n == "transform") { d.translate_x = src.translate_x; d.translate_y = src.translate_y; std::copy(src.tf, src.tf + 4, d.tf); d.tfn = src.tfn; return true; }
    if (n == "transform-origin") { d.origin_x = src.origin_x; d.origin_y = src.origin_y; return true; }
    CP("animation", animations)
    if (n == "outline") { d.outline_style = src.outline_style; d.outline_auto = src.outline_auto; d.outline_w = src.outline_w; d.outline_color = src.outline_color; d.outline_current = src.outline_current; return true; }
#undef CP
    return false;
}

void ComputedStyle::rebuild_transform() {
    const float rad = 3.14159265358979f / 180.f;
    float L[4] = {1, 0, 0, 1};
    Len tx = Len::Px(0), ty = Len::Px(0);
    for (const TfFn& f : tfn) {
        float N[4] = {1, 0, 0, 1};
        Len x = Len::Px(0), y = Len::Px(0);
        switch (f.kind) {
        case 'T': x = f.x; y = f.y; break;
        case 'S': N[0] = f.a; N[3] = f.b; break;
        case 'R': N[0] = N[3] = std::cos(f.a * rad); N[1] = std::sin(f.a * rad); N[2] = -N[1]; break;
        case 'K': N[2] = std::tan(f.a * rad); N[1] = std::tan(f.b * rad); break;
        case 'M': N[0] = f.a; N[1] = f.b; N[2] = f.c; N[3] = f.d; x = Len::Px(f.e); y = Len::Px(f.f); break;
        default: break;   // 3D functions: the painter builds the projective matrix from `tfn`
        }
        // total = total * this: the new translation goes through the linear part so far
        tx.px += L[0] * x.px + L[2] * y.px; tx.pct += L[0] * x.pct + L[2] * y.pct;
        ty.px += L[1] * x.px + L[3] * y.px; ty.pct += L[1] * x.pct + L[3] * y.pct;
        const float R[4] = {L[0] * N[0] + L[2] * N[1], L[1] * N[0] + L[3] * N[1], L[0] * N[2] + L[2] * N[3], L[1] * N[2] + L[3] * N[3]};
        std::copy(R, R + 4, L);
    }
    tx.pct_unit = tx.pct != 0; ty.pct_unit = ty.pct != 0;
    translate_x = tx; translate_y = ty;
    std::copy(L, L + 4, tf);
}

namespace {
// @supports (property: value): the property table says whether it takes the value
const bool kSupportsHookSet = [] {
    set_supports_hook([](const std::string& name, const std::string& value) {
        ComputedStyle s;
        Env env;
        return apply_property(s, env, name, value);
    });
    return true;
}();
} // namespace

} // namespace luxium::style
