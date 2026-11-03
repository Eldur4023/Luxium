// Inline formatting context: tokenizes the inline content of a block
// (white-space processing, break opportunities), breaks it into lines around
// floats, and builds line boxes with Blink-style vertical metrics
// (struts, half-leading, vertical-align), alignment and inline-box fragments.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace luxium::layout {

using style::Float;
using style::TextAlign;
using style::VerticalAlign;
using style::WhiteSpace;

render::FaceKey face_of(Env& env, const ComputedStyle& s) {
    auto it = env.face_cache.find(&s);
    if (it != env.face_cache.end()) return it->second;
    const render::FaceKey k = env.font.resolve(s.family_name, s.font_weight, s.italic);
    env.face_cache.emplace(&s, k);
    return k;
}

float line_height_px(Env& env, const ComputedStyle& s) {
    if (s.lh_mode == 1) return s.font_size * s.lh_val;
    if (s.lh_mode == 2) return s.lh_val;
    const render::FaceMetrics m = env.font.metrics(face_of(env, s), s.font_size);
    return m.ascent + m.descent + m.line_gap;   // Blink: round(ascent) + round(descent) + round(lineGap)
}

namespace {

// ── text helpers ────────────────────────────────────────────────────────────

uint32_t decode(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    ++i;
    while (n-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) cp = (cp << 6) | (s[i++] & 0x3F);
    return cp;
}
void encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

uint32_t upper(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F && (c & 1) && !(c >= 0x138 && c <= 0x148)) return c - 1;   // Latin Ext-A pairs (approx)
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;                              // Greek
    if (c >= 0x430 && c <= 0x44F) return c - 32;                                            // Cyrillic
    return c;
}
uint32_t lower(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x100 && c <= 0x17F && !(c & 1) && !(c >= 0x138 && c <= 0x148)) return c + 1;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    return c;
}
bool is_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0;
}

std::string transform_text(const std::string& in, style::TextTransform t) {
    if (t == style::TextTransform::None) return in;
    std::string out;
    out.reserve(in.size());
    bool start = true;
    for (size_t i = 0; i < in.size();) {
        uint32_t c = decode(in, i);
        if (t == style::TextTransform::Uppercase) { if (c == 0xDF) { out += "SS"; continue; } c = upper(c); }
        else if (t == style::TextTransform::Lowercase) c = lower(c);
        else { if (start && is_letter(c)) c = upper(c); }
        start = !is_letter(c) && c != '\'' && !(c >= '0' && c <= '9');
        encode(c, out);
    }
    return out;
}

bool is_cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FFFF);
}

std::shared_ptr<const render::ShapedRun> shape_cached(Env& env, const std::string& text, render::FaceKey f, float px) {
    std::string key;
    key.reserve(text.size() + 16);
    key += std::to_string(f.id) + 'f';
    key += std::to_string(static_cast<int>(px * 64)); key += '|'; key += text;
    auto it = env.shape_cache.find(key);
    if (it != env.shape_cache.end()) return it->second;
    auto run = std::make_shared<render::ShapedRun>(env.font.shape_at(text, f, px));
    env.shape_cache.emplace(std::move(key), run);
    return run;
}

// ── tokens ──────────────────────────────────────────────────────────────────

enum class K { Word, Space, Break, Atom, Open, Close, Float, Abs, Marker };

struct Tok {
    K k = K::Word;
    Box* box = nullptr;                 // source: text box / inline box / atom / float / abs
    const ComputedStyle* st = nullptr;  // style of the text / inline box
    Box* ibox = nullptr;                // innermost enclosing inline box (null: the block itself)
    std::string text;
    std::shared_ptr<const render::ShapedRun> run;
    float w = 0;
    bool breakable = false;             // Space: a line may break here
    bool break_after = false;           // Word/Atom: opportunity right after
    bool break_before = false;          // Atom
    bool hang = false;                  // preserved space (pre-wrap): may hang past the line end
    int deco = 0;
    uint32_t deco_color = 0;
    const ComputedStyle* deco_style = nullptr;
};

struct Deco { int flags = 0; uint32_t color = 0; const ComputedStyle* st = nullptr; };

struct Ifc {
    Env& env;
    Box& blk;
    const float content_w;
    std::vector<Tok> toks;
    std::vector<Box*> open;             // inline boxes open during tokenization
    bool prev_space = true;             // last emitted char was a collapsible space (or line start)
    std::vector<Deco> deco_stack;       // propagated text-decoration (underline, line-through, overline)
    bool measure = false;

    Ifc(Env& e, Box& b, float w) : env(e), blk(b), content_w(w) {}

    void push_deco(const ComputedStyle& s) {
        Deco d = deco_stack.empty() ? Deco{} : deco_stack.back();
        const uint32_t col = s.decoration_color ? s.decoration_color : s.color;
        if (s.underline) { d.flags |= 1; }
        if (s.line_through) { d.flags |= 2; }
        if (s.overline) { d.flags |= 4; }
        if (s.underline || s.line_through || s.overline) { d.color = col; d.st = &s; }
        deco_stack.push_back(d);
    }
    void stamp(Tok& t) const {
        if (deco_stack.empty()) return;
        t.deco = deco_stack.back().flags; t.deco_color = deco_stack.back().color; t.deco_style = deco_stack.back().st;
    }

    float letter_spacing_extra(const ComputedStyle& s, size_t glyphs) const { return s.letter_spacing * static_cast<float>(glyphs); }

    Tok make_word(Box* tb, const ComputedStyle& s, const std::string& txt) {
        Seg seg{shape_cached(env, txt, face_of(env, s), s.font_size), txt};
        return slice_tok(tb, s, seg, 0, txt.size(), K::Word);
    }

    // The whole text node is shaped once (kerning across spaces, ligatures),
    // then cut into tokens along cluster boundaries.
    struct Seg { std::shared_ptr<const render::ShapedRun> run; std::string str; };

    Tok slice_tok(Box* tb, const ComputedStyle& s, const Seg& seg, size_t b0, size_t b1, K kind) {
        Tok t;
        t.k = kind; t.box = tb; t.st = &s; t.ibox = open.empty() ? nullptr : open.back();
        t.text = seg.str.substr(b0, b1 - b0);
        auto sub = std::make_shared<render::ShapedRun>();
        float first_pen = -1, w = 0;
        size_t nglyph = 0;
        for (const auto& p : seg.run->positions) {
            if (p.cluster < b0 || p.cluster >= b1) continue;
            if (first_pen < 0) first_pen = p.pen;
            auto q = p;
            q.x = p.x - first_pen;
            q.pen = p.pen - first_pen;
            sub->positions.push_back(q);
            w += p.adv;
            ++nglyph;
        }
        float extra = s.letter_spacing * static_cast<float>(nglyph);
        if (s.word_spacing != 0 && kind != K::Space) for (char c : t.text) if (c == ' ') extra += s.word_spacing;
        if (s.letter_spacing != 0)
            for (size_t i = 0; i < sub->positions.size(); ++i) sub->positions[i].x += s.letter_spacing * static_cast<float>(i);
        sub->width = w + extra;
        t.run = sub;
        t.w = w + extra;
        if (kind == K::Space) t.w += s.word_spacing;
        stamp(t);
        return t;
    }

    Tok make_space(Box* tb, const ComputedStyle& s, bool breakable) {
        Seg seg{shape_cached(env, " ", face_of(env, s), s.font_size), " "};
        Tok t = slice_tok(tb, s, seg, 0, 1, K::Space);
        t.breakable = breakable;
        return t;
    }

    // splits one word span [b0,b1) of `seg` at break opportunities into tokens
    void emit_word_span(Box* tb, const ComputedStyle& s, const Seg& seg, size_t b0, size_t b1, bool wrap_ok) {
        if (b1 <= b0) return;
        const bool brkall = s.word_break == style::WordBreak::BreakAll;
        const bool overflow_split = s.overflow_wrap != style::OverflowWrap::Normal ||
                                    s.word_break == style::WordBreak::BreakWord;
        const std::string& w = seg.str;
        std::vector<std::pair<size_t, size_t>> parts;
        std::vector<bool> brk;
        size_t start = b0;
        for (size_t i = b0; i < b1;) {
            const size_t at = i;
            const uint32_t cp = decode(w, i);
            bool split = false;
            if (wrap_ok && i < b1) {
                if (cp == '-' && at > b0 && std::isalnum(static_cast<unsigned char>(w[at - 1])) &&
                    std::isalnum(static_cast<unsigned char>(w[i]))) split = true;
                else if (brkall) split = true;
                else if (is_cjk(cp)) split = true;
                else { size_t j = i; const uint32_t nx = decode(w, j); if (is_cjk(nx)) split = true; }
            }
            if (split) { parts.push_back({start, i}); brk.push_back(true); start = i; }
        }
        if (start < b1) { parts.push_back({start, b1}); brk.push_back(false); }
        for (size_t p = 0; p < parts.size(); ++p) {
            Tok t = slice_tok(tb, s, seg, parts[p].first, parts[p].second, K::Word);
            t.break_after = brk[p];
            if (overflow_split && !measure && content_w > 0 && t.w > content_w) {
                // a word wider than the line breaks wherever it must
                size_t from = parts[p].first;
                const size_t to = parts[p].second;
                while (from < to) {
                    std::vector<size_t> cuts;
                    for (size_t i = from; i < to;) { decode(w, i); cuts.push_back(i); }
                    Tok piece = slice_tok(tb, s, seg, from, to, K::Word);
                    if (piece.w > content_w) {
                        size_t keep = 1;
                        for (size_t c = 1; c < cuts.size(); ++c) {
                            Tok pre = slice_tok(tb, s, seg, from, cuts[c - 1], K::Word);
                            if (pre.w <= content_w) keep = c; else break;
                        }
                        piece = slice_tok(tb, s, seg, from, cuts[keep - 1], K::Word);
                        piece.break_after = true;
                        from = cuts[keep - 1];
                    } else { piece.break_after = brk[p]; from = to; }
                    toks.push_back(std::move(piece));
                }
                continue;
            }
            toks.push_back(std::move(t));
        }
    }

    void text(Box& tb) {
        const ComputedStyle& s = *tb.st;
        const std::string txt = transform_text(tb.text, s.text_transform);
        const WhiteSpace ws = s.white_space;
        const bool collapse = ws == WhiteSpace::Normal || ws == WhiteSpace::Nowrap || ws == WhiteSpace::PreLine;
        const bool keep_nl = ws == WhiteSpace::Pre || ws == WhiteSpace::PreWrap || ws == WhiteSpace::PreLine || ws == WhiteSpace::BreakSpaces;
        const bool wrap_ok = ws != WhiteSpace::Nowrap && ws != WhiteSpace::Pre;

        // pass 1: the processed string and the token spans over it
        struct Spec { K kind; size_t b0, b1; bool hang = false; };
        std::string proc;
        std::vector<Spec> specs;
        bool in_word = false;
        auto end_word = [&] { in_word = false; };
        for (size_t i = 0; i < txt.size();) {
            const size_t at = i;
            const uint32_t cp = decode(txt, i);
            if (cp == '\n' || cp == '\r' || cp == 0x0C) {
                if (cp == '\r' && i < txt.size() && txt[i] == '\n') ++i;
                end_word();
                if (keep_nl) {
                    if (collapse && !specs.empty() && specs.back().kind == K::Space) { proc.pop_back(); specs.pop_back(); }
                    specs.push_back({K::Break, proc.size(), proc.size()});
                    prev_space = true;
                } else if (!prev_space) {
                    specs.push_back({K::Space, proc.size(), proc.size() + 1}); proc += ' '; prev_space = true;
                }
                continue;
            }
            if (cp == ' ' || cp == '\t') {
                if (collapse) {
                    end_word();
                    if (!prev_space) { specs.push_back({K::Space, proc.size(), proc.size() + 1}); proc += ' '; prev_space = true; }
                } else if (ws == WhiteSpace::Pre) {
                    const size_t b0 = proc.size();
                    proc += cp == '\t' ? std::string("        ") : std::string(" ");
                    if (in_word && !specs.empty()) specs.back().b1 = proc.size(); else { specs.push_back({K::Word, b0, proc.size()}); in_word = true; }
                    prev_space = false;
                } else {   // pre-wrap / break-spaces: preserved spaces, hanging, break opportunity after each
                    end_word();
                    const size_t b0 = proc.size();
                    proc += cp == '\t' ? std::string("        ") : std::string(" ");
                    specs.push_back({K::Word, b0, proc.size(), true});
                    prev_space = false;
                }
                continue;
            }
            const size_t b0 = proc.size();
            proc.append(txt, at, i - at);
            if (in_word && !specs.empty() && specs.back().kind == K::Word && !specs.back().hang) specs.back().b1 = proc.size();
            else { specs.push_back({K::Word, b0, proc.size()}); in_word = true; }
            prev_space = false;
        }
        if (specs.empty()) return;

        // pass 2: shape once, then emit tokens
        Seg seg{shape_cached(env, proc, face_of(env, s), s.font_size), proc};
        for (const Spec& sp : specs) {
            switch (sp.kind) {
            case K::Space: {
                Tok t = slice_tok(&tb, s, seg, sp.b0, sp.b1, K::Space);
                t.breakable = wrap_ok;
                toks.push_back(std::move(t));
                break;
            }
            case K::Break: {
                Tok t; t.k = K::Break; t.box = &tb; t.st = &s; t.ibox = open.empty() ? nullptr : open.back();
                toks.push_back(t);
                break;
            }
            default:
                if (sp.hang) {
                    Tok t = slice_tok(&tb, s, seg, sp.b0, sp.b1, K::Word);
                    t.break_after = true; t.hang = true;
                    toks.push_back(std::move(t));
                } else emit_word_span(&tb, s, seg, sp.b0, sp.b1, wrap_ok);
            }
        }
    }

    // marks `t` as inside the current inline box chain
    void walk(Box& parent) {
        for (auto& up : parent.kids) {
            Box& k = *up;
            switch (k.kind) {
            case Box::Kind::Text: text(k); break;
            case Box::Kind::Break: {
                Tok t; t.k = K::Break; t.box = &k; t.st = k.st; t.ibox = open.empty() ? nullptr : open.back();
                toks.push_back(t);
                prev_space = true;
                break;
            }
            case Box::Kind::Marker: {
                if (parent.st->list_inside || &parent != &blk) {}   // markers inside: handled below
                break;
            }
            case Box::Kind::Inline: {
                if (k.is_abs()) { abs_or_float(k); break; }
                resolve_edges(k, content_w);
                push_deco(*k.st);
                Tok o; o.k = K::Open; o.box = &k; o.st = k.st; o.ibox = open.empty() ? nullptr : open.back();
                o.w = k.margin.l + k.border.l + k.padding.l;
                toks.push_back(o);
                open.push_back(&k);
                walk(k);
                open.pop_back();
                Tok c; c.k = K::Close; c.box = &k; c.st = k.st; c.ibox = open.empty() ? nullptr : open.back();
                c.w = k.margin.r + k.border.r + k.padding.r;
                toks.push_back(c);
                deco_stack.pop_back();
                break;
            }
            default:   // Block / Replaced
                if (k.out_of_flow()) { abs_or_float(k); break; }
                atom(k);
            }
        }
    }

    void abs_or_float(Box& k) {
        Tok t; t.k = k.is_float() ? K::Float : K::Abs; t.box = &k; t.st = k.st;
        t.ibox = open.empty() ? nullptr : open.back();
        toks.push_back(t);
    }

    void atom(Box& k) {
        Constraint c;
        c.cb_w = content_w; c.cb_h = -1; c.shrink = true; c.avail_w = content_w;
        resolve_edges(k, content_w);
        if (!measure) layout_box(env, k, c);
        Tok t; t.k = K::Atom; t.box = &k; t.st = k.st; t.ibox = open.empty() ? nullptr : open.back();
        if (measure) { const Intrinsic in = outer_intrinsic(env, k); t.w = in.max; t.text = std::to_string(in.min); }
        else t.w = k.outer_w();
        t.break_before = true; t.break_after = true;
        stamp(t);
        toks.push_back(t);
        prev_space = false;
    }
};

// Active decorations inherited from ancestors of the IFC root.
void collect_block_deco(Ifc& ifc, Box& b) {
    std::vector<Box*> chain;
    for (Box* p = &b; p; p = p->parent) {
        chain.push_back(p);
        if (p->is_atomic_inline() || p->is_float() || p->is_abs()) break;
    }
    for (size_t i = chain.size(); i-- > 0;) ifc.push_deco(*chain[i]->st);
}

// ── vertical metrics ────────────────────────────────────────────────────────

struct Strutm { float asc = 0, desc = 0; };   // above/below the baseline, half-leading included

Strutm strut_of(Env& env, const ComputedStyle& s) {
    const render::FaceMetrics m = env.font.metrics(face_of(env, s), s.font_size);
    const float lh = line_height_px(env, s);
    const float lead = lh - (m.ascent + m.descent);
    const float asc = m.ascent + std::floor(lead / 2.f);
    return {asc, lh - asc};
}

// ── line breaking and line boxes ────────────────────────────────────────────

struct LineBuilder {
    Env& env;
    Box& b;
    Ifc& ifc;
    Bfc& bfc;
    const float ox, oy, content_w;
    const ComputedStyle& bs;
    Strutm root;
    float root_lh;

    float y = 0;                    // current line top (relative to the content top)
    bool first_line = true;
    std::vector<Box*> carried;      // inline boxes open across a line break

    bool line_started = false;
    std::vector<size_t> line;       // token indices on the current line
    float used = 0, line_left = 0, avail = 0;
    bool has_content = false;

    std::vector<size_t> chunk;      // unbreakable run being accumulated
    float chunk_w = 0;
    bool pending_space = false;
    size_t pending_idx = 0;
    size_t cur = 0;                 // index of the token being processed (adjusted on splits)

    LineBuilder(Env& e, Box& blk, Ifc& i, Bfc& f, float x0, float y0, float w)
        : env(e), b(blk), ifc(i), bfc(f), ox(x0), oy(y0), content_w(w), bs(*blk.st) {
        root = strut_of(env, bs);
        root_lh = line_height_px(env, bs);
    }

    void begin_line(float need) {
        for (int guard = 0; guard < 200; ++guard) {
            float l, r;
            bfc.avail(oy + y, root_lh, ox, ox + content_w, l, r);
            line_left = l - ox;
            avail = r - l;
            if (avail + 0.01f >= need || bfc.floats.empty()) break;
            float next = 1e9f;
            for (const auto& f : bfc.floats)
                if (f.y1 > oy + y + 0.001f && f.y0 < oy + y + root_lh) next = std::min(next, f.y1);
            if (next >= 1e9f) break;
            y = next - oy;
        }
        used = first_line ? bs.text_indent : 0.f;
        line.clear();
        has_content = false;
        line_started = true;
    }

    void append(size_t idx) {
        const Tok& t = ifc.toks[idx];
        line.push_back(idx);
        used += t.w;
        if (t.k == K::Word || t.k == K::Atom || t.k == K::Space) has_content = true;
    }

    void commit_chunk() {
        if (chunk.empty()) return;
        if (chunk.size() == 1 && ifc.toks[chunk[0]].hang) {   // preserved spaces hang instead of wrapping
            if (!line_started) begin_line(0);
            append(chunk[0]);
            pending_space = false;
            chunk.clear();
            chunk_w = 0;
            return;
        }
        if (!line_started) begin_line(chunk_w);
        const float sw = pending_space ? ifc.toks[pending_idx].w : 0.f;
        const bool fits = used + (has_content ? sw : 0.f) + chunk_w <= avail + 0.01f;
        if (!has_content || fits) {
            if (has_content && pending_space) append(pending_idx);
            for (size_t i : chunk) append(i);
        } else {
            finish_line(false, false);
            begin_line(chunk_w);
            for (size_t i : chunk) append(i);
        }
        pending_space = false;
        chunk.clear();
        chunk_w = 0;
    }

    void handle_float(Tok& t) {
        Box& f = *t.box;
        f.lparent = &b;
        place_float(env, f, bfc, content_w, ox, oy, oy + y);
        if (line_started) {
            float l, r;
            bfc.avail(oy + y, root_lh, ox, ox + content_w, l, r);
            line_left = l - ox;
            avail = r - l;
        }
    }

    void run() {
        for (cur = 0; cur < ifc.toks.size(); ++cur) {
            Tok& t = ifc.toks[cur];
            switch (t.k) {
            case K::Float: handle_float(t); break;
            case K::Abs: {
                Box& a = *t.box;
                a.static_parent = &b;
                a.lparent = &b;
                const float lx = line_started ? line_left + used : 0.f;
                a.static_x = lx + (chunk.empty() ? 0.f : 0.f);
                a.static_y = y;
                env.abs_pending.push_back(&a);
                break;
            }
            case K::Break:
                commit_chunk();
                if (!line_started) begin_line(0);
                finish_line(true, false);
                pending_space = false;
                break;
            case K::Space:
                if (!t.breakable) { chunk.push_back(cur); chunk_w += t.w; }
                else { commit_chunk(); pending_space = true; pending_idx = cur; }
                break;
            case K::Atom:
                if (!chunk.empty()) commit_chunk();
                chunk.push_back(cur); chunk_w += t.w;
                commit_chunk();
                break;
            default:   // Word, Open, Close, Marker
                chunk.push_back(cur);
                chunk_w += t.w;
                if (t.break_after) commit_chunk();
                break;
            }
        }
        commit_chunk();
        if (line_started && !line.empty()) finish_line(false, true);
        else if (line_started && b.lines.empty()) {}
    }

    // text-overflow: ellipsis on a line that doesn't fit
    void ellipsize() {
        const ComputedStyle& s = bs;
        if (!s.text_overflow_ellipsis) return;
        if (s.overflow_x == style::Overflow::Visible) return;
        float total = first_line ? bs.text_indent : 0.f;
        for (size_t i : line) total += ifc.toks[i].w;
        if (total <= avail + 0.01f) return;
        Tok e = ifc.make_word(nullptr, s, "\xE2\x80\xA6");
        // drop whole tokens from the end until the ellipsis fits
        while (!line.empty() && total + e.w > avail + 0.01f) {
            Tok& last = ifc.toks[line.back()];
            const float w_before = total - last.w;
            if (last.k == K::Word && w_before + e.w <= avail + 0.01f) {
                // keep a prefix of this word
                std::vector<size_t> cuts;
                for (size_t i = 0; i < last.text.size();) { decode(last.text, i); cuts.push_back(i); }
                size_t keep = 0;
                for (size_t c = 0; c < cuts.size(); ++c) {
                    Tok pre = ifc.make_word(last.box, *last.st, last.text.substr(0, cuts[c]));
                    if (w_before + pre.w + e.w <= avail + 0.01f) keep = c + 1; else break;
                }
                if (keep > 0) {
                    Tok pre = ifc.make_word(last.box, *last.st, last.text.substr(0, cuts[keep - 1]));
                    pre.ibox = last.ibox;
                    ifc.toks.push_back(pre);
                    line.back() = ifc.toks.size() - 1;
                    total = w_before + pre.w;
                    break;
                }
            }
            total = w_before;
            line.pop_back();
        }
        if (!line.empty()) { e.ibox = ifc.toks[line.back()].ibox; e.box = ifc.toks[line.back()].box; e.st = ifc.toks[line.back()].st; }
        else { e.box = nullptr; }
        ifc.toks.push_back(e);
        line.push_back(ifc.toks.size() - 1);
    }

    void finish_line(bool forced, bool last) {
        ellipsize();
        LineBox lb;
        // 1. positions along the line
        std::vector<float> xs(line.size());
        float width = first_line ? bs.text_indent : 0.f;
        for (size_t i = 0; i < line.size(); ++i) { xs[i] = width; width += ifc.toks[line[i]].w; }
        const float room = std::max(0.f, avail - width);
        float off = 0, justify_each = 0;
        TextAlign ta = bs.text_align;
        if (ta == TextAlign::Start) ta = TextAlign::Left;
        if (ta == TextAlign::End) ta = TextAlign::Right;
        if (ta == TextAlign::Right) off = room;
        else if (ta == TextAlign::Center) off = room / 2;
        else if (ta == TextAlign::Justify && !forced && !last) {
            int gaps = 0;
            for (size_t i : line) if (ifc.toks[i].k == K::Space) ++gaps;
            if (gaps > 0) justify_each = room / gaps;
        }
        if (justify_each > 0) {
            float add = 0;
            for (size_t i = 0; i < line.size(); ++i) {
                xs[i] += add;
                if (ifc.toks[line[i]].k == K::Space) add += justify_each;
            }
            width = avail;
        }
        off += line_left;

        // 2. vertical metrics
        struct IB {
            Box* box; float shift = 0; Strutm m; float fasc = 0, fdesc = 0;
            float x0 = 0, x1 = 0; bool first = false, last = false;
        };
        std::vector<IB> ibs;
        auto find_ib = [&](Box* bx) -> IB* { for (auto& i : ibs) if (i.box == bx) return &i; return nullptr; };
        std::function<IB&(Box*)> ensure = [&](Box* bx) -> IB& {
            if (IB* e = find_ib(bx)) return *e;
            Box* par = nullptr;
            for (Box* p = bx->parent; p && p != &b; p = p->parent) if (p->kind == Box::Kind::Inline) { par = p; break; }
            float psh = 0;
            const ComputedStyle* pst = &bs;
            if (par) { psh = ensure(par).shift; pst = par->st; }
            IB ib; ib.box = bx;
            ib.m = strut_of(env, *bx->st);
            const render::FaceMetrics fm = env.font.metrics(face_of(env, *bx->st), bx->st->font_size);
            ib.fasc = fm.ascent; ib.fdesc = fm.descent;
            const ComputedStyle& s = *bx->st;
            const render::FaceMetrics pm = env.font.metrics(face_of(env, *pst), pst->font_size);
            float up = 0;
            switch (s.vertical_align) {
            case VerticalAlign::Sub: up = -(pst->font_size / 5.f + 1.f); break;
            case VerticalAlign::Super: up = pst->font_size / 3.f + 1.f; break;
            case VerticalAlign::Middle: up = pm.x_height / 2.f - (ib.m.asc - ib.m.desc) / 2.f; break;
            case VerticalAlign::TextTop: up = pm.ascent - ib.m.asc; break;
            case VerticalAlign::TextBottom: up = -pm.descent + ib.m.desc; break;
            case VerticalAlign::Length: up = s.vertical_align_len.px + s.vertical_align_len.pct * line_height_px(env, s) / 100.f; break;
            default: break;
            }
            ib.shift = psh + up;
            ibs.push_back(ib);
            return ibs.back();
        };
        std::vector<Box*> open_now = carried;
        for (Box* cb : carried) { IB& i = ensure(cb); i.x0 = off; i.first = false; }
        struct AtomInfo { Box* box; float shift; float bo; };
        std::vector<AtomInfo> atoms;
        auto shift_of = [&](Box* ib) { if (!ib) return 0.f; IB* e = find_ib(ib); return e ? e->shift : 0.f; };
        for (size_t i = 0; i < line.size(); ++i) {
            Tok& t = ifc.toks[line[i]];
            if (t.k == K::Open) {
                IB& ib = ensure(t.box);
                ib.x0 = off + xs[i]; ib.first = true;
                open_now.push_back(t.box);
            } else if (t.k == K::Close) {
                if (IB* ib = find_ib(t.box)) { ib->x1 = off + xs[i] + t.w; ib->last = true; }
                for (size_t q = open_now.size(); q-- > 0;) if (open_now[q] == t.box) { open_now.erase(open_now.begin() + q); break; }
            } else if (t.k == K::Atom) {
                Box* a = t.box;
                float bo = a->baseline >= 0 && (a->kind != Box::Kind::Replaced || a->ctl) ? a->margin.t + a->baseline
                                                                                : a->margin.t + a->h + a->margin.b;
                const float below = a->outer_h() - bo;
                const ComputedStyle& pst = t.ibox ? *t.ibox->st : bs;
                const float psh = shift_of(t.ibox);
                const render::FaceMetrics pm = env.font.metrics(face_of(env, pst), pst.font_size);
                float up = 0;
                const ComputedStyle& as = *a->st;
                switch (as.vertical_align) {
                case VerticalAlign::Sub: up = -(pst.font_size / 5.f + 1.f); break;
                case VerticalAlign::Super: up = pst.font_size / 3.f + 1.f; break;
                case VerticalAlign::Middle: up = pm.x_height / 2.f - (bo - below) / 2.f; break;
                case VerticalAlign::TextTop: up = pm.ascent - bo; break;
                case VerticalAlign::TextBottom: up = -pm.descent + below; break;
                case VerticalAlign::Length: up = as.vertical_align_len.px + as.vertical_align_len.pct * line_height_px(env, as) / 100.f; break;
                default: break;
                }
                atoms.push_back({a, psh + up, bo});
            }
        }
        // line-height: normal also covers the fonts the glyphs actually came from (fallback faces: emoji, symbols):
        // each contributes its own ascent/descent (+ half its leading), like Blink's used-fonts accounting
        float fb_asc = 0, fb_desc = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            const Tok& t = ifc.toks[line[i]];
            if (t.k != K::Word || !t.run || !t.st || t.st->lh_mode != 0) continue;
            int seen[4] = {-1, -1, -1, -1}, nseen = 0;
            for (const auto& p : t.run->positions) {
                if (p.face < 0) continue;
                bool dup = false;
                for (int k = 0; k < nseen; ++k) dup = dup || seen[k] == p.face;
                if (dup || nseen == 4) continue;
                seen[nseen++] = p.face;
                const render::FaceMetrics fm = env.font.metrics(render::FaceKey{p.face}, t.st->font_size);
                const float lead = fm.line_gap;
                const float fa = fm.ascent + std::floor(lead / 2.f), fd = fm.ascent + fm.descent + lead - fa;
                if (t.ibox) { IB& ib = ensure(t.ibox); ib.m.asc = std::max(ib.m.asc, fa); ib.m.desc = std::max(ib.m.desc, fd); }
                else { fb_asc = std::max(fb_asc, fa); fb_desc = std::max(fb_desc, fd); }
            }
        }
        // inline boxes that start and end without tokens in between still matter: ensured above
        float asc = std::max(root.asc, fb_asc), desc = std::max(root.desc, fb_desc);
        auto is_edge = [](VerticalAlign v) { return v == VerticalAlign::Top || v == VerticalAlign::Bottom; };
        for (auto& ib : ibs) {
            if (is_edge(ib.box->st->vertical_align)) continue;
            asc = std::max(asc, ib.m.asc + ib.shift);
            desc = std::max(desc, ib.m.desc - ib.shift);
        }
        for (auto& at : atoms) {
            if (is_edge(at.box->st->vertical_align)) continue;
            asc = std::max(asc, at.bo + at.shift);
            desc = std::max(desc, (at.box->outer_h() - at.bo) - at.shift);
        }
        float lh = asc + desc;
        for (auto& ib : ibs) if (is_edge(ib.box->st->vertical_align)) lh = std::max(lh, ib.m.asc + ib.m.desc);
        for (auto& at : atoms) if (is_edge(at.box->st->vertical_align)) lh = std::max(lh, at.box->outer_h());
        if (lh > asc + desc) desc = lh - asc;
        const float base = asc;
        lb.y = y; lb.h = lh; lb.baseline = base;

        // 3. fragments: inline boxes first (backgrounds), then text and atoms
        for (auto& ib : ibs) {
            const Box& bx = *ib.box;
            if (!ib.first && ib.x0 == 0) ib.x0 = off;
            const float x1 = ib.last ? ib.x1 : off + width;
            LineFrag f; f.kind = LineFrag::Kind::Inline; f.box = ib.box;
            f.first = ib.first; f.last = ib.last;
            f.x = ib.x0 + (ib.first ? bx.margin.l : 0.f);
            f.w = std::max(0.f, (x1 - ib.x0) - (ib.first ? bx.margin.l : 0.f) - (ib.last ? bx.margin.r : 0.f));
            float bl = base - ib.shift;
            if (bx.st->vertical_align == VerticalAlign::Top) bl = ib.m.asc;
            else if (bx.st->vertical_align == VerticalAlign::Bottom) bl = lh - ib.m.desc;
            f.top = bl - ib.fasc - bx.padding.t - bx.border.t;
            f.bottom = bl + ib.fdesc + bx.padding.b + bx.border.b;
            f.baseline = bl;
            lb.frags.push_back(f);
        }
        LineFrag curf; bool have = false;
        auto flush_text = [&] { if (have) { lb.frags.push_back(std::move(curf)); curf = LineFrag{}; have = false; } };
        for (size_t i = 0; i < line.size(); ++i) {
            Tok& t = ifc.toks[line[i]];
            const float x = off + xs[i];
            if (t.k == K::Word || t.k == K::Space) {
                if (have && (curf.box != t.box || curf.deco != t.deco || curf.deco_color != t.deco_color ||
                             curf.baseline != base - shift_of(t.ibox))) flush_text();
                if (!have) {
                    have = true;
                    curf = LineFrag{};
                    curf.kind = LineFrag::Kind::Text;
                    curf.box = t.box; curf.x = x;
                    curf.baseline = base - shift_of(t.ibox);
                    curf.run = std::make_shared<render::ShapedRun>();
                    curf.face = face_of(env, *t.st); curf.px = t.st->font_size;
                    curf.deco = t.deco; curf.deco_color = t.deco_color; curf.deco_style = t.deco_style;
                    curf.top = 0; curf.bottom = lh;
                }
                auto* r = const_cast<render::ShapedRun*>(curf.run.get());
                if (t.k == K::Word && t.run) {
                    for (auto p : t.run->positions) { p.x += (x - curf.x); r->positions.push_back(p); }
                    curf.text += t.text;
                    curf.words.push_back({t.text, x - curf.x, t.w});
                } else curf.text += ' ';
                curf.w = (x - curf.x) + t.w;
                r->width = curf.w;
            } else if (t.k == K::Atom) {
                flush_text();
                Box* a = t.box;
                float sh = 0, bo = 0;
                for (auto& at : atoms) if (at.box == a) { sh = at.shift; bo = at.bo; }
                float top = base - sh - bo;
                if (a->st->vertical_align == VerticalAlign::Top) top = 0;
                else if (a->st->vertical_align == VerticalAlign::Bottom) top = lh - a->outer_h();
                a->x = x + a->margin.l;
                a->y = y + top + a->margin.t;
                a->lparent = &b;
                LineFrag f; f.kind = LineFrag::Kind::Atom; f.box = a; f.x = x; f.w = t.w; f.top = top; f.bottom = top + a->outer_h();
                lb.frags.push_back(f);
            } else if (t.k != K::Open && t.k != K::Close) {
                flush_text();
            }
        }
        flush_text();
        carried = open_now;
        b.lines.push_back(std::move(lb));
        y += lh;
        first_line = false;
        line.clear();
        line_started = false;
        used = 0;
        has_content = false;
    }
};

} // namespace

float layout_inline_content(Env& env, Box& b, float content_w, Bfc& bfc, float ox, float oy) {
    Ifc ifc(env, b, content_w);
    collect_block_deco(ifc, b);
    ifc.walk(b);
    b.lines.clear();
    LineBuilder lbld(env, b, ifc, bfc, ox, oy, content_w);
    lbld.run();
    return lbld.y;
}

Intrinsic inline_intrinsic(Env& env, Box& b) {
    Ifc ifc(env, b, 0);
    ifc.measure = true;
    ifc.walk(b);
    float mn = 0, mx = 0, line = b.st->text_indent, chunk = 0, trailing = 0;
    for (const Tok& t : ifc.toks) {
        switch (t.k) {
        case K::Word:
            chunk += t.w; line += t.w; trailing = 0;
            if (t.break_after) { mn = std::max(mn, chunk); chunk = 0; }
            break;
        case K::Space:
            if (t.breakable) { mn = std::max(mn, chunk); chunk = 0; trailing += t.w; line += t.w; }
            else { chunk += t.w; line += t.w; trailing = 0; }
            break;
        case K::Open: case K::Close: case K::Marker: chunk += t.w; line += t.w; trailing = 0; break;
        case K::Atom: {
            mn = std::max(mn, chunk);
            float amin = 0;
            try { amin = std::stof(t.text); } catch (...) {}
            mn = std::max(mn, amin);
            chunk = 0; line += t.w; trailing = 0;
            break;
        }
        case K::Float: {
            const Intrinsic fi = outer_intrinsic(env, *t.box);
            mn = std::max(mn, fi.min); line += fi.max;
            break;
        }
        case K::Break:
            mn = std::max(mn, chunk);
            mx = std::max(mx, line - trailing);
            chunk = 0; line = 0; trailing = 0;
            break;
        default: break;
        }
    }
    mn = std::max(mn, chunk);
    mx = std::max(mx, line - trailing);
    return {mn, mx};
}

} // namespace luxium::layout
