#include "layout/frame.hpp"

#include <page/luxp.hpp>
#include <paint/raster.hpp>
#include <render/layout.hpp>

#include <algorithm>
#include <list>
#include <map>

namespace luxium::layout {

namespace {

struct Cached { std::string key; std::shared_ptr<const render::ImageBitmap> bm; };
std::list<Cached> g_frames;   // most recent first; a few frames are enough for a page
constexpr size_t kMaxFrames = 16;
int g_depth = 0;              // a frame inside a frame inside...: stop before it loops on itself

bool looks_html(const std::string& s) {
    const size_t i = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    return i != std::string::npos && s[i] == '<';
}

} // namespace

std::shared_ptr<const render::ImageBitmap> render_frame(render::Font& font, const dom::Node& el,
                                                        const std::string& base_dir, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || g_depth >= 2) return nullptr;
    std::string html, base = base_dir;
    if (el.has_attr("srcdoc")) {
        html = el.attr_or("srcdoc");
    } else {
        const std::string src = el.attr_or("src");
        if (src.empty() || src.starts_with("javascript:") || src == "about:blank") return nullptr;
        const std::string path = render::resolve_image(base_dir, src);
        const int st = render::fetch_resource(path, html);
        if (st <= 0 || !looks_html(html)) return nullptr;
        base = path.substr(0, path.find_last_of('/') + 1);
    }
    const std::string key = std::to_string(w) + "x" + std::to_string(h) + "|" + base + "|" + html;
    for (auto it = g_frames.begin(); it != g_frames.end(); ++it)
        if (it->key == key) { g_frames.splice(g_frames.begin(), g_frames, it); return it->bm; }

    ++g_depth;
    page::ParsedPage pp = page::parse_page(html);
    page::StylesResult styles = page::translate_styles(*pp.document, [&](const std::string& url) -> std::optional<std::string> {
        std::string css;
        if (render::fetch_resource(render::resolve_image(base, url), css) > 0) return css;
        return std::nullopt;
    });
    render::LayoutResult lay = render::layout(*pp.document, font, float(w), styles.rules, base, 0.f, float(h));
    --g_depth;

    std::vector<uint32_t> px(size_t(w) * h, lay.canvas_bg | 0xFF000000u);
    paint::Canvas cv;
    cv.px = std::span<uint32_t>(px.data(), px.size());
    cv.w = w; cv.h = h; cv.clip = {0, 0, w, h};
    paint::rasterize(lay.list, cv, font);
    paint::rasterize(lay.fixed, cv, font);

    auto bm = std::make_shared<render::ImageBitmap>();
    bm->w = w; bm->h = h;
    bm->rgb.resize(size_t(w) * h * 3);
    for (size_t i = 0; i < px.size(); ++i) {
        bm->rgb[i * 3] = uint8_t(px[i] >> 16); bm->rgb[i * 3 + 1] = uint8_t(px[i] >> 8); bm->rgb[i * 3 + 2] = uint8_t(px[i]);
    }
    g_frames.push_front({key, bm});
    if (g_frames.size() > kMaxFrames) g_frames.pop_back();
    return bm;
}

} // namespace luxium::layout
