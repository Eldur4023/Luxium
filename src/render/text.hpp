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
#include <unordered_map>
#include <vector>

namespace luxium::render {

struct Glyph {
    int bearing_x = 0, bearing_y = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> alpha;   // width*height, 8-bit coverage
};

struct ShapedRun {
    struct Pos { uint32_t glyph_index; float x, y; uint32_t cluster; };
    std::vector<Pos> positions;
    float width = 0;
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
    ~Font();

    void set_size(float px);      // per-pixel em; cheap (FreeType set_pixel_sizes)
    float size() const { return size_; }
    float line_height() const;    // ascent + descent of the current size
    float ascent() const;         // baseline sits this far below the line top

    // UTF-8 -> positioned glyphs.  bold/mono select the face.
    ShapedRun shape(std::string_view utf8, bool bold, bool mono = false) const;

    // The cached bitmap for a shaped glyph index at the current size.
    const Glyph& glyph(uint32_t glyph_index, bool bold, bool mono = false) const;

private:
    struct Face;
    std::unique_ptr<Face> regular_, bold_, mono_;
    float size_ = 16;
    Face* face(bool bold, bool mono) const;

    mutable std::unordered_map<uint64_t, Glyph> cache_;   // key: idx<<2 | bold<<1 | mono
    friend struct Face;
};

// A word-shaped run plus the metrics layout needs.  Layout works on words
// (wrapping happens between them); a run keeps its shaping so kerning inside
// a word is exact.
struct Word {
    ShapedRun shaped;
    float    width = 0;
    bool     bold  = false;
    bool     mono  = false;
    std::string text;         // for the console pane / debugging
};

// Splits utf8 into words on ASCII spaces, shaping each.  Newlines never
// reach here: the layout layer handles explicit breaks first.
std::vector<Word> shape_words(const Font& font, std::string_view utf8, bool bold, bool mono = false);

// Draws a shaped word with the top-left of its line box at (x, y); y is the
// TOP of the line, glyphs are placed via ascent.  `argb` is 0xAARRGGBB.
void draw_word(std::span<uint32_t> fb, int fb_w, int fb_h,
               const Font& font, const Word& w, float x, float y, uint32_t argb);

} // namespace luxium::render
