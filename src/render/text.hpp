#pragma once
// Text: FreeType glyphs shaped by HarfBuzz, rasterized once per (glyph,
// size) and cached as 8-bit alpha bitmaps.  The layout layer measures runs
// and asks for draws; nobody else touches FreeType.
//
// Shaping scope for the M-demo: one font family (regular + bold faces of
// DejaVu Sans), UTF-8 in, HarfBuzz clusters out -- enough for Spanish and
// the common Latin range, which is what the demo pages speak.

#include <cstdint>
#include <span>
#include <memory>
#include <string>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luxium::render {

struct Glyph {
    int bearing_x = 0, bearing_y = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> alpha;   // width*height, 8-bit coverage
    std::vector<uint8_t> lcd;     // height * width*3: per-subpixel (R,G,B) coverage, empty if none
    std::vector<uint8_t> color;   // colour glyphs (emoji bitmaps): premultiplied RGBA, width*height*4; `alpha` mirrors its alpha
};

struct ShapedRun {
    struct Pos { uint32_t glyph_index; float x, y; uint32_t cluster; float adv = 0, pen = 0; int face = -1; };   // face: fallback face id (-1 = the run's)   // adv: x_advance, pen: pen x before the offset
    std::vector<Pos> positions;
    float width = 0;
};

// An installed face chosen by Font::resolve (fontconfig); id indexes the font's face table.
struct FaceKey {
    int id = 0;
    bool operator==(const FaceKey& o) const { return id == o.id; }
};

struct FaceMetrics {         // Blink rounds ascent/descent to whole pixels
    float ascent = 0, descent = 0, line_gap = 0;
    float x_height = 0;
    float avg_char = 0, max_char = 0;   // OS/2 xAvgCharWidth and the bbox width: what Blink sizes text fields with
};

class Font {
public:
    // Regular and bold faces of the same family.  Returns null if either
    // file cannot be opened; the caller falls back to... there is no
    // fallback: a browser without its font is a build error, not a runtime
    // one (see the paths in shell.cpp).
    static std::unique_ptr<Font> load(const std::string& regular,
                                      const std::string& bold,
                                      const std::string& mono);
    // Loads the whole DejaVu family from `dir` (sans/serif/mono, each in
    // regular/bold/italic/bold-italic; missing variants fall back).
    static std::unique_ptr<Font> load_dejavu(const std::string& dir);
    ~Font();
    // The font the engine draws with (the last one created): SVG text takes its glyph outlines from it.
    static const Font* active();
    // A glyph's outline at `px` em as closed polylines in pixels, y up (FreeType's orientation).
    void glyph_outline(uint32_t glyph_index, FaceKey key, float px, std::vector<std::vector<std::pair<float, float>>>& out) const;

    // Picks the face for a CSS `font-family` list like Chromium on Linux does:
    // each family goes through fontconfig and is accepted only if it is the
    // requested family or a metric-compatible alias (Arial -> Liberation Sans);
    // generic keywords use Chromium's defaults (serif = Times New Roman,
    // sans-serif = Arial, monospace = the system fixed font).
    FaceKey resolve(const std::string& family_list, int weight, bool italic) const;

    // @font-face: `family` now also names this file's face (WOFF/WOFF2/TrueType/OpenType bytes, copied).  A
    // family with several faces is matched by weight and style like CSS font matching.  False if the bytes
    // are not a font.  `key` identifies the source (its URL): adding it twice is a no-op, see has_web_face.
    bool add_web_face(const std::string& family, int weight, bool italic, const std::string& bytes, const std::string& key);
    // @font-face { src: local(name) }: `family` also names an installed font.
    bool add_local_alias(const std::string& family, int weight, bool italic, const std::string& installed);
    bool has_web_face(const std::string& key) const { return web_keys_.count(key) != 0; }

    // Stateless-size API for the layout engine: shape at an explicit em.
    ShapedRun shape_at(std::string_view utf8, FaceKey key, float px) const;
    FaceMetrics metrics(FaceKey key, float px) const;
    // A face able to draw code point `cp`, styled like `base` (fontconfig); -1 if none.
    int fallback_for(uint32_t cp, FaceKey base) const;
    // The colour emoji face (Noto Color Emoji and the like); -1 if none is installed.
    int emoji_face() const;
    // `subx` (0..3): the glyph is drawn shifted right by subx/4 pixel (sub-pixel positioning)
    const Glyph& glyph_at(uint32_t glyph_index, FaceKey key, float px, int subx = 0) const;

    void set_size(float px);      // per-pixel em; cheap (FreeType set_pixel_sizes)
    float size() const { return size_; }
    float line_height() const;    // ascent + descent of the current size
    float ascent() const;         // baseline sits this far below the line top

    // UTF-8 -> positioned glyphs.  bold/mono select the face.
    ShapedRun shape(std::string_view utf8, bool bold, bool mono = false) const;

    // Real advance of a space at the current size (what Blink uses for word gaps).
    float space_width(bool bold, bool mono = false) const { return shape(" ", bold, mono).width; }

    // The cached bitmap for a shaped glyph at a given em.  The size is part
    // of the cache key: a glyph rasterized for the h1 (31px) reused by a
    // 15.5px run is exactly the "sometimes giant, sometimes glued" bug.
    const Glyph& glyph(uint32_t glyph_index, bool bold, bool mono, float size) const;

private:
    struct Face;
    std::unique_ptr<Face> regular_, bold_, mono_;
    std::unique_ptr<Face> var_[3][2][2];       // family, bold, italic (load_dejavu)
    mutable std::vector<std::unique_ptr<Face>> dyn_;                 // faces chosen by resolve()
    mutable std::unordered_map<std::string, int> resolve_cache_;     // "family|weight|italic" -> id
    struct WebFace { int weight; bool italic; int id; };
    std::unordered_map<std::string, std::vector<WebFace>> web_;      // lowercase @font-face family -> its faces
    std::unordered_set<std::string> web_keys_;                       // sources already added
    std::deque<std::string> web_bytes_;                              // the font files FreeType reads from
    int web_match(const std::string& family, int weight, bool italic) const;
    mutable std::unordered_map<std::string, int> file_ids_;          // "path#index#flags" -> id
    mutable std::unordered_map<int, std::vector<int>> chain_;        // face -> the later faces of the family list that resolved to it
    mutable std::unordered_map<uint64_t, int> fallback_cache_;       // (cp, base face) -> id
    int face_from_match(void* fc_pattern, int weight, bool italic) const;
    const Glyph& color_glyph(Face* f, uint32_t glyph_index, float px, uint64_t key) const;
    mutable int emoji_face_ = -2;                                    // -2: not looked up yet
    ShapedRun shape_face(Face* f, std::string_view utf8, float px) const;
    int match_family(const std::string& name, bool accept_any, int weight, bool italic) const;
    float size_ = 16;
    Face* face(bool bold, bool mono) const;
    Face* face(FaceKey key) const;
    void use_size(Face* f, float px) const;

    mutable std::unordered_map<uint64_t, Glyph> cache_;   // key: idx<<2 | bold<<1 | mono
    friend struct Face;
};

// A word-shaped run plus the metrics layout needs.  Layout works on words
// (wrapping happens between them); a run keeps its shaping so kerning inside
// a word is exact.
struct Word {
    ShapedRun shaped;
    float    width  = 0;
    bool     bold   = false;
    bool     mono   = false;
    float    size   = 0;     // the em this word was shaped at
    float    ascent = 0;     // baseline depth for THAT size
    std::string text;        // for the console pane / debugging
    bool     glued = false;  // no word gap after it (a hyphen break: "border-" | "box")
};



// Splits utf8 into words on ASCII spaces, shaping each.  Newlines never
// reach here: the layout layer handles explicit breaks first.
std::vector<Word> shape_words(const Font& font, std::string_view utf8, bool bold, bool mono = false);

// Draws a shaped word with the top-left of its line box at (x, y); y is the
// TOP of the line, glyphs are placed via ascent.  `argb` is 0xAARRGGBB.
void draw_word(std::span<uint32_t> fb, int fb_w, int fb_h,
               const Font& font, const Word& w, float x, float y, uint32_t argb);

} // namespace luxium::render
