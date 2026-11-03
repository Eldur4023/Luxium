#include "render/svg.hpp"

#include "render/images.hpp"
#include "style/values.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace luxium::render::svg {

namespace {

constexpr float kPi = 3.14159265358979f;

// ── the element tree (tags and attribute names lower-cased, entities decoded) ─────────────────────────────

struct Node {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> kids;
    std::string text;      // concatenated character data (for <style>)
    const std::string* attr(const char* k) const {
        for (const auto& a : attrs) if (a.first == k) return &a.second;
        return nullptr;
    }
    std::string str(const char* k, const std::string& d = {}) const { const auto* v = attr(k); return v ? *v : d; }
};

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string decode_entities(const std::string& in) {
    if (in.find('&') == std::string::npos) return in;
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { out += in[i]; continue; }
        const size_t e = in.find(';', i);
        if (e == std::string::npos || e - i > 8) { out += in[i]; continue; }
        const std::string n = in.substr(i + 1, e - i - 1);
        if (n == "amp") out += '&'; else if (n == "lt") out += '<'; else if (n == "gt") out += '>';
        else if (n == "quot") out += '"'; else if (n == "apos") out += '\'';
        else if (!n.empty() && n[0] == '#') {
            const long cp = n.size() > 1 && (n[1] == 'x' || n[1] == 'X') ? std::strtol(n.c_str() + 2, nullptr, 16) : std::strtol(n.c_str() + 1, nullptr, 10);
            if (cp > 0 && cp < 128) out += char(cp);
            else { out += '?'; }
        } else { out += in[i]; continue; }
        i = e;
    }
    return out;
}

struct XmlParser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    bool parse_element(Node& out) {   // at '<name'
        if (++depth > 64) return false;
        ++i;
        size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '>' && s[i] != '/') ++i;
        out.tag = lower(s.substr(b, i - b));
        // attributes
        for (;;) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size()) return false;
            if (s[i] == '/') { i += 2; --depth; return true; }   // "/>"
            if (s[i] == '>') { ++i; break; }
            b = i;
            while (i < s.size() && s[i] != '=' && s[i] != '>' && s[i] != '/' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            const std::string name = lower(s.substr(b, i - b));
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            std::string val;
            if (i < s.size() && s[i] == '=') {
                ++i;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                    const char q = s[i++];
                    b = i;
                    while (i < s.size() && s[i] != q) ++i;
                    val = decode_entities(s.substr(b, i - b));
                    ++i;
                } else {
                    b = i;
                    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '>') ++i;
                    val = s.substr(b, i - b);
                }
            }
            if (!name.empty()) out.attrs.emplace_back(name, val);
            else ++i;
        }
        // content
        for (;;) {
            const size_t lt = s.find('<', i);
            if (lt == std::string::npos) return false;
            out.text += decode_entities(s.substr(i, lt - i));
            i = lt;
            if (s.compare(i, 4, "<!--") == 0) { const size_t e = s.find("-->", i + 4); if (e == std::string::npos) return false; i = e + 3; continue; }
            if (s.compare(i, 9, "<![CDATA[") == 0) { const size_t e = s.find("]]>", i); if (e == std::string::npos) return false; out.text += s.substr(i + 9, e - i - 9); i = e + 3; continue; }
            if (s[i + 1] == '/') { const size_t e = s.find('>', i); if (e == std::string::npos) return false; i = e + 1; --depth; return true; }
            if (s[i + 1] == '?' || s[i + 1] == '!') { const size_t e = s.find('>', i); if (e == std::string::npos) return false; i = e + 1; continue; }
            Node kid;
            if (!parse_element(kid)) return false;
            out.kids.push_back(std::move(kid));
        }
    }
};

void hash_node(uint64_t& h, const Node& n) {
    auto mix = [&](const std::string& x) { for (unsigned char c : x) h = (h ^ c) * 1099511628211ull; h = (h ^ 0xFF) * 1099511628211ull; };
    mix(n.tag);
    for (const auto& a : n.attrs) { mix(a.first); mix(a.second); }
    mix(n.text);
    for (const auto& k : n.kids) hash_node(h, k);
}

void from_dom_rec(const dom::Node& e, Node& out) {
    out.tag = e.name;
    for (const auto& a : e.attrs) out.attrs.emplace_back(lower(a.first), decode_entities(a.second));
    for (const auto& c : e.children) {
        if (c->is_element()) { out.kids.emplace_back(); from_dom_rec(*c, out.kids.back()); }
        else if (c->type == dom::NodeType::Text) out.text += c->text;
    }
}

// ── geometry ────────────────────────────────────────────────────────────────────────────────────────

struct Pt { float x = 0, y = 0; };

struct Mat {   // x' = a x + c y + e, y' = b x + d y + f
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Pt apply(Pt p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Mat operator*(const Mat& o) const {   // this applied after o
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    float det() const { return a * d - b * c; }
    float scale() const { return std::sqrt(std::fabs(det())); }
    Mat inverse() const {
        const float dt = det();
        if (std::fabs(dt) < 1e-12f) return {};
        Mat r;
        r.a = d / dt; r.b = -b / dt; r.c = -c / dt; r.d = a / dt;
        r.e = -(r.a * e + r.c * f); r.f = -(r.b * e + r.d * f);
        return r;
    }
};

struct Number {   // a number list scanner for path data / points / transforms
    const std::string& s;
    size_t i = 0;
    void skip() { while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i; }
    bool more() { skip(); return i < s.size(); }
    bool next(float& v) {
        skip();
        if (i >= s.size()) return false;
        const char* st = s.c_str() + i;
        char* end = nullptr;
        v = std::strtof(st, &end);
        if (end == st) return false;
        i += size_t(end - st);
        return true;
    }
    bool flag(float& v) {   // arc flags may be glued: "a1 1 0 00 1 1"
        skip();
        if (i < s.size() && (s[i] == '0' || s[i] == '1')) { v = s[i] == '1' ? 1.f : 0.f; ++i; return true; }
        return false;
    }
};

Mat parse_transform(const std::string& t) {
    Mat m;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == ',')) ++i;
        const size_t open = t.find('(', i);
        if (open == std::string::npos) break;
        const size_t close = t.find(')', open);
        if (close == std::string::npos) break;
        const std::string fn = lower(trim(t.substr(i, open - i)));
        const std::string args = t.substr(open + 1, close - open - 1);
        Number nr{args};
        float v[6] = {0, 0, 0, 0, 0, 0};
        int n = 0;
        while (n < 6 && nr.next(v[n])) ++n;
        Mat k;
        if (fn == "matrix" && n == 6) k = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (fn == "translate") k.e = v[0], k.f = n > 1 ? v[1] : 0;
        else if (fn == "scale") { k.a = v[0]; k.d = n > 1 ? v[1] : v[0]; }
        else if (fn == "rotate") {
            const float r = v[0] * kPi / 180.f, cs = std::cos(r), sn = std::sin(r);
            k = {cs, sn, -sn, cs, 0, 0};
            if (n == 3) { Mat t1; t1.e = v[1]; t1.f = v[2]; Mat t2; t2.e = -v[1]; t2.f = -v[2]; k = t1 * k * t2; }
        } else if (fn == "skewx") k.c = std::tan(v[0] * kPi / 180.f);
        else if (fn == "skewy") k.b = std::tan(v[0] * kPi / 180.f);
        m = m * k;
        i = close + 1;
    }
    return m;
}

// Flattened subpaths in user space.
struct Poly { std::vector<Pt> pts; bool closed = false; };
using Polys = std::vector<Poly>;

struct PathBuilder {
    Polys out;
    Pt cur, start;
    float tol = 0.1f;
    void move(Pt p) { out.push_back({{p}, false}); cur = start = p; }
    void line(Pt p) { if (out.empty() || out.back().closed) move(cur); out.back().pts.push_back(p); cur = p; }
    void close() { if (!out.empty()) { out.back().closed = true; cur = start; } }
    void cubic(Pt c1, Pt c2, Pt p) {
        if (out.empty() || out.back().closed) move(cur);
        // fixed subdivision driven by the control polygon's length
        const float len = std::hypot(c1.x - cur.x, c1.y - cur.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) + std::hypot(p.x - c2.x, p.y - c2.y);
        const int n = std::clamp(int(std::sqrt(len / tol) * 0.7f) + 2, 4, 80);
        const Pt p0 = cur;
        for (int i = 1; i <= n; ++i) {
            const float t = float(i) / float(n), u = 1 - t;
            out.back().pts.push_back({u * u * u * p0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p.x,
                                      u * u * u * p0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p.y});
        }
        cur = p;
    }
    void quad(Pt c, Pt p) {
        const Pt p0 = cur;
        cubic({p0.x + 2.f / 3.f * (c.x - p0.x), p0.y + 2.f / 3.f * (c.y - p0.y)}, {p.x + 2.f / 3.f * (c.x - p.x), p.y + 2.f / 3.f * (c.y - p.y)}, p);
    }
    void arc(float rx, float ry, float rot, bool large, bool sweep, Pt p) {
        const Pt p0 = cur;
        if (p0.x == p.x && p0.y == p.y) return;
        rx = std::fabs(rx); ry = std::fabs(ry);
        if (rx == 0 || ry == 0) { line(p); return; }
        const float phi = rot * kPi / 180.f, cp = std::cos(phi), sp = std::sin(phi);
        const float dx = (p0.x - p.x) / 2, dy = (p0.y - p.y) / 2;
        const float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
        const float lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
        const float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        float co = den == 0 ? 0 : std::sqrt(std::max(0.f, num / den));
        if (large == sweep) co = -co;
        const float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
        const float cx = cp * cxp - sp * cyp + (p0.x + p.x) / 2, cy = sp * cxp + cp * cyp + (p0.y + p.y) / 2;
        auto ang = [](float ux, float uy, float vx, float vy) {
            const float a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            return a;
        };
        const float th1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        float dth = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && dth > 0) dth -= 2 * kPi;
        if (sweep && dth < 0) dth += 2 * kPi;
        const int segs = std::max(1, int(std::ceil(std::fabs(dth) / (kPi / 2) - 1e-4f)));
        const float step = dth / float(segs);
        const float k = 4.f / 3.f * std::tan(step / 4);
        float t = th1;
        for (int i = 0; i < segs; ++i) {
            const float c1 = std::cos(t), s1 = std::sin(t), c2 = std::cos(t + step), s2 = std::sin(t + step);
            auto map = [&](float ux, float uy) { return Pt{cp * rx * ux - sp * ry * uy + cx, sp * rx * ux + cp * ry * uy + cy}; };
            const Pt q1 = map(c1 - k * s1, s1 + k * c1), q2 = map(c2 + k * s2, s2 - k * c2);
            Pt e = map(c2, s2);
            if (i == segs - 1) e = p;
            cubic(q1, q2, e);
            t += step;
        }
    }
};

Polys parse_path(const std::string& d, float tol) {
    PathBuilder pb;
    pb.tol = tol;
    Number nr{d};
    char cmd = 0;
    Pt last_c, last_q;
    char prev = 0;
    while (nr.more()) {
        const char c = d[nr.i];
        if (std::isalpha(static_cast<unsigned char>(c))) { cmd = c; ++nr.i; if (c == 'Z' || c == 'z') { pb.close(); prev = 'z'; continue; } }
        else if (cmd == 0) break;
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const char up = char(std::toupper(static_cast<unsigned char>(cmd)));
        float v[7];
        auto need = [&](int n) { for (int k = 0; k < n; ++k) if (!nr.next(v[k])) return false; return true; };
        const Pt cur = pb.cur;
        auto abs_pt = [&](float x, float y) { return Pt{rel ? cur.x + x : x, rel ? cur.y + y : y}; };
        switch (up) {
        case 'M': if (!need(2)) return pb.out; pb.move(abs_pt(v[0], v[1])); cmd = rel ? 'l' : 'L'; break;
        case 'L': if (!need(2)) return pb.out; pb.line(abs_pt(v[0], v[1])); break;
        case 'H': if (!need(1)) return pb.out; pb.line({rel ? cur.x + v[0] : v[0], cur.y}); break;
        case 'V': if (!need(1)) return pb.out; pb.line({cur.x, rel ? cur.y + v[0] : v[0]}); break;
        case 'C': { if (!need(6)) return pb.out; const Pt c1 = abs_pt(v[0], v[1]), c2 = abs_pt(v[2], v[3]), e = abs_pt(v[4], v[5]); pb.cubic(c1, c2, e); last_c = c2; break; }
        case 'S': {
            if (!need(4)) return pb.out;
            const Pt c1 = (prev == 'C' || prev == 'S') ? Pt{2 * cur.x - last_c.x, 2 * cur.y - last_c.y} : cur;
            const Pt c2 = abs_pt(v[0], v[1]), e = abs_pt(v[2], v[3]);
            pb.cubic(c1, c2, e); last_c = c2; break;
        }
        case 'Q': { if (!need(4)) return pb.out; const Pt c1 = abs_pt(v[0], v[1]), e = abs_pt(v[2], v[3]); pb.quad(c1, e); last_q = c1; break; }
        case 'T': {
            if (!need(2)) return pb.out;
            const Pt c1 = (prev == 'Q' || prev == 'T') ? Pt{2 * cur.x - last_q.x, 2 * cur.y - last_q.y} : cur;
            pb.quad(c1, abs_pt(v[0], v[1])); last_q = c1; break;
        }
        case 'A': {
            if (!need(3)) return pb.out;
            if (!nr.flag(v[3]) || !nr.flag(v[4]) || !need(0)) return pb.out;
            float ex, ey;
            if (!nr.next(ex) || !nr.next(ey)) return pb.out;
            pb.arc(v[0], v[1], v[2], v[3] != 0, v[4] != 0, abs_pt(ex, ey));
            break;
        }
        default: return pb.out;
        }
        prev = up;
    }
    return pb.out;
}

// ── styles and paints ─────────────────────────────────────────────────────────────────────────────

struct PaintRef {
    enum Kind { None, Color, Url, Current } kind = Color;
    uint32_t color = 0xFF000000;   // ARGB
    std::string url;               // id
    uint32_t fallback = 0;         // colour when the url is unknown (0 = none)
};

struct Style {
    PaintRef fill, stroke{PaintRef::None};
    float fill_opacity = 1, stroke_opacity = 1, stroke_width = 1, miter = 4, dash_off = 0, opacity = 1;
    int cap = 0, join = 0;          // butt/round/square, miter/round/bevel
    bool evenodd = false;
    std::vector<float> dash;
    uint32_t color = 0xFF000000;    // for currentColor
    bool hidden = false;
    uint32_t stop_color = 0xFF000000;
    float stop_opacity = 1;
};

struct Stop { float off; uint32_t color; };   // color ARGB incl. stop-opacity

struct Gradient {
    bool radial = false, obb = true;
    float x1 = 0, y1 = 0, x2 = 1, y2 = 0;           // linear (fractions in obb units, or user units)
    float cx = .5f, cy = .5f, r = .5f, fx = .5f, fy = .5f;
    bool pct_x1 = true, pct_x2 = true, pct_y1 = true, pct_y2 = true;
    Mat tf;
    int spread = 0;                                   // pad, reflect, repeat
    std::vector<Stop> stops;
};

uint32_t parse_col(const std::string& s, uint32_t current, bool& ok) {
    ok = true;
    const std::string v = trim(s);
    if (lower(v) == "currentcolor") return current;
    if (lower(v) == "transparent") return 0;
    uint32_t c;
    if (style::parse_color(lower(v), c)) return c;
    ok = false;
    return 0;
}

PaintRef parse_paint(const std::string& raw, const Style& st, const PaintRef& inherited) {
    const std::string v = trim(raw);
    PaintRef p;
    if (v == "none") { p.kind = PaintRef::None; return p; }
    if (v == "inherit") return inherited;
    if (lower(v) == "currentcolor") { p.kind = PaintRef::Current; return p; }
    if (v.starts_with("url(")) {
        const size_t close = v.find(')');
        std::string id = trim(v.substr(4, close == std::string::npos ? std::string::npos : close - 4));
        if (!id.empty() && (id.front() == '"' || id.front() == '\'')) id = id.substr(1, id.size() - 2);
        if (!id.empty() && id[0] == '#') id = id.substr(1);
        p.kind = PaintRef::Url; p.url = id;
        if (close != std::string::npos) {
            bool ok; const std::string rest = trim(v.substr(close + 1));
            if (!rest.empty()) { const uint32_t c = parse_col(rest, st.color, ok); if (ok) p.fallback = c; }
        }
        return p;
    }
    bool ok;
    const uint32_t c = parse_col(v, st.color, ok);
    if (!ok) return inherited;
    p.kind = PaintRef::Color; p.color = c;
    return p;
}

float parse_len(const std::string& s, float fallback = 0) {
    const std::string v = trim(s);
    if (v.empty()) return fallback;
    char* end = nullptr;
    const float n = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) return fallback;
    const std::string u = lower(end);
    if (u == "pt") return n * 96.f / 72.f;
    if (u == "pc") return n * 16.f;
    if (u == "mm") return n * 96.f / 25.4f;
    if (u == "cm") return n * 96.f / 2.54f;
    if (u == "in") return n * 96.f;
    if (u == "em") return n * 16.f;
    return n;
}

struct CssRule { std::string sel; std::vector<std::pair<std::string, std::string>> decls; };

std::vector<std::pair<std::string, std::string>> parse_decls(const std::string& body) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < body.size()) {
        size_t semi = i;
        int depth = 0;
        while (semi < body.size() && !(body[semi] == ';' && depth == 0)) { if (body[semi] == '(') ++depth; else if (body[semi] == ')') --depth; ++semi; }
        const std::string d = body.substr(i, semi - i);
        const size_t colon = d.find(':');
        if (colon != std::string::npos) out.emplace_back(lower(trim(d.substr(0, colon))), trim(d.substr(colon + 1)));
        i = semi + 1;
    }
    return out;
}

void parse_css(const std::string& css, std::vector<CssRule>& out) {
    size_t i = 0;
    while (i < css.size()) {
        const size_t ob = css.find('{', i);
        if (ob == std::string::npos) break;
        const size_t cb = css.find('}', ob);
        if (cb == std::string::npos) break;
        const std::string sels = css.substr(i, ob - i);
        const auto decls = parse_decls(css.substr(ob + 1, cb - ob - 1));
        size_t p = 0;
        while (p <= sels.size()) {
            const size_t c = sels.find(',', p);
            const std::string sel = trim(sels.substr(p, c == std::string::npos ? c : c - p));
            if (!sel.empty() && sel[0] != '@') out.push_back({sel, decls});
            if (c == std::string::npos) break;
            p = c + 1;
        }
        i = cb + 1;
    }
}

bool css_matches(const std::string& sel, const Node& n) {   // tag, .class, #id, tag.class, *
    std::string tag, id;
    std::vector<std::string> classes;
    size_t i = 0;
    while (i < sel.size()) {
        const char c = sel[i];
        if (c == '.' || c == '#') {
            size_t j = i + 1;
            while (j < sel.size() && sel[j] != '.' && sel[j] != '#') ++j;
            (c == '.' ? classes.push_back(sel.substr(i + 1, j - i - 1)) : void(id = sel.substr(i + 1, j - i - 1)));
            i = j;
        } else {
            size_t j = i;
            while (j < sel.size() && sel[j] != '.' && sel[j] != '#') ++j;
            tag = lower(sel.substr(i, j - i));
            i = j;
        }
    }
    if (!tag.empty() && tag != "*" && tag != n.tag) return false;
    if (!id.empty() && n.str("id") != id) return false;
    if (!classes.empty()) {
        const std::string cl = " " + n.str("class") + " ";
        for (const auto& c : classes) if (cl.find(" " + c + " ") == std::string::npos) return false;
    }
    return true;
}

void apply_decl(Style& s, const std::string& name, const std::string& value, const Style& parent) {
    const std::string v = trim(value);
    if (name == "fill") s.fill = parse_paint(v, s, parent.fill);
    else if (name == "stroke") s.stroke = parse_paint(v, s, parent.stroke);
    else if (name == "color") { bool ok; const uint32_t c = parse_col(v, parent.color, ok); if (ok) s.color = c; }
    else if (name == "fill-opacity") s.fill_opacity = std::clamp(parse_len(v, 1), 0.f, 1.f);
    else if (name == "stroke-opacity") s.stroke_opacity = std::clamp(parse_len(v, 1), 0.f, 1.f);
    else if (name == "opacity") s.opacity = std::clamp(parse_len(v, 1), 0.f, 1.f);
    else if (name == "stroke-width") s.stroke_width = std::max(0.f, parse_len(v, 1));
    else if (name == "stroke-linecap") s.cap = v == "round" ? 1 : v == "square" ? 2 : 0;
    else if (name == "stroke-linejoin") s.join = v == "round" ? 1 : v == "bevel" ? 2 : 0;
    else if (name == "stroke-miterlimit") s.miter = std::max(1.f, parse_len(v, 4));
    else if (name == "stroke-dashoffset") s.dash_off = parse_len(v, 0);
    else if (name == "stroke-dasharray") {
        s.dash.clear();
        if (v != "none") {
            Number nr{v};
            float x;
            while (nr.next(x)) s.dash.push_back(std::max(0.f, x));
            float sum = 0;
            for (float d : s.dash) sum += d;
            if (sum <= 0) s.dash.clear();
            else if (s.dash.size() % 2) { const auto copy = s.dash; s.dash.insert(s.dash.end(), copy.begin(), copy.end()); }
        }
    } else if (name == "fill-rule") s.evenodd = v == "evenodd";
    else if (name == "visibility") { s.hidden = v == "hidden" || v == "collapse"; }
    else if (name == "stop-color") { bool ok; const uint32_t c = parse_col(v, s.color, ok); if (ok) s.stop_color = c; }
    else if (name == "stop-opacity") s.stop_opacity = std::clamp(parse_len(v, 1), 0.f, 1.f);
}

// ── rasterisation ───────────────────────────────────────────────────────────────────────────────────

struct Edge { float x0, y0, x1, y1; int dir; };

struct Canvas {
    int w = 0, h = 0;
    std::vector<std::vector<float>> layers;      // premultiplied RGBA floats, top = current
    std::vector<float> mask, acc;
    std::vector<Edge> edges;
    std::vector<float> xs_buf;
    float* px() { return layers.back().data(); }
};

// Fills the winding-rule coverage of closed device-space polygons into cv.mask (only inside the returned box).
bool fill_mask(Canvas& cv, const std::vector<std::vector<Pt>>& polys, bool evenodd, int& bx0, int& by0, int& bx1, int& by1) {
    cv.edges.clear();
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (const auto& poly : polys) {
        const size_t n = poly.size();
        if (n < 3) continue;
        for (size_t i = 0; i < n; ++i) {
            Pt a = poly[i], b = poly[(i + 1) % n];
            minx = std::min({minx, a.x}); maxx = std::max({maxx, a.x}); miny = std::min(miny, a.y); maxy = std::max(maxy, a.y);
            if (a.y == b.y) continue;
            int dir = 1;
            if (a.y > b.y) { std::swap(a, b); dir = -1; }
            cv.edges.push_back({a.x, a.y, b.x, b.y, dir});
        }
    }
    if (cv.edges.empty()) return false;
    bx0 = std::max(0, int(std::floor(minx))); bx1 = std::min(cv.w, int(std::ceil(maxx)) + 1);
    by0 = std::max(0, int(std::floor(miny))); by1 = std::min(cv.h, int(std::ceil(maxy)) + 1);
    if (bx1 <= bx0 || by1 <= by0) return false;
    std::sort(cv.edges.begin(), cv.edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    constexpr int SS = 8;
    std::vector<int> active;
    size_t next = 0;
    std::vector<std::pair<float, int>> xs;
    cv.acc.assign(size_t(cv.w) + 2, 0.f);
    for (int y = by0; y < by1; ++y) {
        std::fill(cv.acc.begin() + bx0, cv.acc.begin() + bx1 + 1, 0.f);
        for (int s = 0; s < SS; ++s) {
            const float sy = float(y) + (float(s) + 0.5f) / float(SS);
            while (next < cv.edges.size() && cv.edges[next].y0 <= sy) active.push_back(int(next++));
            active.erase(std::remove_if(active.begin(), active.end(), [&](int i) { return cv.edges[size_t(i)].y1 <= sy; }), active.end());
            xs.clear();
            for (int i : active) {
                const Edge& e = cv.edges[size_t(i)];
                xs.emplace_back(e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir);
            }
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (size_t k = 0; k + 1 < xs.size(); ++k) {
                wind += xs[k].second;
                const bool inside = evenodd ? ((k + 1) % 2 == 1) : wind != 0;
                if (!inside) continue;
                float xa = std::clamp(xs[k].first, float(bx0), float(bx1)), xb = std::clamp(xs[k + 1].first, float(bx0), float(bx1));
                if (xb <= xa) continue;
                const int ia = int(xa), ib = int(xb);
                if (ia == ib) cv.acc[size_t(ia)] += (xb - xa) / SS;
                else {
                    cv.acc[size_t(ia)] += (float(ia + 1) - xa) / SS;
                    for (int x = ia + 1; x < ib; ++x) cv.acc[size_t(x)] += 1.f / SS;
                    cv.acc[size_t(ib)] += (xb - float(ib)) / SS;
                }
            }
        }
        float* row = cv.mask.data() + size_t(y) * size_t(cv.w);
        for (int x = bx0; x < bx1; ++x) row[x] = std::min(1.f, cv.acc[size_t(x)]);
    }
    return true;
}

// Stroke outline pieces (user space).  Every piece is a convex-ish polygon; the union is taken by the caller (non-zero).
struct Stroker {
    std::vector<std::vector<Pt>> pieces;
    float hw;
    int cap, join;
    float miter;
    float tol;    // user-space flatness
    void circle(Pt c) {
        const int n = std::clamp(int(kPi / std::acos(std::max(0.f, 1.f - tol / std::max(hw, 1e-3f)))), 8, 64);
        std::vector<Pt> p;
        for (int i = 0; i < n; ++i) { const float a = 2 * kPi * float(i) / float(n); p.push_back({c.x + hw * std::cos(a), c.y + hw * std::sin(a)}); }
        pieces.push_back(std::move(p));
    }
    void polyline(const std::vector<Pt>& in, bool closed) {
        std::vector<Pt> p;
        for (const Pt& q : in) if (p.empty() || std::hypot(q.x - p.back().x, q.y - p.back().y) > 1e-6f) p.push_back(q);
        if (closed && p.size() > 1 && std::hypot(p.front().x - p.back().x, p.front().y - p.back().y) <= 1e-6f) p.pop_back();
        if (p.size() == 1) {   // a dot: only round and square caps draw one
            if (cap == 1) circle(p[0]);
            else if (cap == 2) pieces.push_back({{p[0].x - hw, p[0].y - hw}, {p[0].x + hw, p[0].y - hw}, {p[0].x + hw, p[0].y + hw}, {p[0].x - hw, p[0].y + hw}});
            return;
        }
        if (p.size() < 2) return;
        const size_t n = p.size(), segs = closed ? n : n - 1;
        auto dirn = [&](size_t i) {
            const Pt a = p[i], b = p[(i + 1) % n];
            const float l = std::hypot(b.x - a.x, b.y - a.y);
            return Pt{(b.x - a.x) / l, (b.y - a.y) / l};
        };
        for (size_t i = 0; i < segs; ++i) {
            Pt a = p[i], b = p[(i + 1) % n];
            const Pt d = dirn(i), nm{-d.y * hw, d.x * hw};
            if (!closed && cap == 2) {
                if (i == 0) { a.x -= d.x * hw; a.y -= d.y * hw; }
                if (i == segs - 1) { b.x += d.x * hw; b.y += d.y * hw; }
            }
            pieces.push_back({{a.x + nm.x, a.y + nm.y}, {b.x + nm.x, b.y + nm.y}, {b.x - nm.x, b.y - nm.y}, {a.x - nm.x, a.y - nm.y}});
        }
        // joins
        for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i) {
            const Pt v = p[i], d0 = dirn((i + n - 1) % n), d1 = dirn(i);
            const float cr = d0.x * d1.y - d0.y * d1.x, dt = d0.x * d1.x + d0.y * d1.y;
            if (std::fabs(cr) < 1e-5f && dt > 0) continue;   // straight on
            if (join == 1) { circle(v); continue; }
            const float side = cr > 0 ? -1.f : 1.f;          // the outer side of the turn
            const Pt n0{-d0.y * hw * side, d0.x * hw * side}, n1{-d1.y * hw * side, d1.x * hw * side};
            const Pt a{v.x + n0.x, v.y + n0.y}, b{v.x + n1.x, v.y + n1.y};
            // miter length ratio = 1 / sin(theta/2), theta = angle between the segments
            const float cos_t = -dt, sin_half = std::sqrt(std::max(0.f, (1 - cos_t) / 2));
            if (join == 0 && sin_half > 1e-4f && 1.f / sin_half <= miter) {
                const float mx = n0.x + n1.x, my = n0.y + n1.y, ml = std::hypot(mx, my);
                if (ml > 1e-6f) {
                    const float k = hw / sin_half / ml * 1.f;   // tip distance hw / sin(half) along the bisector
                    pieces.push_back({v, a, {v.x + mx * k, v.y + my * k}, b});
                    continue;
                }
            }
            pieces.push_back({v, a, b});
        }
        if (!closed && cap == 1) { circle(p.front()); circle(p.back()); }
    }
};

Polys apply_dash(const Polys& in, const std::vector<float>& dash, float offset) {
    Polys out;
    float total = 0;
    for (float d : dash) total += d;
    for (const Poly& pl : in) {
        std::vector<Pt> pts = pl.pts;
        if (pl.closed && pts.size() > 1) pts.push_back(pts.front());
        // position inside the dash pattern
        float off = std::fmod(offset, total);
        if (off < 0) off += total;
        size_t di = 0;
        while (off >= dash[di]) { off -= dash[di]; di = (di + 1) % dash.size(); if (dash[di] == 0 && off == 0) break; }
        float left = dash[di] - off;
        bool on = di % 2 == 0;
        Poly cur;
        if (on && !pts.empty()) cur.pts.push_back(pts[0]);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            Pt a = pts[i];
            const Pt b = pts[i + 1];
            float seg = std::hypot(b.x - a.x, b.y - a.y);
            while (seg > left) {
                const float t = left / seg;
                const Pt m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (on) { cur.pts.push_back(m); out.push_back(cur); cur = {}; }
                else cur.pts = {m};
                on = !on;
                di = (di + 1) % dash.size();
                seg -= left; a = m; left = dash[di];
                if (left <= 0 && seg <= 0) break;
            }
            left -= seg;
            if (on) cur.pts.push_back(b);
        }
        if (on && cur.pts.size() > 1) out.push_back(cur);
    }
    return out;
}

struct Ctx {
    const Node* root = nullptr;
    std::unordered_map<std::string, const Node*> ids;
    std::vector<CssRule> css;
    Canvas cv;
    uint32_t current = 0xFF000000;
    int depth = 0;
};

void collect(Ctx& c, const Node& n) {
    const std::string* id = n.attr("id");
    if (id && !c.ids.count(*id)) c.ids[*id] = &n;
    if (n.tag == "style") parse_css(n.text, c.css);
    for (const auto& k : n.kids) collect(c, k);
}

Style style_of(Ctx& c, const Node& n, const Style& parent) {
    Style s = parent;
    s.opacity = 1;   // not inherited (`visibility` is: it lives in `hidden`; `display` is handled by render_node)
    static const char* kPresentation[] = {"fill", "stroke", "color", "fill-opacity", "stroke-opacity", "opacity", "stroke-width", "stroke-linecap",
        "stroke-linejoin", "stroke-miterlimit", "stroke-dashoffset", "stroke-dasharray", "fill-rule", "display", "visibility", "stop-color", "stop-opacity"};
    for (const char* k : kPresentation) if (const auto* v = n.attr(k)) apply_decl(s, k, *v, parent);
    for (const auto& r : c.css) if (css_matches(r.sel, n)) for (const auto& d : r.decls) apply_decl(s, d.first, d.second, parent);
    if (const auto* st = n.attr("style")) for (const auto& d : parse_decls(*st)) apply_decl(s, d.first, d.second, parent);
    return s;
}

Gradient build_gradient(Ctx& c, const Node& n, int depth = 0) {
    Gradient g;
    g.radial = n.tag == "radialgradient";
    if (const auto* href = n.attr("href") ? n.attr("href") : n.attr("xlink:href")) {
        if (!href->empty() && (*href)[0] == '#' && depth < 8) {
            auto it = c.ids.find(href->substr(1));
            if (it != c.ids.end() && (it->second->tag == "lineargradient" || it->second->tag == "radialgradient")) {
                const Gradient base = build_gradient(c, *it->second, depth + 1);
                const bool rad = g.radial;
                g = base; g.radial = rad;   // inherits stops and any attribute not given here
            }
        }
    }
    auto frac = [&](const char* k, float& v, bool& pct) {
        if (const auto* a = n.attr(k)) {
            pct = a->find('%') != std::string::npos;
            v = parse_len(*a) / (pct ? 100.f : 1.f);
        }
    };
    bool dummy;
    if (const auto* u = n.attr("gradientunits")) g.obb = *u != "userSpaceOnUse" && *u != "userspaceonuse";
    if (const auto* t = n.attr("gradienttransform")) g.tf = parse_transform(*t);
    if (const auto* sp = n.attr("spreadmethod")) g.spread = *sp == "reflect" ? 1 : *sp == "repeat" ? 2 : 0;
    if (!g.radial) {
        if (!n.attr("x2") && depth == 0 && g.stops.empty()) g.x2 = 1;
        frac("x1", g.x1, g.pct_x1); frac("y1", g.y1, g.pct_y1); frac("x2", g.x2, g.pct_x2); frac("y2", g.y2, g.pct_y2);
    } else {
        const bool had_f = n.attr("fx") || n.attr("fy");
        frac("cx", g.cx, dummy); frac("cy", g.cy, dummy); frac("r", g.r, dummy);
        if (!n.attr("fx")) g.fx = g.cx;
        if (!n.attr("fy")) g.fy = g.cy;
        frac("fx", g.fx, dummy); frac("fy", g.fy, dummy);
        (void)had_f;
    }
    std::vector<Stop> stops;
    Style base;
    for (const auto& k : n.kids) {
        if (k.tag != "stop") continue;
        Style s = style_of(c, k, base);
        float off = 0;
        if (const auto* o = k.attr("offset")) { off = parse_len(*o) / (o->find('%') != std::string::npos ? 100.f : 1.f); }
        off = std::clamp(off, 0.f, 1.f);
        if (!stops.empty()) off = std::max(off, stops.back().off);
        uint32_t col = s.stop_color;
        const float a = float(col >> 24) / 255.f * s.stop_opacity;
        col = (uint32_t(std::clamp(a * 255.f + 0.5f, 0.f, 255.f)) << 24) | (col & 0xFFFFFF);
        stops.push_back({off, col});
    }
    if (!stops.empty()) g.stops = stops;
    return g;
}

// Colour of a gradient at parameter t (premultiplied floats).
void grad_color(const Gradient& g, float t, float out[4]) {
    if (g.spread == 2) t = t - std::floor(t);
    else if (g.spread == 1) { t = std::fmod(std::fabs(t), 2.f); if (t > 1) t = 2 - t; }
    t = std::clamp(t, 0.f, 1.f);
    const auto& st = g.stops;
    size_t i = 0;
    while (i + 1 < st.size() && st[i + 1].off < t) ++i;
    uint32_t c0 = st[i].color, c1 = st[std::min(i + 1, st.size() - 1)].color;
    float k = 0;
    if (i + 1 < st.size() && st[i + 1].off > st[i].off) k = std::clamp((t - st[i].off) / (st[i + 1].off - st[i].off), 0.f, 1.f);
    if (t <= st.front().off) { c0 = c1 = st.front().color; k = 0; }
    auto ch = [&](uint32_t c, int sh) { return float((c >> sh) & 255) / 255.f; };
    const float a = ch(c0, 24) + (ch(c1, 24) - ch(c0, 24)) * k;
    out[3] = a;
    out[0] = (ch(c0, 16) + (ch(c1, 16) - ch(c0, 16)) * k) * a;
    out[1] = (ch(c0, 8) + (ch(c1, 8) - ch(c0, 8)) * k) * a;
    out[2] = (ch(c0, 0) + (ch(c1, 0) - ch(c0, 0)) * k) * a;
}

struct BBox { float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f; bool ok() const { return x1 >= x0; } };

// Composites the current mask through a paint.
void paint_mask(Ctx& c, int bx0, int by0, int bx1, int by1, const PaintRef& p, float opacity, const Mat& ctm, const BBox& bb, const Style& st) {
    Canvas& cv = c.cv;
    float* dst = cv.px();
    PaintRef pr = p;
    if (pr.kind == PaintRef::Current) { pr.kind = PaintRef::Color; pr.color = st.color; }
    const Gradient* grad = nullptr;
    Gradient gstore;
    Mat inv;
    if (pr.kind == PaintRef::Url) {
        auto it = c.ids.find(pr.url);
        if (it != c.ids.end() && (it->second->tag == "lineargradient" || it->second->tag == "radialgradient")) {
            gstore = build_gradient(c, *it->second);
            if (gstore.stops.empty()) return;
            if (gstore.stops.size() == 1) { pr.kind = PaintRef::Color; pr.color = gstore.stops[0].color; }
            else {
                Mat m = ctm;
                if (gstore.obb) {
                    if (!bb.ok() || bb.x1 <= bb.x0 || bb.y1 <= bb.y0) return;
                    Mat o; o.a = bb.x1 - bb.x0; o.d = bb.y1 - bb.y0; o.e = bb.x0; o.f = bb.y0;
                    m = m * o;
                }
                m = m * gstore.tf;
                inv = m.inverse();
                grad = &gstore;
            }
        } else if (pr.fallback) { pr.kind = PaintRef::Color; pr.color = pr.fallback; }
        else return;
    }
    float solid[4] = {0, 0, 0, 0};
    if (!grad) {
        const float a = float(pr.color >> 24) / 255.f;
        solid[3] = a; solid[0] = float((pr.color >> 16) & 255) / 255.f * a; solid[1] = float((pr.color >> 8) & 255) / 255.f * a; solid[2] = float(pr.color & 255) / 255.f * a;
    }
    for (int y = by0; y < by1; ++y) {
        const float* mrow = cv.mask.data() + size_t(y) * size_t(cv.w);
        float* drow = dst + size_t(y) * size_t(cv.w) * 4;
        for (int x = bx0; x < bx1; ++x) {
            const float cov = mrow[x] * opacity;
            if (cov <= 0.001f) continue;
            float s[4] = {solid[0], solid[1], solid[2], solid[3]};
            if (grad) {
                const Pt q = inv.apply({float(x) + 0.5f, float(y) + 0.5f});
                float t;
                if (!grad->radial) {
                    const float dx = grad->x2 - grad->x1, dy = grad->y2 - grad->y1, l2 = dx * dx + dy * dy;
                    t = l2 > 0 ? ((q.x - grad->x1) * dx + (q.y - grad->y1) * dy) / l2 : 0;
                } else {
                    // focal gradient: t = scale such that q = f + t (edge point - f)
                    const float fx = grad->fx, fy = grad->fy, r = std::max(grad->r, 1e-6f);
                    const float dx = q.x - fx, dy = q.y - fy, cx = grad->cx - fx, cy = grad->cy - fy;
                    const float a = dx * dx + dy * dy;
                    if (a < 1e-12f) t = 0;
                    else {
                        // solve |s d - c| = r for s: (a) s² - 2 (d.c) s + (c.c - r²) = 0
                        const float b = dx * cx + dy * cy, cc = cx * cx + cy * cy - r * r;
                        const float disc = b * b - a * cc;
                        const float sroot = disc > 0 ? (b + std::sqrt(disc)) / a : 1e9f;
                        t = sroot > 1e-6f ? 1.f / sroot : 1.f;
                    }
                }
                grad_color(*grad, t, s);
            }
            const float k = cov, sa = s[3] * k;
            float* d = drow + size_t(x) * 4;
            const float ia = 1.f - sa;
            d[0] = s[0] * k + d[0] * ia; d[1] = s[1] * k + d[1] * ia; d[2] = s[2] * k + d[2] * ia; d[3] = sa + d[3] * ia;
        }
    }
}

void orient_ccw(std::vector<Pt>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) { const Pt& u = p[i]; const Pt& v = p[(i + 1) % p.size()]; a += double(u.x) * v.y - double(v.x) * u.y; }
    if (a < 0) std::reverse(p.begin(), p.end());
}

void draw_shape(Ctx& c, const Polys& user, const Style& st, const Mat& ctm) {
    Canvas& cv = c.cv;
    const float sc = std::max(ctm.scale(), 1e-4f);
    BBox bb;
    for (const auto& pl : user) for (const Pt& p : pl.pts) { bb.x0 = std::min(bb.x0, p.x); bb.x1 = std::max(bb.x1, p.x); bb.y0 = std::min(bb.y0, p.y); bb.y1 = std::max(bb.y1, p.y); }
    int bx0, by0, bx1, by1;
    // fill
    if (st.fill.kind != PaintRef::None && st.fill_opacity > 0) {
        std::vector<std::vector<Pt>> dev;
        for (const auto& pl : user) {
            if (pl.pts.size() < 3) continue;
            std::vector<Pt> q;
            for (const Pt& p : pl.pts) q.push_back(ctm.apply(p));
            dev.push_back(std::move(q));
        }
        if (fill_mask(cv, dev, st.evenodd, bx0, by0, bx1, by1)) paint_mask(c, bx0, by0, bx1, by1, st.fill, st.fill_opacity, ctm, bb, st);
    }
    // stroke
    if (st.stroke.kind != PaintRef::None && st.stroke_opacity > 0 && st.stroke_width > 0) {
        Polys src = user;
        if (!st.dash.empty()) src = apply_dash(user, st.dash, st.dash_off);
        Stroker sk;
        sk.hw = st.stroke_width / 2; sk.cap = st.cap; sk.join = st.join; sk.miter = st.miter; sk.tol = 0.1f / sc;
        for (const auto& pl : src) sk.polyline(pl.pts, pl.closed && st.dash.empty());
        std::vector<std::vector<Pt>> dev;
        for (auto& piece : sk.pieces) {
            std::vector<Pt> q;
            for (const Pt& p : piece) q.push_back(ctm.apply(p));
            orient_ccw(q);
            dev.push_back(std::move(q));
        }
        if (fill_mask(cv, dev, false, bx0, by0, bx1, by1)) paint_mask(c, bx0, by0, bx1, by1, st.stroke, st.stroke_opacity, ctm, bb, st);
    }
}

Polys shape_path(const Node& n, float tol) {
    PathBuilder pb;
    pb.tol = tol;
    auto num = [&](const char* k, float d = 0) { return parse_len(n.str(k), d); };
    if (n.tag == "path") return parse_path(n.str("d"), tol);
    if (n.tag == "rect") {
        const float x = num("x"), y = num("y"), w = num("width"), h = num("height");
        if (w <= 0 || h <= 0) return {};
        float rx = n.attr("rx") ? num("rx") : -1, ry = n.attr("ry") ? num("ry") : -1;
        if (rx < 0 && ry < 0) rx = ry = 0;
        else if (rx < 0) rx = ry; else if (ry < 0) ry = rx;
        rx = std::min(rx, w / 2); ry = std::min(ry, h / 2);
        if (rx <= 0 || ry <= 0) { pb.move({x, y}); pb.line({x + w, y}); pb.line({x + w, y + h}); pb.line({x, y + h}); pb.close(); return pb.out; }
        pb.move({x + rx, y});
        pb.line({x + w - rx, y}); pb.arc(rx, ry, 0, false, true, {x + w, y + ry});
        pb.line({x + w, y + h - ry}); pb.arc(rx, ry, 0, false, true, {x + w - rx, y + h});
        pb.line({x + rx, y + h}); pb.arc(rx, ry, 0, false, true, {x, y + h - ry});
        pb.line({x, y + ry}); pb.arc(rx, ry, 0, false, true, {x + rx, y});
        pb.close();
        return pb.out;
    }
    if (n.tag == "circle" || n.tag == "ellipse") {
        const float cx = num("cx"), cy = num("cy");
        const float rx = n.tag == "circle" ? num("r") : num("rx"), ry = n.tag == "circle" ? rx : num("ry");
        if (rx <= 0 || ry <= 0) return {};
        pb.move({cx + rx, cy});
        pb.arc(rx, ry, 0, false, true, {cx, cy + ry}); pb.arc(rx, ry, 0, false, true, {cx - rx, cy});
        pb.arc(rx, ry, 0, false, true, {cx, cy - ry}); pb.arc(rx, ry, 0, false, true, {cx + rx, cy});
        pb.close();
        return pb.out;
    }
    if (n.tag == "line") { pb.move({num("x1"), num("y1")}); pb.line({num("x2"), num("y2")}); return pb.out; }
    if (n.tag == "polyline" || n.tag == "polygon") {
        const std::string pts = n.str("points");
        Number nr{pts};
        float x, y;
        bool first = true;
        while (nr.next(x) && nr.next(y)) { if (first) pb.move({x, y}); else pb.line({x, y}); first = false; }
        if (n.tag == "polygon") pb.close();
        return pb.out;
    }
    return {};
}

void render_node(Ctx& c, const Node& n, const Style& parent, const Mat& ctm);

void render_kids(Ctx& c, const Node& n, const Style& st, const Mat& ctm) {
    for (const auto& k : n.kids) render_node(c, k, st, ctm);
}

void render_node(Ctx& c, const Node& n, const Style& parent, const Mat& ctm_in) {
    static const char* kSkip[] = {"defs", "style", "title", "desc", "metadata", "lineargradient", "radialgradient", "clippath", "mask", "pattern", "marker",
                                  "symbol", "filter", "script", "text", "image", "foreignobject", "switch"};
    for (const char* s : kSkip) if (n.tag == s) return;
    if (c.depth > 24) return;
    Style st = style_of(c, n, parent);
    if (n.attr("display") && trim(n.str("display")) == "none") return;
    for (const auto& r : c.css) if (css_matches(r.sel, n)) for (const auto& d : r.decls) if (d.first == "display" && d.second == "none") return;
    if (const auto* s = n.attr("style")) for (const auto& d : parse_decls(*s)) if (d.first == "display" && d.second == "none") return;
    Mat ctm = ctm_in;
    if (const auto* t = n.attr("transform")) ctm = ctm * parse_transform(*t);
    const bool layer = st.opacity < 1.f;
    if (layer) c.cv.layers.emplace_back(size_t(c.cv.w) * size_t(c.cv.h) * 4, 0.f);
    ++c.depth;
    if (n.tag == "g" || n.tag == "svg" || n.tag == "a") {
        Mat m = ctm;
        if (n.tag == "svg") { m.e += 0; Mat t; t.e = parse_len(n.str("x")); t.f = parse_len(n.str("y")); m = ctm * t; }
        render_kids(c, n, st, m);
    } else if (n.tag == "use") {
        const std::string* href = n.attr("href") ? n.attr("href") : n.attr("xlink:href");
        if (href && href->size() > 1 && (*href)[0] == '#') {
            auto it = c.ids.find(href->substr(1));
            if (it != c.ids.end()) {
                Mat t; t.e = parse_len(n.str("x")); t.f = parse_len(n.str("y"));
                const Node& ref = *it->second;
                if (ref.tag == "symbol" || ref.tag == "svg") {
                    Mat vb = ctm * t;
                    render_kids(c, ref, st, vb);
                } else render_node(c, ref, st, ctm * t);
            }
        }
    } else {
        const float sc = std::max(ctm.scale(), 1e-4f);
        const Polys path = shape_path(n, 0.15f / sc);
        if (!path.empty() && !st.hidden) draw_shape(c, path, st, ctm);
    }
    --c.depth;
    if (layer) {
        std::vector<float> top = std::move(c.cv.layers.back());
        c.cv.layers.pop_back();
        float* d = c.cv.px();
        const size_t n_px = size_t(c.cv.w) * size_t(c.cv.h);
        for (size_t i = 0; i < n_px; ++i) {
            const float* s = &top[i * 4];
            if (s[3] <= 0) continue;
            const float a = s[3] * st.opacity, ia = 1.f - a;
            float* q = d + i * 4;
            q[0] = s[0] * st.opacity + q[0] * ia; q[1] = s[1] * st.opacity + q[1] * ia; q[2] = s[2] * st.opacity + q[2] * ia; q[3] = a + q[3] * ia;
        }
    }
}

} // namespace

// ── public API ──────────────────────────────────────────────────────────────────────────────────────

struct Doc {
    Node root;
    uint64_t hash = 0;
    uint32_t current = 0xFF000000;
};

namespace {

std::shared_ptr<const Doc> finish(Node&& root, uint32_t current) {
    if (root.tag != "svg") return nullptr;
    auto d = std::make_shared<Doc>();
    d->root = std::move(root);
    d->current = current;
    d->hash = 1469598103934665603ull ^ current;
    hash_node(d->hash, d->root);
    return d;
}

bool viewbox(const Node& n, float v[4]) {
    const auto* a = n.attr("viewbox");
    if (!a) return false;
    Number nr{*a};
    for (int i = 0; i < 4; ++i) if (!nr.next(v[i])) return false;
    return v[2] > 0 && v[3] > 0;
}

} // namespace

std::shared_ptr<const Doc> parse(const std::string& xml, uint32_t current) {
    size_t i = 0;
    while (true) {   // skip the prolog: BOM, <?xml?>, comments, doctype
        while (i < xml.size() && (std::isspace(static_cast<unsigned char>(xml[i])) || (unsigned char)xml[i] >= 0x80)) ++i;
        if (xml.compare(i, 4, "<!--") == 0) { const size_t e = xml.find("-->", i); if (e == std::string::npos) return nullptr; i = e + 3; }
        else if (xml.compare(i, 2, "<?") == 0 || xml.compare(i, 2, "<!") == 0) { const size_t e = xml.find('>', i); if (e == std::string::npos) return nullptr; i = e + 1; }
        else break;
    }
    if (i >= xml.size() || xml[i] != '<') return nullptr;
    XmlParser p{xml};
    p.i = i;
    Node root;
    if (!p.parse_element(root)) return nullptr;
    return finish(std::move(root), current);
}

std::shared_ptr<const Doc> from_dom(const dom::Node& svg, uint32_t current) {
    Node root;
    from_dom_rec(svg, root);
    return finish(std::move(root), current);
}

void intrinsic_size(const Doc& d, float& w, float& h) {
    float vb[4];
    const bool has_vb = viewbox(d.root, vb);
    auto dim = [&](const char* k, float fallback_vb, float def) {
        const auto* a = d.root.attr(k);
        if (a && a->find('%') == std::string::npos) { const float v = parse_len(*a); if (v > 0) return v; }
        return has_vb ? fallback_vb : def;
    };
    w = dim("width", vb[2], 300);
    h = dim("height", vb[3], 150);
    // one dimension given and a viewBox: keep its ratio
    if (has_vb) {
        const auto *aw = d.root.attr("width"), *ah = d.root.attr("height");
        const bool gw = aw && aw->find('%') == std::string::npos, gh = ah && ah->find('%') == std::string::npos;
        if (gw && !gh) h = w * vb[3] / vb[2];
        if (gh && !gw) w = h * vb[2] / vb[3];
    }
}

namespace {
std::mutex g_cache_m;
std::map<std::tuple<uint64_t, int, int>, std::shared_ptr<const ImageBitmap>> g_cache;
std::vector<std::tuple<uint64_t, int, int>> g_order;
} // namespace

std::shared_ptr<const ImageBitmap> raster(const std::shared_ptr<const Doc>& dp, int W, int H) {
    if (!dp || W <= 0 || H <= 0 || W > 8192 || H > 8192 || size_t(W) * size_t(H) > (32u << 20) / 4) return nullptr;
    const auto key = std::make_tuple(dp->hash, W, H);
    {
        std::lock_guard<std::mutex> lk(g_cache_m);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it->second;
    }
    const Doc& d = *dp;
    Ctx c;
    c.root = &d.root;
    c.current = d.current;
    collect(c, d.root);
    c.cv.w = W; c.cv.h = H;
    c.cv.layers.emplace_back(size_t(W) * size_t(H) * 4, 0.f);
    c.cv.mask.assign(size_t(W) * size_t(H), 0.f);

    // viewport -> user space
    float iw, ih;
    intrinsic_size(d, iw, ih);
    float vb[4];
    const bool has_vb = viewbox(d.root, vb);
    Mat base;
    if (!has_vb) { base.a = float(W) / iw; base.d = float(H) / ih; }
    else {
        const std::string par = lower(trim(d.root.str("preserveaspectratio", "xMidYMid meet")));
        const bool none = par.find("none") != std::string::npos, slice = par.find("slice") != std::string::npos;
        float sx = float(W) / vb[2], sy = float(H) / vb[3];
        float ax = .5f, ay = .5f;
        if (par.find("xmin") != std::string::npos) ax = 0; else if (par.find("xmax") != std::string::npos) ax = 1;
        if (par.find("ymin") != std::string::npos) ay = 0; else if (par.find("ymax") != std::string::npos) ay = 1;
        if (!none) { sx = sy = slice ? std::max(sx, sy) : std::min(sx, sy); }
        base.a = sx; base.d = sy;
        base.e = (float(W) - vb[2] * sx) * ax - vb[0] * sx;
        base.f = (float(H) - vb[3] * sy) * ay - vb[1] * sy;
    }
    Style root_style;
    root_style.color = c.current;
    root_style.fill.color = 0xFF000000;
    // the root's own presentation attributes (fill, stroke, color...) apply to everything below
    Style rs = style_of(c, d.root, root_style);
    rs.opacity = 1;
    for (const auto& k : d.root.kids) render_node(c, k, rs, base);

    auto bm = std::make_shared<ImageBitmap>();
    bm->w = W; bm->h = H;
    bm->rgba.resize(size_t(W) * size_t(H) * 4);
    bm->rgb.resize(size_t(W) * size_t(H) * 3);
    const float* src = c.cv.layers[0].data();
    for (size_t i = 0; i < size_t(W) * size_t(H); ++i) {
        const float a = src[i * 4 + 3];
        for (int ch = 0; ch < 3; ++ch) {
            const float v = a > 0.0001f ? src[i * 4 + ch] / a : 0.f;
            bm->rgba[i * 4 + size_t(ch)] = uint8_t(std::clamp(v * 255.f + 0.5f, 0.f, 255.f));
            bm->rgb[i * 3 + size_t(ch)] = uint8_t(std::clamp((src[i * 4 + ch] + (1.f - a)) * 255.f + 0.5f, 0.f, 255.f));
        }
        bm->rgba[i * 4 + 3] = uint8_t(std::clamp(a * 255.f + 0.5f, 0.f, 255.f));
    }
    std::shared_ptr<const ImageBitmap> out = bm;
    std::lock_guard<std::mutex> lk(g_cache_m);
    g_cache[key] = out;
    g_order.push_back(key);
    if (g_order.size() > 96) { g_cache.erase(g_order.front()); g_order.erase(g_order.begin()); }
    return out;
}

} // namespace luxium::render::svg
