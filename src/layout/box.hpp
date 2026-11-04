#pragma once
// The box tree and the shared declarations of the layout engine.
//
//   DOM + ComputedStyle  --build-->  Box tree  --layout-->  geometry
//                                                  --paint-->  DisplayList
//
// A Box carries its used values (margins, borders, padding, size) and, for
// block containers with inline content, the line boxes.  Geometry is stored
// relative to the parent's content origin; `finalize` turns it into absolute
// document coordinates (ax, ay) once everything is placed.

#include <dom/node.hpp>
#include <render/images.hpp>
#include <render/layout.hpp>
#include <render/text.hpp>
#include <paint/display_list.hpp>
#include <style/cascade.hpp>

#include <deque>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace luxium::layout {

using style::ComputedStyle;
using style::Display;
using style::Len;

struct Box;

struct Edges {
    float t = 0, r = 0, b = 0, l = 0;
    float h() const { return l + r; }
    float v() const { return t + b; }
};

// Margin collapsing: positives and negatives collapse separately.
struct Strut {
    float pos = 0, neg = 0;
    void add(float m) { if (m >= 0) pos = std::max(pos, m); else neg = std::min(neg, m); }
    void merge(const Strut& o) { pos = std::max(pos, o.pos); neg = std::min(neg, o.neg); }
    float value() const { return pos + neg; }
};

// One positioned piece of a line.
struct WordSpan { std::string text; float x = 0, w = 0; };   // for text selection

struct LineFrag {
    enum class Kind { Text, Atom, Inline, Marker } kind = Kind::Text;
    Box* box = nullptr;
    const ComputedStyle* st = nullptr;  // Text: the style the run was shaped with (differs from box->st on a ::first-line)
    float x = 0, w = 0;                 // from the block's content-left edge
    float top = 0, bottom = 0;          // Inline: border-box vertical extent, relative to line top
    float baseline = 0;                 // Text/Marker: baseline y relative to line top
    std::shared_ptr<const render::ShapedRun> run;   // Text
    render::FaceKey face;               // Text
    float px = 16;                      // Text: font size
    std::string text;                   // Text (debug, ellipsis)
    bool first = false, last = false;   // Inline: first/last fragment of its box (edges)
    float deco_w = 0;                   // decoration width (text + trailing glue)
    int deco = 0;                       // 1 underline | 2 line-through | 4 overline (propagated)
    uint32_t deco_color = 0;
    const ComputedStyle* deco_style = nullptr;
    std::vector<WordSpan> words;        // Text: the words it contains
};

struct LineBox {
    float y = 0, h = 0, baseline = 0;   // y relative to the block's content top
    std::vector<LineFrag> frags;
};

// A form control drawn by the engine itself (<input>, <textarea>, <select>): the box is a replaced
// element whose intrinsic size comes from the font, and the paint pass draws its text/glyphs.
struct Control {
    enum class Type { Text, Password, Textarea, Select, Checkbox, Radio, Range, File, Progress, Meter, Color } type = Type::Text;
    std::string text;                   // value / selected option / file name; "" when empty
    std::string placeholder;
    bool checked = false, disabled = false, readonly = false;
    int size = 20, cols = 20, rows = 2; // text: size attr; textarea: cols/rows
    float frac = 0;                     // range / progress / meter: position 0..1
    bool  indeterminate = false;        // progress without a value
    int   meter_state = 0;              // meter: 0 in the optimum region, 1 sub-optimal, 2 far from it
    uint32_t color = 0xFF000000;        // input type=color
    std::vector<std::string> options;   // select: option labels (popup)
    int selected = 0;                   // select: index into options
    bool multi = false;
    uint32_t ph_color = 0xFF757575;     // ::placeholder colour
};

struct Box {
    enum class Kind { Block, Inline, Text, Replaced, Break, Marker } kind = Kind::Block;
    const dom::Node* node = nullptr;
    const ComputedStyle* st = nullptr;
    Box* parent = nullptr;
    std::vector<std::unique_ptr<Box>> kids;
    std::string text;                   // Text / Marker
    bool anonymous = false;
    int order = 0;                      // tree order (pre-order index)

    // replaced content
    std::shared_ptr<const render::ImageBitmap> img;
    float nat_w = 0, nat_h = 0;
    bool broken_img = false;
    bool media = false;                 // <video>/<audio>: img is the poster, the shell patches in the frames
    bool media_controls = false;
    bool no_ratio = false;              // replaced without an intrinsic aspect ratio (unloaded <video>)
    std::shared_ptr<Control> ctl;       // form control (kind == Replaced)
    // list marker
    int marker_shape = 0;               // 0 text, 1 disc, 2 circle, 3 square

    // used values (px)
    float x = 0, y = 0, w = 0, h = 0;   // border box; relative to parent's content origin
    Edges margin, border, padding;
    bool mar_l_auto = false, mar_r_auto = false, mar_t_auto = false, mar_b_auto = false;
    float baseline = -1;                // distance from border-box top to the (last) baseline; <0 = none
    float ctl_baseline = 0;             // controls: baseline from the content-box top
    std::vector<LineBox> lines;
    const ComputedStyle* first_line_st = nullptr;   // ::first-line style (inline layout restyles the first line's own text)
    std::vector<float> sub_cols, sub_rows;   // subgrid: the parent's track sizes over this box's span (set by the parent grid)
    float sub_cgap = 0, sub_rgap = 0;
    std::vector<float> own_rows;             // grid container: the row track sizes it used (a rows-subgrid child feeds them to its parent)
    int mc_n = 0; float mc_w = 0, mc_gap = 0;   // multicol container: columns used, column width, gap (painting the rules)
    Strut bottom_strut;                 // margin sticking out below (collapsed with last child)
    bool has_inline_content = false;
    float scroll_h = 0;                 // in-flow content height
    float max_sx = 0, max_sy = 0;       // scroll containers: how far the content can be scrolled (finalize)
    float ext_r = 0, ext_b = 0;         // absolute right/bottom extent of the box and its overflowing content
    float content_right = 0;            // rightmost extent of content (intrinsic w/ overflow)

    // intrinsic widths of the CONTENT box (min-content / max-content)
    mutable bool has_intr = false;
    mutable float min_c = 0, max_c = 0;

    // out-of-flow
    float static_x = 0, static_y = 0;   // static position, relative to parent content origin
    float static_fx = 0, static_fy = 0; // flex parent: where the static position sits in its content box (0 start .. 1 end)
    Box* cb = nullptr;                  // containing block for abs/fixed (null = initial CB)
    Box* static_parent = nullptr;       // block whose content origin static_x/static_y refer to
    Box* lparent = nullptr;             // layout parent when != parent (inline descendants, table cells)

    // final absolute geometry (finalize)
    float ax = 0, ay = 0;               // border-box origin in document coordinates
    float rel_dx = 0, rel_dy = 0;       // position: relative visual offset

    // table
    int col_span = 1, row_span = 1;
    bool border_collapsed = false;      // cell/table: borders belong to the collapsed model (table paints them)
    Edges coll_border;                  // used border widths in the collapsed model
    struct Edge { float x = 0, y = 0, w = 0, h = 0; uint32_t color = 0; style::BorderStyle style = style::BorderStyle::Solid; };
    std::vector<Edge> table_edges;      // table: collapsed border segments, relative to the table border box

    bool is_text() const { return kind == Kind::Text; }
    // text/markers share their parent's style: those flags belong to the element, not to them
    bool own_style() const { return st && kind != Kind::Text && kind != Kind::Marker && kind != Kind::Break; }
    bool is_float() const { return own_style() && st->float_ != style::Float::None; }
    bool is_abs() const { return own_style() && (st->position == style::Position::Absolute || st->position == style::Position::Fixed); }
    bool out_of_flow() const { return is_float() || is_abs(); }
    bool is_positioned() const { return own_style() && st->position != style::Position::Static; }
    // atomic inline-level: inline-block/inline-flex/... or inline replaced
    bool is_atomic_inline() const;
    bool inline_level() const;          // participates in an inline formatting context
    float outer_w() const { return w + margin.h(); }
    float outer_h() const { return h + margin.v(); }
    float content_w() const { return w - border.h() - padding.h(); }
    float content_h() const { return h - border.v() - padding.v(); }
};

// Owns styles and the root box.
struct Tree {
    std::deque<ComputedStyle> styles;
    std::unique_ptr<Box> root;           // the <html> element box (or empty document)
    std::vector<Box*> order;             // pre-order
    const ComputedStyle* html_style = nullptr;
    const ComputedStyle* body_style = nullptr;
    Box* body_box = nullptr;
};

// ── shared engine state ─────────────────────────────────────────────────────

struct Env {
    render::Font& font;
    const style::Resolver& res;
    float vw = 1180, vh = 762;           // viewport (layout) size
    std::string base_dir;
    // absolutely positioned boxes waiting for their containing block to finish
    std::vector<Box*> abs_pending;
    std::unordered_map<std::string, std::shared_ptr<const render::ShapedRun>> shape_cache;
    std::unordered_map<const ComputedStyle*, render::FaceKey> face_cache;
};

struct FloatRec {
    float x0, x1, y0, y1;                // BFC coordinates (margin box)
    bool left;
    Box* box;
};

// Floats live per block formatting context.
struct Bfc {
    std::vector<FloatRec> floats;
    float bottom_of(bool left, bool right) const;   // lowest bottom among left/right floats
    // available horizontal range [l, r) for a box of height h at y (BFC coords), within [x0, x1]
    void avail(float y, float h, float x0, float x1, float& l, float& r) const;
};

struct Constraint {
    float cb_w = 0;                      // containing block content width (for %)
    float cb_h = -1;                     // containing block content height, <0 if indefinite
    float forced_w = -1, forced_h = -1;  // border-box overrides (flex/grid/table/abs)
    bool  shrink = false;                // auto width = shrink-to-fit (floats, inline-block, abs...)
    float avail_w = -1;                  // shrink-to-fit: room for the margin box
    Bfc*  bfc = nullptr;                 // current BFC (null: the box starts its own)
    float bfc_x = 0, bfc_y = 0;          // border-box origin in BFC coordinates
    bool  stretch_h = false;             // forced_h is a definite size for % children (flex stretch)
};

// ── build.cpp ───────────────────────────────────────────────────────────────
std::unique_ptr<Tree> build_tree(const dom::Node& document, const style::Resolver& res,
                                 const std::string& base_dir);
ComputedStyle inherited_style(const ComputedStyle& parent, Display d);

// ── block.cpp ───────────────────────────────────────────────────────────────
void layout_box(Env& env, Box& b, const Constraint& c);
float resolve_len(const Len& l, float base);          // px + pct*base (auto -> 0)
void resolve_edges(Box& b, float cb_w);                // margins/borders/padding -> px
struct Intrinsic { float min = 0, max = 0; };
Intrinsic intrinsic_widths(Env& env, Box& b);          // content-box min/max-content
Intrinsic outer_intrinsic(Env& env, Box& c);           // + margins/borders/padding, honors px width
void resolve_abs(Env& env, Box* container, size_t from);   // lays out pending abs boxes whose cb is `container`
void place_float(Env& env, Box& f, Bfc& bfc, float content_w, float ox, float oy, float y_top);
float place_atom_static(Box& b);
bool establishes_bfc(const Box& b);
bool is_block_flow(const Box& b);                      // lays out as block container
float last_baseline(const Box& b);

// ── controls.cpp ────────────────────────────────────────────────────────────
// Fills nat_w/nat_h/baseline of a form-control box from the font (idempotent).
void ctl_metrics(Env& env, Box& b);
std::string password_mask(const std::string& value);

// ── inline.cpp ──────────────────────────────────────────────────────────────
// Lays out the inline content of block container `b` (children are inline-level).
// Returns the content height; fills b.lines.  `cx0` is the BFC x of the content
// left edge, `cy0` the BFC y of the content top.
float layout_inline_content(Env& env, Box& b, float content_w, Bfc& bfc, float cx0, float cy0);
Intrinsic inline_intrinsic(Env& env, Box& b);
render::FaceKey face_of(Env& env, const ComputedStyle& s);
float line_height_px(Env& env, const ComputedStyle& s);   // `normal` uses the face's own metrics

// ── flex.cpp / grid.cpp / table.cpp ─────────────────────────────────────────
// Lays out the children of a flex/grid container; sets b.h-relevant results through
// `content_h` (the content height) and positions every child.
float layout_flex(Env& env, Box& b, const Constraint& c, float content_w, float content_h_def,
                  float min_h = 0, float max_h = -1);
Intrinsic flex_intrinsic(Env& env, Box& b);
float layout_grid(Env& env, Box& b, const Constraint& c, float content_w, float content_h_def,
                  float min_h = 0, float max_h = -1);
Intrinsic grid_intrinsic(Env& env, Box& b);
// Sizes and lays out a whole table box (width included).
void layout_table(Env& env, Box& b, const Constraint& c);
Intrinsic table_intrinsic(Env& env, Box& b);

// ── layout.cpp / paint.cpp ──────────────────────────────────────────────────
struct HitRect {
    const dom::Node* node = nullptr; float x = 0, y = 0, w = 0, h = 0; bool button = false;
    int cursor = 0;                     // style cursor of the box (see ComputedStyle::cursor)
    // form controls: text origin (document x of the content box) and the font the text is drawn with
    int ctl = -1; float tx = 0, ty = 0, th = 0, px = 0; int face = 0; bool password = false;
    // visible part of the box: intersection of the clipping ancestors' padding boxes (document coords)
    float clip_x0 = -1e9f, clip_y0 = -1e9f, clip_x1 = 1e9f, clip_y1 = 1e9f;
    // scroll containers: reachable offsets and the vertical track (0 width = no scrollbar)
    float max_sx = 0, max_sy = 0, sb_x = 0, sb_y = 0, sb_w = 0, sb_h = 0;
    int sticky = -1;
};
struct NamedRect { std::string name; float x = 0, y = 0, w = 0, h = 0; };
struct WordRect { std::string text; float x = 0, y = 0, w = 0, h = 0; };
using MediaRect = render::MediaBox;
struct PaintOutput {
    paint::DisplayList main, fixed;
    std::vector<HitRect> hits;
    std::vector<HitRect> areas;
    std::vector<NamedRect> boxes;
    std::vector<WordRect> words;
    std::vector<MediaRect> media;
    std::vector<render::StickyGroup> sticky;
    uint32_t canvas_bg = 0xFFFFFFFF;
};
void finalize_positions(Env& env, Tree& tree);
void paint_tree(Env& env, Tree& tree, float canvas_w, float canvas_h, PaintOutput& out);

} // namespace luxium::layout
