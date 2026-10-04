// luxium -- the browser.
//
//   luxium <page.html>
//
// Loads a local page, runs its LuxScript, and shows it: HTML + text layout,
// a console pane for log output, click handlers live.  `--headless` runs the
// engine without a window (same as the luxium_headless harness).

#include <page/page.hpp>
#include <shell/shell.hpp>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* shot = nullptr;
    std::vector<std::string> clicks;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (std::strcmp(argv[i], "--click") == 0 && i + 1 < argc) clicks.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stderr, "uso: luxium <page.html> [--headless] [--shot out.ppm]\n");
            return 0;
        } else if (!path) path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr, "uso: luxium <page.html>\n");
        return 2;
    }

    luxium::page::Page page;
    if (!page.load_file(path)) {
        std::fprintf(stderr, "luxium: cannot read '%s'\n", path);
        return 1;
    }

    if (headless) {
        static const char* kLevel[] = {"info", "warn", "error"};
        for (const auto& l : page.console_lines())
            std::printf("[%s] %s\n", kLevel[l.level], l.text.c_str());
        return 0;
    }

    luxium::shell::Shell shell(page, {1100, 780, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                      "Luxium \xE2\x80\x94 " + std::string(path)});
    for (const auto& id : clicks) page.click_by_id(id);
    if (shot) return shell.screenshot(shot);   // SDL dummy driver, one frame
    return shell.run();
}
