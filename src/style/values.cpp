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
        // shortcut: cq* units use the viewport, not the container, until container queries exist
        else if (unit == "vw" || unit == "dvw" || unit == "svw" || unit == "lvw" || unit == "cqw" || unit == "cqi") r.px = n * env.vw / 100.f;
        else if (unit == "vh" || unit == "dvh" || unit == "svh" || unit == "lvh" || unit == "cqh" || unit == "cqb") r.px = n * env.vh / 100.f;
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

bool parse_color(const std::string& raw, uint32_t& out, bool* is_current) {
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

} // namespace luxium::style
