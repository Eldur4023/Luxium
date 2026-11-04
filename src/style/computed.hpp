#pragma once
// ComputedStyle: every property the engine honours, typed and resolved
// (lengths in px or %-of-something, colors ARGB, enums), with inheritance
// and initial values.  Blink's ComputedStyle in miniature, and the shape the
// future "compile the page to one file" format can serialise: no strings
// survive past computation except url()s and content.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace luxium::style {

// A length: px + pct% of some base the layout supplies (width -> containing
// block width, etc.).  calc(100% - 40px) is {px=-40, pct=100}.  `auto`
// carries no number.
struct Len {
    float px = 0, pct = 0;
    bool  is_auto = false;
    unsigned char kw = 0;       // width/height: 1 fit-content, 2 max-content, 3 min-content (with is_auto: other code sees `auto`)
    bool  pct_unit = false;     // written with %, even if 0% (flex-basis: 0% != 0px)
    // min()/max()/clamp() mixing % and px: fn 1 = min(t1,t2), 2 = max(t1,t2), 3 = clamp(t1,t2,t3), t1 = (px,pct)
    unsigned char fn = 0;
    float px2 = 0, pct2 = 0, px3 = 0, pct3 = 0;
    static Len Auto() { Len l; l.is_auto = true; return l; }
    static Len Px(float v) { Len l; l.px = v; return l; }
    static Len Pct(float v) { Len l; l.pct = v; l.pct_unit = true; return l; }
    bool has_pct() const { return pct != 0.f || pct_unit || (fn && (pct2 != 0.f || pct3 != 0.f)); }
    float resolve(float base) const {
        const float a = px + pct * base / 100.f;
        if (!fn) return a;
        const float b = px2 + pct2 * base / 100.f, c = px3 + pct3 * base / 100.f;
        return fn == 1 ? std::min(a, b) : fn == 2 ? std::max(a, b) : std::max(a, std::min(b, c));
    }
    bool operator==(const Len& o) const { return px == o.px && pct == o.pct && is_auto == o.is_auto && kw == o.kw && fn == o.fn && px2 == o.px2 && pct2 == o.pct2 && px3 == o.px3 && pct3 == o.pct3; }
};

constexpr uint32_t kTransparent = 0x00000000;

enum class Display { None, Contents, Block, Inline, InlineBlock, Flex, InlineFlex, Grid, InlineGrid,
                     Table, InlineTable, TableRow, TableCell, TableRowGroup, TableHeaderGroup,
                     TableFooterGroup, TableColumn, TableColumnGroup, TableCaption, ListItem, FlowRoot };
enum class Position { Static, Relative, Absolute, Fixed, Sticky };
enum class Float { None, Left, Right };
enum class Clear { None, Left, Right, Both };
enum class BoxSizing { Content, Border };
enum class Overflow { Visible, Hidden, Scroll, Auto, Clip };
enum class Visibility { Visible, Hidden, Collapse };
enum class BorderStyle { None, Hidden, Solid, Dashed, Dotted, Double, Groove, Ridge, Inset, Outset };
enum class TextAlign { Start, End, Left, Right, Center, Justify };
enum class WhiteSpace { Normal, Nowrap, Pre, PreWrap, PreLine, BreakSpaces };
enum class TextTransform { None, Uppercase, Lowercase, Capitalize };
enum class OverflowWrap { Normal, BreakWord, Anywhere };
enum class WordBreak { Normal, BreakAll, KeepAll, BreakWord };
enum class VerticalAlign { Baseline, Sub, Super, Top, TextTop, Middle, Bottom, TextBottom, Length };
enum class FontFamily { Sans, Serif, Mono };
enum class ListStyle { Disc, Circle, Square, Decimal, LowerAlpha, UpperAlpha, LowerRoman, UpperRoman, None, DecimalLeadingZero };
enum class FlexDirection { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap { Nowrap, Wrap, WrapReverse };
// justify-content / align-content / align-items / align-self / justify-items / justify-self
enum class Align { Auto, Normal, Stretch, Start, End, Center, FlexStart, FlexEnd, Baseline,
                   SpaceBetween, SpaceAround, SpaceEvenly };
enum class ObjectFit { Fill, Contain, Cover, None, ScaleDown };
enum class BgRepeat { Repeat, NoRepeat, Space, Round };
enum class BgSizeMode { Auto, Cover, Contain, Explicit };

struct GradStop {
    uint32_t color = 0;
    Len      pos;            // is_auto = distribute evenly
};
struct Gradient {
    bool radial = false;
    bool conic = false;      // conic-gradient: stops run clockwise round (cx, cy), `angle` is `from`
    std::string interp;      // `in <space> [<hue> hue]` of the gradient ("" = sRGB): see mix_in_space
    float angle = 180.f;     // linear, degrees (CSS: 0 = to top)
    int   corner = 0;        // `to top right` & co: 1 TR, 2 BR, 3 BL, 4 TL (angle depends on the box)
    bool  circle = false;    // radial shape
    int   extent = 0;        // radial: 0 farthest-corner, 1 closest-side, 2 farthest-side, 3 closest-corner
    Len   cx = Len::Pct(50), cy = Len::Pct(50);
    bool  repeating = false;
    std::vector<GradStop> stops;
};

struct BgLayer {
    enum class Kind { None, Url, Gradient } kind = Kind::None;
    std::string url;
    Gradient    grad;
    BgRepeat    repeat_x = BgRepeat::Repeat, repeat_y = BgRepeat::Repeat;
    Len         pos_x = Len::Pct(0), pos_y = Len::Pct(0);
    BgSizeMode  size_mode = BgSizeMode::Auto;
    Len         size_w = Len::Auto(), size_h = Len::Auto();
    bool        clip_padding = false;   // background-clip: padding-box
};

struct Shadow {
    float x = 0, y = 0, blur = 0, spread = 0;
    uint32_t color = 0xFF000000;
    bool inset = false;
};

// One filter function of `filter` / `backdrop-filter`.  `a`: the amount (blur: radius in px, hue-rotate: degrees,
// the rest: a factor where 1 = unchanged / 100 %).  drop-shadow: dx, dy, blur (radius), color.
struct FilterFn {
    enum class Kind : uint8_t { Blur, Brightness, Contrast, Grayscale, Sepia, Saturate, HueRotate, Invert, Opacity, DropShadow } kind = Kind::Blur;
    float a = 0, dx = 0, dy = 0, blur = 0;
    uint32_t color = 0xFF000000;
};

// One piece of `content` when it holds counter()/counters(); strings between them are Str.
struct ContentItem {
    enum class Kind : uint8_t { Str, Counter, Counters } kind = Kind::Str;
    std::string str;                       // Str: the text
    std::string name, sep;                 // Counter/Counters: counter name, separator (Counters)
    ListStyle   style = ListStyle::Decimal;
};

// `clip-path` basic shape.  box: reference box 0 border, 1 padding, 2 content, 3 margin.
struct ClipPath {
    enum class Kind : uint8_t { None, Inset, Circle, Ellipse, Polygon } kind = Kind::None;
    int box = 0;
    Len edge[4];                 // inset(): top right bottom left
    Len rx[4], ry[4];            // inset() round: tl tr br bl (horizontal / vertical radii)
    Len r[2];                    // circle: r[0]; ellipse: rx, ry (keywords go in rkw)
    int rkw[2] = {0, 0};         // 0 length, 1 closest-side, 2 farthest-side
    Len cx = Len::Pct(50), cy = Len::Pct(50);
    bool evenodd = false;
    std::vector<std::pair<Len, Len>> pts;
};

// grid track size: fr, px/%, auto, min-content, max-content, minmax(a, b)
struct Track {
    enum class Kind { Len, Fr, Auto, MinContent, MaxContent, MinMax } kind = Kind::Auto;
    Len   len;               // Kind::Len
    float fr = 0;            // Kind::Fr
    // minmax(): min/max as nested single-value tracks (not recursive)
    int   min_kind = 0, max_kind = 0;      // 0 len, 1 fr, 2 auto, 3 min-content, 4 max-content
    Len   min_len, max_len;
    float max_fr = 0;
};
struct GridLine {            // grid-column-start & co
    bool auto_ = true;
    int  line = 0;           // explicit line number (1-based, negative from end), 0 if none
    int  span = 0;           // span N (0 if not a span)
    std::string name;
};

// One entry of `transition`: which property (bit set), how long, delay, easing.
struct TransitionSpec {
    enum Prop : unsigned { Opacity = 1, Transform = 2, Background = 4, Color = 8, Border = 16, Length = 32, All = 63 };
    unsigned props = 0;
    float duration = 0, delay = 0;    // ms
    float bez[4] = {0.25f, 0.1f, 0.25f, 1.f};   // cubic-bezier control points (ease)
};

// One transform function: T translate(x, y), S scale(a, b), R rotate(a deg), K skew(a, b deg), M matrix(a..f).
struct TfFn {
    char kind = 'T';
    Len  x = Len::Px(0), y = Len::Px(0);
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
};

// One entry of `animation`: which @keyframes, timing and playback.
struct AnimationSpec {
    std::string name;
    float duration = 0, delay = 0;                // ms
    float bez[4] = {0.25f, 0.1f, 0.25f, 1.f};     // timing function (ease), or...
    int   steps = 0;                              // ...steps(n) when > 0
    bool  steps_start = false;                    // jump-start (steps(n, start))
    float iterations = 1;                         // infinity for `infinite`
    int   direction = 0;                          // 0 normal, 1 reverse, 2 alternate, 3 alternate-reverse
    int   fill = 0;                               // bit 1 forwards, bit 2 backwards
    bool  paused = false;
};

struct ComputedStyle {
    uint32_t cache_id = 0;              // identity of this exact style within one layout (see Resolver::compute)
    // ── display / positioning ───────────────────────────────────────────────
    Display  display = Display::Inline;
    Position position = Position::Static;
    Float    float_ = Float::None;
    Clear    clear = Clear::None;
    BoxSizing box_sizing = BoxSizing::Content;
    Overflow overflow_x = Overflow::Visible, overflow_y = Overflow::Visible;
    Visibility visibility = Visibility::Visible;
    float    opacity = 1.f;
    int      mix_blend = 0;   // mix-blend-mode: 0 normal, 1 multiply, 2 screen, 3 overlay, 4 darken, 5 lighten, 6 color-dodge, 7 color-burn, 8 hard-light, 9 soft-light, 10 difference, 11 exclusion
    bool     z_auto = true;
    int      z_index = 0;

    // ── box ─────────────────────────────────────────────────────────────────
    Len width = Len::Auto(), height = Len::Auto();
    Len min_width, min_height;                          // 0
    Len max_width = Len::Auto(), max_height = Len::Auto();   // auto == none
    Len margin[4] = {Len::Px(0), Len::Px(0), Len::Px(0), Len::Px(0)};    // top right bottom left
    Len padding[4] = {Len::Px(0), Len::Px(0), Len::Px(0), Len::Px(0)};
    float       border_w[4] = {3, 3, 3, 3};   // medium; zeroed when the style is none
    BorderStyle border_style[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};
    uint32_t    border_color[4] = {0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000};
    bool        bc_current[4] = {true, true, true, true};   // currentcolor: follows `color`
    Len      radius[4] = {Len::Px(0), Len::Px(0), Len::Px(0), Len::Px(0)};   // tl tr br bl
    Len      radiusv[4] = {Len::Px(0), Len::Px(0), Len::Px(0), Len::Px(0)};  // vertical radii, only read when radius_ell
    bool     radius_ell = false;   // border-radius with a `/` or two values per corner: elliptical
    Len      inset[4] = {Len::Auto(), Len::Auto(), Len::Auto(), Len::Auto()};  // top right bottom left

    // ── painting ────────────────────────────────────────────────────────────
    uint32_t bg_color = kTransparent;
    std::vector<BgLayer> bg;
    std::vector<int> bg_blend;   // background-blend-mode per layer (cycled), same codes as mix_blend; empty = normal
    std::vector<Shadow>  box_shadow;
    std::vector<Shadow>  text_shadow;   // inherited
    std::vector<FilterFn> filter, backdrop_filter;
    std::vector<BgLayer> mask;          // mask-image layers (+ mask-size/-position/-repeat), alpha multiplies the element
    ClipPath clip_path;
    ObjectFit object_fit = ObjectFit::Fill;
    // -webkit-line-clamp: only with display: -webkit-box / -webkit-inline-box and -webkit-box-orient: vertical
    bool     webkit_box = false, box_orient_v = false;
    int      line_clamp = 0;
    Len      object_pos_x = Len::Pct(50), object_pos_y = Len::Pct(50);   // object-position

    // ── text ────────────────────────────────────────────────────────────────
    uint32_t color = 0xFF000000;
    float    font_size = 16.f;
    int      font_weight = 400;
    bool     italic = false;
    FontFamily family = FontFamily::Sans;
    std::string family_name;
    int      lh_mode = 0;          // 0 normal, 1 number, 2 px
    float    lh_val = 0;
    TextAlign text_align = TextAlign::Start;
    bool     underline = false, line_through = false, overline = false;
    uint32_t decoration_color = 0;     // 0 = currentcolor
    TextTransform text_transform = TextTransform::None;
    float    letter_spacing = 0, word_spacing = 0;
    float    text_indent = 0;
    WhiteSpace white_space = WhiteSpace::Normal;
    bool     text_overflow_ellipsis = false;
    OverflowWrap overflow_wrap = OverflowWrap::Normal;
    WordBreak word_break = WordBreak::Normal;
    VerticalAlign vertical_align = VerticalAlign::Baseline;
    Len      vertical_align_len;
    ListStyle list_style = ListStyle::Disc;
    bool     bg_clip_text = false;     // background-clip: text -- the background shows through the glyphs only
    bool     list_inside = false;
    std::string list_image;            // list-style-image: url(...) (inherited)

    // ── flex ────────────────────────────────────────────────────────────────
    FlexDirection flex_direction = FlexDirection::Row;
    FlexWrap flex_wrap = FlexWrap::Nowrap;
    Align    justify_content = Align::Normal;
    Align    align_items = Align::Normal;
    Align    align_self = Align::Auto;
    Align    align_content = Align::Normal;
    Align    justify_items = Align::Normal;
    Align    justify_self = Align::Auto;
    int      order = 0;
    float    flex_grow = 0, flex_shrink = 1;
    Len      flex_basis = Len::Auto();
    int      column_count = 0;            // multicol: 0 = auto
    Len      column_width = Len::Auto();  // multicol: auto = from column-count
    bool     row_gap_set = false;
    bool     column_gap_set = false;      // column-gap other than `normal` (multicol: normal = 1em)
    float    column_rule_w = 0; style::BorderStyle column_rule_style = style::BorderStyle::None;
    uint32_t column_rule_color = 0; bool column_rule_cur = true;
    bool     multicol() const { return column_count > 0 || !column_width.is_auto; }
    Len      row_gap, column_gap;      // normal == 0 here

    // ── grid ────────────────────────────────────────────────────────────────
    std::vector<Track> grid_template_columns, grid_template_rows;
    bool     grid_cols_sub = false, grid_rows_sub = false;   // grid-template-columns/rows: subgrid
    std::vector<Track> grid_auto_columns, grid_auto_rows;
    bool     grid_auto_flow_column = false, grid_auto_flow_dense = false;
    GridLine grid_column_start, grid_column_end, grid_row_start, grid_row_end;
    std::vector<std::vector<std::string>> grid_template_areas;   // rows of names
    std::string grid_area_name;                                  // grid-area: <ident>
    // repeat(auto-fill|auto-fit, ...) on the column axis (resolved at layout)
    int      grid_col_autorepeat = 0;          // 0 none, 1 auto-fill, 2 auto-fit
    std::vector<Track> grid_col_auto_pattern;
    size_t   grid_col_auto_pos = 0;            // index in grid_template_columns where it expands

    // ── table ───────────────────────────────────────────────────────────────
    bool     border_collapse = false;
    float    border_spacing_h = 2, border_spacing_v = 2;
    bool     table_fixed = false;

    // ── generated content / misc ────────────────────────────────────────────
    bool     has_content = false;       // ::before/::after generate a box iff true
    std::string content;
    std::vector<ContentItem> content_items;   // only when `content` uses counter()/counters()
    // counter-reset / counter-increment / counter-set (name, integer); not inherited
    std::vector<std::pair<std::string, int>> counter_reset, counter_inc, counter_set;
    int      cursor = 0;                // 0 auto, 1 pointer, 2 text, 3 default, 4 not-allowed, 5 grab, 6 grabbing
    bool     pointer_events = true;
    uint32_t accent = 0;                // accent-color (0 = auto)
    bool     appearance_none = false;   // appearance: none (controls keep only their CSS look)
    bool     scrollbar_thin = false, scrollbar_none = false, scrollbar_set = false;   // scrollbar-width: thin; scrollbar-color given
    uint32_t scrollbar_thumb = 0, scrollbar_track = 0;
    // transform: a 2D affine map p' = O + L (p - O) + t with L = [tf0 tf2; tf1 tf3] (x' = a x + c y, y' = b x + d y),
    // t = (translate_x, translate_y) (% of the border box) and O = transform-origin.
    Len      translate_x = Len::Px(0), translate_y = Len::Px(0);
    float    tf[4] = {1, 0, 0, 1};
    std::vector<TfFn> tfn;              // the function list tf/translate_* were built from (animations interpolate it)
    void     rebuild_transform();       // tfn -> tf + translate_x/y
    Len      origin_x = Len::Pct(50), origin_y = Len::Pct(50);
    bool     tf_linear() const { return tf[0] != 1 || tf[1] != 0 || tf[2] != 0 || tf[3] != 1; }
    std::vector<TransitionSpec> transitions;
    std::vector<AnimationSpec> animations;
    float    aspect_ratio = 0;          // aspect-ratio (width/height); 0 = auto
    // container queries: this element as a query container, and the size cq* units resolve against
    uint8_t  container_type = 0;        // 0 normal, 1 inline-size, 2 size
    std::string container_name;         // space-separated names
    float    cq_w = -1, cq_h = -1;      // nearest container's content box (-1: none, units use the viewport)
    // outline: drawn outside the border edge, never affects layout.  `auto` = the focus ring.
    BorderStyle outline_style = BorderStyle::None;
    bool     outline_auto = false;
    float    outline_w = 3, outline_offset = 0;
    uint32_t outline_color = 0xFF000000;
    bool     outline_current = true;
    std::map<std::string, std::string> custom;   // --x: raw value (inherited)

    // line-height in px for THIS element's font
    float line_px() const;
    bool  is_blockish() const;          // generates a block-level box
};

} // namespace luxium::style
