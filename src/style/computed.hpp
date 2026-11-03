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
    bool operator==(const Len& o) const { return px == o.px && pct == o.pct && is_auto == o.is_auto && fn == o.fn && px2 == o.px2 && pct2 == o.pct2 && px3 == o.px3 && pct3 == o.pct3; }
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
enum class ListStyle { Disc, Circle, Square, Decimal, LowerAlpha, UpperAlpha, LowerRoman, UpperRoman, None };
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

struct ComputedStyle {
    // ── display / positioning ───────────────────────────────────────────────
    Display  display = Display::Inline;
    Position position = Position::Static;
    Float    float_ = Float::None;
    Clear    clear = Clear::None;
    BoxSizing box_sizing = BoxSizing::Content;
    Overflow overflow_x = Overflow::Visible, overflow_y = Overflow::Visible;
    Visibility visibility = Visibility::Visible;
    float    opacity = 1.f;
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
    Len      inset[4] = {Len::Auto(), Len::Auto(), Len::Auto(), Len::Auto()};  // top right bottom left

    // ── painting ────────────────────────────────────────────────────────────
    uint32_t bg_color = kTransparent;
    std::vector<BgLayer> bg;
    std::vector<Shadow>  box_shadow;
    ObjectFit object_fit = ObjectFit::Fill;

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
    bool     list_inside = false;

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
    Len      row_gap, column_gap;      // normal == 0 here

    // ── grid ────────────────────────────────────────────────────────────────
    std::vector<Track> grid_template_columns, grid_template_rows;
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
    int      cursor = 0;                // 0 auto, 1 pointer, 2 text, 3 default, 4 not-allowed, 5 grab, 6 grabbing
    bool     pointer_events = true;
    uint32_t accent = 0;                // accent-color (0 = auto)
    bool     appearance_none = false;   // appearance: none (controls keep only their CSS look)
    float    aspect_ratio = 0;          // aspect-ratio (width/height); 0 = auto
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
