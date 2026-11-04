#include "style/values.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string_view>
#include <utility>

namespace luxium::style {

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> tokens(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    auto flush = [&] { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
    for (char c : v) {
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') ++depth;
        if (c == ')') depth = std::max(0, depth - 1);
        if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) { flush(); continue; }
        if (depth == 0 && (c == '/' || c == ',')) { flush(); out.push_back(std::string(1, c)); continue; }
        cur += c;
    }
    flush();
    return out;
}

std::vector<std::string> split_commas(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (char c : v) {
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') ++depth;
        if (c == ')') depth = std::max(0, depth - 1);
        if (c == ',' && depth == 0) { out.push_back(trim_ws(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trim_ws(cur).empty()) out.push_back(trim_ws(cur));
    return out;
}

bool parse_number(const std::string& tok, float& out) {
    if (tok.empty()) return false;
    try {
        size_t used = 0;
        out = std::stof(tok, &used);
        return used == tok.size();
    } catch (...) { return false; }
}

// ── lengths and calc() ──────────────────────────────────────────────────────

namespace {

struct CV {              // calc value: number, or length (px + pct)
    bool is_num = true;
    float num = 0;
    float px = 0, pct = 0;
    unsigned char fn = 0;          // min/max/clamp that mixes % and px (see Len::fn)
    float px2 = 0, pct2 = 0, px3 = 0, pct3 = 0;
};

struct CalcParser {
    const std::string& s;
    const Env& env;
    size_t i = 0;
    bool ok = true;

    CalcParser(const std::string& text, const Env& e) : s(text), env(e) {}

    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }

    CV expr() {
        CV a = term();
        while (ok) {
            ws();
            if (i >= s.size() || (s[i] != '+' && s[i] != '-')) break;
            const char op = s[i++];
            CV b = term();
            if (a.is_num != b.is_num || a.fn || b.fn) { ok = false; return a; }
            const float sg = op == '+' ? 1.f : -1.f;
            if (a.is_num) a.num += sg * b.num;
            else { a.px += sg * b.px; a.pct += sg * b.pct; }
        }
        return a;
    }

    CV term() {
        CV a = factor();
        while (ok) {
            ws();
            if (i >= s.size() || (s[i] != '*' && s[i] != '/')) break;
            const char op = s[i++];
            CV b = factor();
            if (a.fn || b.fn) { ok = false; return a; }
            if (op == '*') {
                if (a.is_num && b.is_num) a.num *= b.num;
                else if (a.is_num) { b.px *= a.num; b.pct *= a.num; a = b; }
                else if (b.is_num) { a.px *= b.num; a.pct *= b.num; }
                else { ok = false; return a; }
            } else {
                if (!b.is_num || b.num == 0) { ok = false; return a; }
                if (a.is_num) a.num /= b.num;
                else { a.px /= b.num; a.pct /= b.num; }
            }
        }
        return a;
    }

    CV factor() {
        ws();
        CV r;
        if (i >= s.size()) { ok = false; return r; }
        if (s[i] == '(') {
            ++i;
            r = expr();
            ws();
            if (i < s.size() && s[i] == ')') ++i; else ok = false;
            return r;
        }
        // function: calc( min( max( clamp(
        if (std::isalpha(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && (std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '-')) ++j;
            const std::string fn = lower_ascii(s.substr(i, j - i));
            if (j < s.size() && s[j] == '(' && (fn == "calc" || fn == "min" || fn == "max" || fn == "clamp")) {
                i = j + 1;
                std::vector<CV> args;
                while (ok) {
                    args.push_back(expr());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    break;
                }
                ws();
                if (i < s.size() && s[i] == ')') ++i; else ok = false;
                if (!ok) return r;
                if (fn == "calc") return args.empty() ? r : args[0];
                // min/max/clamp: representable only when everything is a pure px/number
                bool mixed = false;
                for (const auto& a : args) mixed = mixed || a.pct != 0.f || a.fn;
                if (mixed) {   // needs the containing block at layout time: keep the terms
                    for (const auto& a : args) if (a.is_num || a.fn) { ok = false; return r; }
                    if ((fn == "clamp" && args.size() != 3) || (fn != "clamp" && args.size() != 2)) { ok = false; return r; }
                    r.is_num = false;
                    r.fn = fn == "min" ? 1 : fn == "max" ? 2 : 3;
                    r.px = args[0].px; r.pct = args[0].pct; r.px2 = args[1].px; r.pct2 = args[1].pct;
                    if (args.size() == 3) { r.px3 = args[2].px; r.pct3 = args[2].pct; }
                    return r;
                }
                auto val = [](const CV& a) { return a.is_num ? a.num : a.px; };
                const bool num = std::all_of(args.begin(), args.end(), [](const CV& a) { return a.is_num; });
                float v = val(args[0]);
                if (fn == "min") for (const auto& a : args) v = std::min(v, val(a));
                else if (fn == "max") for (const auto& a : args) v = std::max(v, val(a));
                else if (args.size() == 3) v = std::max(val(args[0]), std::min(val(args[1]), val(args[2])));
                else ok = false;
                r.is_num = num;
                r.num = num ? v : 0;
                r.px = num ? 0 : v;
                return r;
            }
        }
        // number with optional unit / percent
        size_t j = i;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
        while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
        if (j == i) { ok = false; return r; }
        float n = 0;
        try { n = std::stof(s.substr(i, j - i)); } catch (...) { ok = false; return r; }
        size_t k = j;
        while (k < s.size() && (std::isalpha(static_cast<unsigned char>(s[k])) || s[k] == '%')) ++k;
        const std::string unit = lower_ascii(s.substr(j, k - j));
        i = k;
        if (unit.empty()) { r.is_num = true; r.num = n; return r; }
        r.is_num = false;
        if (unit == "%") r.pct = n;
        else if (unit == "px") r.px = n;
        else if (unit == "em") r.px = n * env.em;
        else if (unit == "rem") r.px = n * env.root_em;
        else if (unit == "ex") r.px = n * env.em * 0.5f;
        else if (unit == "ch") r.px = n * env.em * 0.636f;   // DejaVu "0"
        else if (unit == "vw" || unit == "dvw" || unit == "svw" || unit == "lvw") r.px = n * env.vw / 100.f;
        else if (unit == "vh" || unit == "dvh" || unit == "svh" || unit == "lvh") r.px = n * env.vh / 100.f;
        // container units: the nearest query container, or the (small) viewport when there is none
        else if (unit == "cqw" || unit == "cqi") r.px = n * (env.cqw >= 0 ? env.cqw : env.vw) / 100.f;
        else if (unit == "cqh" || unit == "cqb") r.px = n * (env.cqh >= 0 ? env.cqh : env.vh) / 100.f;
        else if (unit == "cqmin" || unit == "cqmax") {
            const float w = env.cqw >= 0 ? env.cqw : env.vw, h = env.cqh >= 0 ? env.cqh : env.vh;
            r.px = n * (unit == "cqmin" ? std::min(w, h) : std::max(w, h)) / 100.f;
        }
        else if (unit == "vmin") r.px = n * std::min(env.vw, env.vh) / 100.f;
        else if (unit == "vmax") r.px = n * std::max(env.vw, env.vh) / 100.f;
        else if (unit == "pt") r.px = n * 96.f / 72.f;
        else if (unit == "pc") r.px = n * 16.f;
        else if (unit == "in") r.px = n * 96.f;
        else if (unit == "cm") r.px = n * 96.f / 2.54f;
        else if (unit == "mm") r.px = n * 96.f / 25.4f;
        else if (unit == "q") r.px = n * 96.f / 101.6f;
        else ok = false;
        return r;
    }
};

} // namespace

bool parse_length(const std::string& raw, const Env& env, Len& out, bool allow_auto) {
    const std::string tok = lower_ascii(trim_ws(raw));
    if (tok.empty()) return false;
    if (tok == "auto") { if (!allow_auto) return false; out = Len::Auto(); return true; }
    CalcParser p(tok, env);
    CV v = p.expr();
    p.ws();
    if (!p.ok || p.i != tok.size()) return false;
    if (v.is_num) {
        if (v.num != 0.f) return false;   // unitless non-zero is not a length
        out = Len::Px(0);
        return true;
    }
    out = Len::Px(v.px);
    out.pct = v.pct;
    out.fn = v.fn; out.px2 = v.px2; out.pct2 = v.pct2; out.px3 = v.px3; out.pct3 = v.pct3;
    out.pct_unit = tok.find('%') != std::string::npos;
    return true;
}

bool parse_angle(const std::string& raw, float& deg) {
    const std::string t = lower_ascii(trim_ws(raw));
    try {
        size_t used = 0;
        const float n = std::stof(t, &used);
        const std::string u = t.substr(used);
        if (u == "deg" || (u.empty() && n == 0)) deg = n;
        else if (u == "rad") deg = n * 180.f / 3.14159265f;
        else if (u == "turn") deg = n * 360.f;
        else if (u == "grad") deg = n * 0.9f;
        else return false;
        return true;
    } catch (...) { return false; }
}

// ── colors ──────────────────────────────────────────────────────────────────

// CSS Color 4 named colors.
static const std::pair<std::string_view, uint32_t> kNamedColors[] = {
    {"aliceblue", 0xFFF0F8FF}, {"antiquewhite", 0xFFFAEBD7}, {"aqua", 0xFF00FFFF}, {"aquamarine", 0xFF7FFFD4},
    {"azure", 0xFFF0FFFF}, {"beige", 0xFFF5F5DC}, {"bisque", 0xFFFFE4C4}, {"black", 0xFF000000},
    {"blanchedalmond", 0xFFFFEBCD}, {"blue", 0xFF0000FF}, {"blueviolet", 0xFF8A2BE2}, {"brown", 0xFFA52A2A},
    {"burlywood", 0xFFDEB887}, {"cadetblue", 0xFF5F9EA0}, {"chartreuse", 0xFF7FFF00}, {"chocolate", 0xFFD2691E},
    {"coral", 0xFFFF7F50}, {"cornflowerblue", 0xFF6495ED}, {"cornsilk", 0xFFFFF8DC}, {"crimson", 0xFFDC143C},
    {"cyan", 0xFF00FFFF}, {"darkblue", 0xFF00008B}, {"darkcyan", 0xFF008B8B}, {"darkgoldenrod", 0xFFB8860B},
    {"darkgray", 0xFFA9A9A9}, {"darkgreen", 0xFF006400}, {"darkgrey", 0xFFA9A9A9}, {"darkkhaki", 0xFFBDB76B},
    {"darkmagenta", 0xFF8B008B}, {"darkolivegreen", 0xFF556B2F}, {"darkorange", 0xFFFF8C00}, {"darkorchid", 0xFF9932CC},
    {"darkred", 0xFF8B0000}, {"darksalmon", 0xFFE9967A}, {"darkseagreen", 0xFF8FBC8F}, {"darkslateblue", 0xFF483D8B},
    {"darkslategray", 0xFF2F4F4F}, {"darkslategrey", 0xFF2F4F4F}, {"darkturquoise", 0xFF00CED1}, {"darkviolet", 0xFF9400D3},
    {"deeppink", 0xFFFF1493}, {"deepskyblue", 0xFF00BFFF}, {"dimgray", 0xFF696969}, {"dimgrey", 0xFF696969},
    {"dodgerblue", 0xFF1E90FF}, {"firebrick", 0xFFB22222}, {"floralwhite", 0xFFFFFAF0}, {"forestgreen", 0xFF228B22},
    {"fuchsia", 0xFFFF00FF}, {"gainsboro", 0xFFDCDCDC}, {"ghostwhite", 0xFFF8F8FF}, {"gold", 0xFFFFD700},
    {"goldenrod", 0xFFDAA520}, {"gray", 0xFF808080}, {"green", 0xFF008000}, {"greenyellow", 0xFFADFF2F},
    {"grey", 0xFF808080}, {"honeydew", 0xFFF0FFF0}, {"hotpink", 0xFFFF69B4}, {"indianred", 0xFFCD5C5C},
    {"indigo", 0xFF4B0082}, {"ivory", 0xFFFFFFF0}, {"khaki", 0xFFF0E68C}, {"lavender", 0xFFE6E6FA},
    {"lavenderblush", 0xFFFFF0F5}, {"lawngreen", 0xFF7CFC00}, {"lemonchiffon", 0xFFFFFACD}, {"lightblue", 0xFFADD8E6},
    {"lightcoral", 0xFFF08080}, {"lightcyan", 0xFFE0FFFF}, {"lightgoldenrodyellow", 0xFFFAFAD2}, {"lightgray", 0xFFD3D3D3},
    {"lightgreen", 0xFF90EE90}, {"lightgrey", 0xFFD3D3D3}, {"lightpink", 0xFFFFB6C1}, {"lightsalmon", 0xFFFFA07A},
    {"lightseagreen", 0xFF20B2AA}, {"lightskyblue", 0xFF87CEFA}, {"lightslategray", 0xFF778899}, {"lightslategrey", 0xFF778899},
    {"lightsteelblue", 0xFFB0C4DE}, {"lightyellow", 0xFFFFFFE0}, {"lime", 0xFF00FF00}, {"limegreen", 0xFF32CD32},
    {"linen", 0xFFFAF0E6}, {"magenta", 0xFFFF00FF}, {"maroon", 0xFF800000}, {"mediumaquamarine", 0xFF66CDAA},
    {"mediumblue", 0xFF0000CD}, {"mediumorchid", 0xFFBA55D3}, {"mediumpurple", 0xFF9370DB}, {"mediumseagreen", 0xFF3CB371},
    {"mediumslateblue", 0xFF7B68EE}, {"mediumspringgreen", 0xFF00FA9A}, {"mediumturquoise", 0xFF48D1CC}, {"mediumvioletred", 0xFFC71585},
    {"midnightblue", 0xFF191970}, {"mintcream", 0xFFF5FFFA}, {"mistyrose", 0xFFFFE4E1}, {"moccasin", 0xFFFFE4B5},
    {"navajowhite", 0xFFFFDEAD}, {"navy", 0xFF000080}, {"oldlace", 0xFFFDF5E6}, {"olive", 0xFF808000},
    {"olivedrab", 0xFF6B8E23}, {"orange", 0xFFFFA500}, {"orangered", 0xFFFF4500}, {"orchid", 0xFFDA70D6},
    {"palegoldenrod", 0xFFEEE8AA}, {"palegreen", 0xFF98FB98}, {"paleturquoise", 0xFFAFEEEE}, {"palevioletred", 0xFFDB7093},
    {"papayawhip", 0xFFFFEFD5}, {"peachpuff", 0xFFFFDAB9}, {"peru", 0xFFCD853F}, {"pink", 0xFFFFC0CB},
    {"plum", 0xFFDDA0DD}, {"powderblue", 0xFFB0E0E6}, {"purple", 0xFF800080}, {"rebeccapurple", 0xFF663399},
    {"red", 0xFFFF0000}, {"rosybrown", 0xFFBC8F8F}, {"royalblue", 0xFF4169E1}, {"saddlebrown", 0xFF8B4513},
    {"salmon", 0xFFFA8072}, {"sandybrown", 0xFFF4A460}, {"seagreen", 0xFF2E8B57}, {"seashell", 0xFFFFF5EE},
    {"sienna", 0xFFA0522D}, {"silver", 0xFFC0C0C0}, {"skyblue", 0xFF87CEEB}, {"slateblue", 0xFF6A5ACD},
    {"slategray", 0xFF708090}, {"slategrey", 0xFF708090}, {"snow", 0xFFFFFAFA}, {"springgreen", 0xFF00FF7F},
    {"steelblue", 0xFF4682B4}, {"tan", 0xFFD2B48C}, {"teal", 0xFF008080}, {"thistle", 0xFFD8BFD8},
    {"tomato", 0xFFFF6347}, {"turquoise", 0xFF40E0D0}, {"violet", 0xFFEE82EE}, {"wheat", 0xFFF5DEB3},
    {"white", 0xFFFFFFFF}, {"whitesmoke", 0xFFF5F5F5}, {"yellow", 0xFFFFFF00}, {"yellowgreen", 0xFF9ACD32}
};

static bool all_hex(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
        [](unsigned char c) { return std::isxdigit(c) != 0; });
}

static float hue_to_rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 1.f / 2) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

// ── CSS Color 4/5: color spaces, hwb/lab/lch/oklab/oklch/color(), color-mix() ──
namespace {

enum class Space { SRGB, SRGB_LIN, P3, XYZ65, XYZ50, LAB, LCH, OKLAB, OKLCH, HSL, HWB };
using V3 = std::array<float, 3>;

bool space_by_name(const std::string& n, Space& sp) {
    static const std::pair<std::string_view, Space> k[] = {
        {"srgb", Space::SRGB}, {"srgb-linear", Space::SRGB_LIN}, {"display-p3", Space::P3}, {"xyz", Space::XYZ65},
        {"xyz-d65", Space::XYZ65}, {"xyz-d50", Space::XYZ50}, {"lab", Space::LAB}, {"lch", Space::LCH},
        {"oklab", Space::OKLAB}, {"oklch", Space::OKLCH}, {"hsl", Space::HSL}, {"hwb", Space::HWB}};
    for (const auto& [name, s] : k) if (n == name) { sp = s; return true; }
    return false;
}
bool is_polar(Space s) { return s == Space::LCH || s == Space::OKLCH || s == Space::HSL || s == Space::HWB; }
int hue_index(Space s) { return s == Space::HSL || s == Space::HWB ? 0 : 2; }

float to_lin(float c) { const float a = std::fabs(c); return a <= 0.04045f ? c / 12.92f : std::copysign(std::pow((a + 0.055f) / 1.055f, 2.4f), c); }
float to_gam(float c) { const float a = std::fabs(c); return a <= 0.0031308f ? c * 12.92f : std::copysign(1.055f * std::pow(a, 1.f / 2.4f) - 0.055f, c); }
V3 mul(const float m[9], const V3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}
constexpr float kLinToXyz[9] = {0.4123908f, 0.3575843f, 0.1804808f, 0.2126390f, 0.7151687f, 0.0721923f, 0.0193308f, 0.1191948f, 0.9505322f};
constexpr float kXyzToLin[9] = {3.2409699f, -1.5373832f, -0.4986108f, -0.9692436f, 1.8759675f, 0.0415551f, 0.0556301f, -0.2039770f, 1.0569715f};
constexpr float kP3ToXyz[9] = {0.4865709f, 0.2656677f, 0.1982173f, 0.2289746f, 0.6917385f, 0.0792869f, 0.0f, 0.0451134f, 1.0439444f};
constexpr float kXyzToP3[9] = {2.4934969f, -0.9313836f, -0.4027108f, -0.8294890f, 1.7626641f, 0.0236247f, 0.0358458f, -0.0761724f, 0.9568845f};
constexpr float kD65ToD50[9] = {1.0479298f, 0.0229468f, -0.0501922f, 0.0296278f, 0.9904345f, -0.0170738f, -0.0092431f, 0.0150551f, 0.7518743f};
constexpr float kD50ToD65[9] = {0.9554735f, -0.0230985f, 0.0632593f, -0.0283697f, 1.0099955f, 0.0210414f, 0.0123140f, -0.0205077f, 1.3303659f};
constexpr V3 kD50White = {0.3457f / 0.3585f, 1.f, (1.f - 0.3457f - 0.3585f) / 0.3585f};

V3 xyz50_to_lab(const V3& x) {
    constexpr float e = 216.f / 24389.f, k = 24389.f / 27.f;
    float f[3];
    for (int i = 0; i < 3; ++i) { const float t = x[i] / kD50White[i]; f[i] = t > e ? std::cbrt(t) : (k * t + 16.f) / 116.f; }
    return {116.f * f[1] - 16.f, 500.f * (f[0] - f[1]), 200.f * (f[1] - f[2])};
}
V3 lab_to_xyz50(const V3& l) {
    constexpr float e = 216.f / 24389.f, k = 24389.f / 27.f;
    const float fy = (l[0] + 16.f) / 116.f, fx = fy + l[1] / 500.f, fz = fy - l[2] / 200.f;
    const float x = fx * fx * fx > e ? fx * fx * fx : (116.f * fx - 16.f) / k;
    const float y = l[0] > k * e ? fy * fy * fy : l[0] / k;
    const float z = fz * fz * fz > e ? fz * fz * fz : (116.f * fz - 16.f) / k;
    return {x * kD50White[0], y * kD50White[1], z * kD50White[2]};
}
V3 lin_to_oklab(const V3& c) {
    const float l = std::cbrt(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = std::cbrt(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = std::cbrt(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}
V3 oklab_to_lin(const V3& o) {
    float l = o[0] + 0.3963377774f * o[1] + 0.2158037573f * o[2];
    float m = o[0] - 0.1055613458f * o[1] - 0.0638541728f * o[2];
    float s = o[0] - 0.0894841775f * o[1] - 1.2914855480f * o[2];
    l = l * l * l; m = m * m * m; s = s * s * s;
    return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s, -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
            -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
}
V3 to_polar(const V3& v) {   // (L, a, b) -> (L, C, h)
    float h = std::atan2(v[2], v[1]) * 180.f / float(M_PI);
    if (h < 0) h += 360.f;
    return {v[0], std::hypot(v[1], v[2]), h};
}
V3 from_polar(const V3& v) {
    const float r = v[2] * float(M_PI) / 180.f;
    return {v[0], v[1] * std::cos(r), v[1] * std::sin(r)};
}
V3 srgb_to_hsl(const V3& c) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), d = mx - mn, l = (mx + mn) / 2;
    float h = 0, s = 0;
    if (d > 1e-6f) {
        s = l > 0 && l < 1 ? (mx - l) / std::min(l, 1 - l) : 0;
        if (mx == c[0]) h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6 : 0);
        else if (mx == c[1]) h = (c[2] - c[0]) / d + 2;
        else h = (c[0] - c[1]) / d + 4;
        h *= 60.f;
    }
    return {h, s * 100.f, l * 100.f};
}
V3 hsl_to_srgb(const V3& v) {
    const float h = std::fmod(std::fmod(v[0], 360.f) + 360.f, 360.f) / 360.f;
    const float s = std::clamp(v[1] / 100.f, 0.f, 1.f), l = std::clamp(v[2] / 100.f, 0.f, 1.f);
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    return {hue_to_rgb(p, q, h + 1.f / 3), hue_to_rgb(p, q, h), hue_to_rgb(p, q, h - 1.f / 3)};
}
V3 hwb_to_srgb(const V3& v) {
    float w = v[1] / 100.f, b = v[2] / 100.f;
    if (w + b >= 1) { const float g = w / (w + b); return {g, g, g}; }
    V3 c = hsl_to_srgb({v[0], 100.f, 50.f});
    for (float& x : c) x = x * (1 - w - b) + w;
    return c;
}
V3 srgb_to_hwb(const V3& c) {
    const V3 hsl = srgb_to_hsl(c);
    return {hsl[0], std::min({c[0], c[1], c[2]}) * 100.f, (1 - std::max({c[0], c[1], c[2]})) * 100.f};
}

// gamma-encoded sRGB (0..1) <-> any space
V3 from_srgb(const V3& c, Space sp) {
    const V3 lin = {to_lin(c[0]), to_lin(c[1]), to_lin(c[2])};
    switch (sp) {
    case Space::SRGB: return c;
    case Space::SRGB_LIN: return lin;
    case Space::P3: { V3 p = mul(kXyzToP3, mul(kLinToXyz, lin)); for (float& x : p) x = to_gam(x); return p; }
    case Space::XYZ65: return mul(kLinToXyz, lin);
    case Space::XYZ50: return mul(kD65ToD50, mul(kLinToXyz, lin));
    case Space::LAB: return xyz50_to_lab(mul(kD65ToD50, mul(kLinToXyz, lin)));
    case Space::LCH: return to_polar(xyz50_to_lab(mul(kD65ToD50, mul(kLinToXyz, lin))));
    case Space::OKLAB: return lin_to_oklab(lin);
    case Space::OKLCH: return to_polar(lin_to_oklab(lin));
    case Space::HSL: return srgb_to_hsl(c);
    case Space::HWB: return srgb_to_hwb(c);
    }
    return c;
}
V3 to_srgb(const V3& v, Space sp) {
    V3 lin{};
    switch (sp) {
    case Space::SRGB: return v;
    case Space::HSL: return hsl_to_srgb(v);
    case Space::HWB: return hwb_to_srgb(v);
    case Space::SRGB_LIN: lin = v; break;
    case Space::P3: lin = mul(kXyzToLin, mul(kP3ToXyz, {to_lin(v[0]), to_lin(v[1]), to_lin(v[2])})); break;
    case Space::XYZ65: lin = mul(kXyzToLin, v); break;
    case Space::XYZ50: lin = mul(kXyzToLin, mul(kD50ToD65, v)); break;
    case Space::LAB: lin = mul(kXyzToLin, mul(kD50ToD65, lab_to_xyz50(v))); break;
    case Space::LCH: lin = mul(kXyzToLin, mul(kD50ToD65, lab_to_xyz50(from_polar(v)))); break;
    case Space::OKLAB: lin = oklab_to_lin(v); break;
    case Space::OKLCH: lin = oklab_to_lin(from_polar(v)); break;
    }
    return {to_gam(lin[0]), to_gam(lin[1]), to_gam(lin[2])};
}

uint32_t pack(const V3& c, float a) {
    auto q = [](float x) { return uint32_t(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
    return (q(a) << 24) | (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
}
V3 unpack(uint32_t c, float& a) {
    a = float(c >> 24) / 255.f;
    return {float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, float(c & 255) / 255.f};
}

// One numeric component: number, percentage (scaled by pct_ref), angle (hue), or `none` (0).
bool component(const std::string& t, float pct_ref, bool hue, float& out) {
    const std::string s = lower_ascii(t);
    if (s == "none") { out = 0; return true; }
    if (s.ends_with('%')) { float n; if (!parse_number(s.substr(0, s.size() - 1), n)) return false; out = n / 100.f * pct_ref; return true; }
    if (hue) { if (parse_number(s, out)) return true; return parse_angle(s, out); }
    return parse_number(s, out);
}

// hwb() lab() lch() oklab() oklch() color(): space-separated, optional "/ alpha".
bool parse_modern_fn(const std::string& fn, const std::string& inner, uint32_t& out) {
    std::vector<std::string> t;
    for (auto& x : tokens(inner)) if (x != ",") t.push_back(x);
    Space sp;
    if (fn == "color") {
        if (t.empty() || !space_by_name(lower_ascii(t[0]), sp) || is_polar(sp) || sp == Space::LAB || sp == Space::OKLAB) return false;
        t.erase(t.begin());
    } else if (!space_by_name(fn, sp) || sp == Space::SRGB || sp == Space::HSL) return false;
    float alpha = 1.f;
    if (t.size() == 5 && t[3] == "/") { if (!component(t[4], 1.f, false, alpha)) return false; t.resize(3); }
    if (t.size() != 3) return false;
    float ref[3] = {1.f, 1.f, 1.f};
    bool hue[3] = {false, false, false};
    switch (sp) {
    case Space::HWB: ref[1] = ref[2] = 100.f; hue[0] = true; break;
    case Space::LAB: ref[0] = 100.f; ref[1] = ref[2] = 125.f; break;
    case Space::LCH: ref[0] = 100.f; ref[1] = 150.f; hue[2] = true; break;
    case Space::OKLAB: ref[1] = ref[2] = 0.4f; break;
    case Space::OKLCH: ref[1] = 0.4f; hue[2] = true; break;
    default: break;
    }
    V3 v;
    for (int i = 0; i < 3; ++i) if (!component(t[i], ref[i], hue[i], v[i])) return false;
    if (sp == Space::HWB && t[1].back() != '%' && lower_ascii(t[1]) != "none") return false;
    out = pack(to_srgb(v, sp), std::clamp(alpha, 0.f, 1.f));
    return true;
}

// color-mix(in <space> [<hue> hue], <color> [<p>%], <color> [<p>%])
bool parse_color_mix(const std::string& inner, uint32_t& out) {
    const auto args = split_commas(inner);
    if (args.size() != 3) return false;
    const auto how = tokens(args[0]);
    if (how.size() < 2 || lower_ascii(how[0]) != "in") return false;
    Space sp;
    if (!space_by_name(lower_ascii(how[1]), sp)) return false;
    std::string hue_method = "shorter";
    if (how.size() == 4 && lower_ascii(how[3]) == "hue" && is_polar(sp)) hue_method = lower_ascii(how[2]);
    else if (how.size() != 2) return false;

    uint32_t col[2];
    float pct[2] = {-1.f, -1.f};
    for (int i = 0; i < 2; ++i) {
        auto t = tokens(args[size_t(i) + 1]);
        if (t.empty() || t.size() > 2) return false;
        size_t ci = 0;
        if (t.size() == 2) {
            const size_t pi = t[0].ends_with('%') ? 0 : 1;
            ci = 1 - pi;
            if (!t[pi].ends_with('%') || !parse_number(t[pi].substr(0, t[pi].size() - 1), pct[i])) return false;
            if (pct[i] < 0 || pct[i] > 100) return false;
        }
        bool cur = false;
        if (!parse_color(t[ci], col[i], &cur) || cur) return false;   // currentColor would need the element: unsupported
    }
    if (pct[0] < 0 && pct[1] < 0) pct[0] = pct[1] = 50.f;
    else if (pct[0] < 0) pct[0] = 100.f - pct[1];
    else if (pct[1] < 0) pct[1] = 100.f - pct[0];
    const float sum = pct[0] + pct[1];
    if (sum <= 0) return false;
    const float mult = std::min(sum, 100.f) / 100.f, w1 = pct[1] / sum, w0 = 1.f - w1;

    float a[2];
    V3 v[2];
    for (int i = 0; i < 2; ++i) v[i] = from_srgb(unpack(col[i], a[i]), sp);
    const int hi = is_polar(sp) ? hue_index(sp) : -1;
    if (hi >= 0) {
        // powerless hue (achromatic or transparent) takes the other colour's
        auto achromatic = [&](int i) {
            if (a[i] == 0) return true;
            if (sp == Space::HWB) return v[i][1] + v[i][2] >= 99.99f;
            return std::fabs(v[i][1]) < (sp == Space::OKLCH ? 1e-4f : 1e-2f);   // saturation / chroma
        };
        if (achromatic(0) && !achromatic(1)) v[0][hi] = v[1][hi];
        else if (achromatic(1) && !achromatic(0)) v[1][hi] = v[0][hi];
        float h0 = v[0][hi], h1 = v[1][hi], d = h1 - h0;
        if (hue_method == "shorter") { if (d > 180) h0 += 360; else if (d < -180) h1 += 360; }
        else if (hue_method == "longer") { if (d > 0 && d < 180) h0 += 360; else if (d > -180 && d <= 0) h1 += 360; }
        else if (hue_method == "increasing") { if (d < 0) h1 += 360; }
        else if (hue_method == "decreasing") { if (d > 0) h0 += 360; }
        else return false;
        v[0][hi] = h0; v[1][hi] = h1;
    }
    const float am = a[0] * w0 + a[1] * w1;
    V3 m{};
    for (int k = 0; k < 3; ++k) {
        if (k == hi) { m[k] = v[0][k] * w0 + v[1][k] * w1; continue; }   // hue is not premultiplied
        const float p = v[0][k] * a[0] * w0 + v[1][k] * a[1] * w1;
        m[k] = am > 0 ? p / am : 0.f;
    }
    out = pack(to_srgb(m, sp), am * mult);
    return true;
}

} // namespace

bool parse_color(const std::string& raw, uint32_t& out, bool* is_current) {
    {
        const std::string t = trim_ws(raw);
        const size_t open = t.find('(');
        if (open != std::string::npos && open > 0 && t.back() == ')') {
            const std::string fn = lower_ascii(t.substr(0, open));
            const std::string inner = t.substr(open + 1, t.size() - open - 2);
            if (fn == "color-mix" || fn == "hwb" || fn == "lab" || fn == "lch" || fn == "oklab" || fn == "oklch" || fn == "color") {
                if (is_current) *is_current = false;
                return fn == "color-mix" ? parse_color_mix(inner, out) : parse_modern_fn(fn, inner, out);
            }
        }
    }
    std::string s;
    for (char ch : raw)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s.empty()) return false;
    if (is_current) *is_current = false;
    if (s == "transparent") { out = 0; return true; }
    if (s == "currentcolor") { out = 0; if (is_current) *is_current = true; return true; }
    if (s[0] == '#') {
        std::string h = s.substr(1);
        if (!all_hex(h)) return false;
        if (h.size() == 3 || h.size() == 4) {
            std::string d;
            for (char ch : h) { d += ch; d += ch; }
            h = d;
        }
        if (h.size() == 6) { out = 0xFF000000u | static_cast<uint32_t>(std::stoul(h, nullptr, 16)); return true; }
        if (h.size() == 8) {
            const uint32_t rgba = static_cast<uint32_t>(std::stoul(h, nullptr, 16));
            out = (rgba << 24) | (rgba >> 8);
            return true;
        }
        return false;
    }
    const bool is_rgb = s.starts_with("rgb(") || s.starts_with("rgba(");
    const bool is_hsl = s.starts_with("hsl(") || s.starts_with("hsla(");
    if (is_rgb || is_hsl) {
        std::vector<std::pair<float, bool>> nums;
        std::string cur;
        auto push = [&](bool pct) {
            if (cur.empty()) return;
            try { nums.push_back({std::stof(cur), pct}); } catch (...) {}
            cur.clear();
        };
        for (size_t i = s.find('(') + 1; i < s.size(); ++i) {
            const char ch = s[i];
            if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '+') cur += ch;
            else if (ch == '%') push(true);
            else push(false);   // separators and unit suffixes (deg, turn...)
        }
        push(false);
        if (nums.size() < 3) return false;
        float a = 1.f;
        if (nums.size() > 3) a = std::clamp(nums[3].second ? nums[3].first / 100.f : nums[3].first, 0.f, 1.f);
        float r, g, b;
        if (is_rgb) {
            auto ch = [](std::pair<float, bool> n) {
                return std::clamp(n.second ? n.first * 2.55f : n.first, 0.f, 255.f);
            };
            r = ch(nums[0]); g = ch(nums[1]); b = ch(nums[2]);
        } else {
            const float h = std::fmod(std::fmod(nums[0].first, 360.f) + 360.f, 360.f) / 360.f;
            const float sat = std::clamp(nums[1].first / 100.f, 0.f, 1.f);
            const float l = std::clamp(nums[2].first / 100.f, 0.f, 1.f);
            const float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat;
            const float p = 2 * l - q;
            r = 255.f * hue_to_rgb(p, q, h + 1.f / 3);
            g = 255.f * hue_to_rgb(p, q, h);
            b = 255.f * hue_to_rgb(p, q, h - 1.f / 3);
        }
        out = (uint32_t(std::lround(a * 255.f)) << 24) | (uint32_t(std::lround(r)) << 16) |
              (uint32_t(std::lround(g)) << 8) | uint32_t(std::lround(b));
        return true;
    }
    for (const auto& [n, c] : kNamedColors)
        if (s == n) { out = c; return true; }
    return false;
}

// ── gradients ───────────────────────────────────────────────────────────────

bool parse_gradient(const std::string& value, const Env& env, uint32_t current, Gradient& g) {
    std::string v = trim_ws(value);
    const std::string lv = lower_ascii(v);
    size_t open = v.find('(');
    if (open == std::string::npos || v.back() != ')') return false;
    const std::string fn = lv.substr(0, open);
    g = Gradient{};
    g.repeating = fn.starts_with("repeating-");
    if (fn == "linear-gradient" || fn == "repeating-linear-gradient") g.radial = false;
    else if (fn == "radial-gradient" || fn == "repeating-radial-gradient") g.radial = true;
    else return false;

    auto args = split_commas(v.substr(open + 1, v.size() - open - 2));
    if (args.empty()) return false;
    size_t first_stop = 0;

    if (!g.radial) {
        g.angle = 180.f;
        const std::string a0 = lower_ascii(args[0]);
        float deg;
        if (parse_angle(a0, deg)) { g.angle = deg; first_stop = 1; }
        else if (a0.starts_with("to ")) {
            const auto t = tokens(a0.substr(3));
            bool top = false, bottom = false, left = false, right = false;
            for (const auto& w : t) {
                if (w == "top") top = true; else if (w == "bottom") bottom = true;
                else if (w == "left") left = true; else if (w == "right") right = true;
            }
            // corner directions depend on the box; store the common axis angles,
            // corners as the diagonal's nominal 45deg-multiples (box aspect fixed up by layout)
            if (top && right) { g.angle = 45; g.corner = 1; }
            else if (bottom && right) { g.angle = 135; g.corner = 2; }
            else if (bottom && left) { g.angle = 225; g.corner = 3; }
            else if (top && left) { g.angle = 315; g.corner = 4; }
            else if (top) g.angle = 0;
            else if (right) g.angle = 90;
            else if (bottom) g.angle = 180;
            else if (left) g.angle = 270;
            first_stop = 1;
        }
    } else {
        const std::string a0 = lower_ascii(args[0]);
        bool is_prelude = a0.find("circle") != std::string::npos || a0.find("ellipse") != std::string::npos ||
                          a0.find("closest") != std::string::npos || a0.find("farthest") != std::string::npos ||
                          a0.starts_with("at ") || (!a0.empty() && std::isdigit(static_cast<unsigned char>(a0[0])) && a0.find(" at ") != std::string::npos);
        if (is_prelude) {
            first_stop = 1;
            g.circle = a0.find("circle") != std::string::npos;
            if (a0.find("closest-side") != std::string::npos) g.extent = 1;
            else if (a0.find("farthest-side") != std::string::npos) g.extent = 2;
            else if (a0.find("closest-corner") != std::string::npos) g.extent = 3;
            const size_t at = a0.find("at ");
            if (at != std::string::npos) {
                const auto t = tokens(a0.substr(at + 3));
                auto pos1 = [&](const std::string& w, bool horiz, Len& out) {
                    if (w == "left" || w == "top") out = Len::Pct(0);
                    else if (w == "right" || w == "bottom") out = Len::Pct(100);
                    else if (w == "center") out = Len::Pct(50);
                    else { Len l; if (parse_length(w, env, l)) out = l; }
                    (void)horiz;
                };
                if (t.size() == 1) {
                    if (t[0] == "top" || t[0] == "bottom") { pos1("center", true, g.cx); pos1(t[0], false, g.cy); }
                    else { pos1(t[0], true, g.cx); pos1("center", false, g.cy); }
                } else if (t.size() >= 2) {
                    if (t[0] == "top" || t[0] == "bottom") { pos1(t[1], true, g.cx); pos1(t[0], false, g.cy); }
                    else { pos1(t[0], true, g.cx); pos1(t[1], false, g.cy); }
                }
            }
        }
    }

    for (size_t k = first_stop; k < args.size(); ++k) {
        const auto t = tokens(args[k]);
        if (t.empty()) continue;
        GradStop st;
        bool cur = false;
        // color first, then up to two positions
        std::string colortok = t[0];
        size_t pi = 1;
        if (!parse_color(colortok, st.color, &cur)) {
            // maybe "10% red" (position first)
            if (t.size() >= 2 && parse_color(t.back(), st.color, &cur)) { colortok = t.back(); pi = 0; }
            else return false;
        }
        if (cur) st.color = current;
        st.pos = Len::Auto();
        std::vector<Len> ps;
        for (size_t q = pi; q < t.size(); ++q) {
            if (t[q] == colortok) continue;
            Len l;
            if (parse_length(t[q], env, l)) ps.push_back(l);
        }
        if (!ps.empty()) { st.pos = ps[0]; }
        g.stops.push_back(st);
        if (ps.size() >= 2) { GradStop s2 = st; s2.pos = ps[1]; g.stops.push_back(s2); }
    }
    return g.stops.size() >= 2;
}

// ── shadows ─────────────────────────────────────────────────────────────────

bool parse_shadow_list(const std::string& value, const Env& env, uint32_t current,
                       std::vector<Shadow>& out) {
    out.clear();
    if (lower_ascii(trim_ws(value)) == "none") return true;
    for (const auto& item : split_commas(value)) {
        Shadow sh;
        sh.color = current;
        std::vector<float> nums;
        for (const auto& t : tokens(item)) {
            if (lower_ascii(t) == "inset") { sh.inset = true; continue; }
            uint32_t c; bool cur = false;
            if (parse_color(t, c, &cur)) { sh.color = cur ? current : c; continue; }
            Len l;
            if (parse_length(t, env, l)) nums.push_back(l.px);
            else return false;
        }
        if (nums.size() < 2) return false;
        sh.x = nums[0]; sh.y = nums[1];
        if (nums.size() > 2) sh.blur = std::max(0.f, nums[2]);
        if (nums.size() > 3) sh.spread = nums[3];
        out.push_back(sh);
    }
    return true;
}

bool parse_filter_list(const std::string& value, const Env& env, uint32_t current, std::vector<FilterFn>& out) {
    out.clear();
    for (const auto& tk : tokens(value)) {
        const size_t open = tk.find('(');
        if (open == std::string::npos || tk.back() != ')') return false;
        const std::string fn = lower_ascii(tk.substr(0, open));
        const std::string arg = trim_ws(tk.substr(open + 1, tk.size() - open - 2));
        FilterFn f;
        // amount: number or percentage; `def` when empty
        auto amount = [&](float def, float& a) {
            if (arg.empty()) { a = def; return true; }
            const std::string l = lower_ascii(arg);
            if (l.ends_with('%')) { if (!parse_number(l.substr(0, l.size() - 1), a)) return false; a /= 100.f; }
            else if (!parse_number(l, a)) return false;
            return a >= 0;
        };
        using K = FilterFn::Kind;
        if (fn == "blur") {
            f.kind = K::Blur;
            Len l;
            if (!arg.empty()) { if (!parse_length(arg, env, l) || l.pct != 0 || l.px < 0) return false; f.a = l.px; }
        } else if (fn == "brightness") { f.kind = K::Brightness; if (!amount(1, f.a)) return false; }
        else if (fn == "contrast") { f.kind = K::Contrast; if (!amount(1, f.a)) return false; }
        else if (fn == "grayscale") { f.kind = K::Grayscale; if (!amount(1, f.a)) return false; f.a = std::min(f.a, 1.f); }
        else if (fn == "sepia") { f.kind = K::Sepia; if (!amount(1, f.a)) return false; f.a = std::min(f.a, 1.f); }
        else if (fn == "saturate") { f.kind = K::Saturate; if (!amount(1, f.a)) return false; }
        else if (fn == "invert") { f.kind = K::Invert; if (!amount(1, f.a)) return false; f.a = std::min(f.a, 1.f); }
        else if (fn == "opacity") { f.kind = K::Opacity; if (!amount(1, f.a)) return false; f.a = std::min(f.a, 1.f); }
        else if (fn == "hue-rotate") {
            f.kind = K::HueRotate;
            if (!arg.empty() && !parse_angle(arg, f.a)) { float n; if (!parse_number(arg, n) || n != 0) return false; f.a = 0; }
        } else if (fn == "drop-shadow") {
            std::vector<Shadow> sh;
            if (!parse_shadow_list(arg, env, current, sh) || sh.size() != 1 || sh[0].inset || sh[0].spread != 0) return false;
            f.kind = K::DropShadow; f.dx = sh[0].x; f.dy = sh[0].y; f.blur = sh[0].blur; f.color = sh[0].color;
        } else return false;
        out.push_back(f);
    }
    return !out.empty();
}

} // namespace luxium::style
