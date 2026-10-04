// luxium_headless -- the script engine's test harness: no window, no render.
//
//   luxium_headless <page.html>
//
// Loads the page (parse + compile + run), prints the console the way the
// shell's pane would, and reports script errors.  Exit code 1 if a script
// failed to compile.

#include "page/page.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: luxium_headless <page.html>\n");
        return 2;
    }
    luxium::page::Page page;
    if (!page.load_file(argv[1])) {
        std::fprintf(stderr, "cannot read '%s'\n", argv[1]);
        return 2;
    }

    // Test hook: extra args are ids to click, after the page loaded.
    for (int i = 2; i < argc; ++i) page.click_by_id(argv[i]);
    if (argc > 2)
        std::printf("-- after clicks --\n");

    static const char* kLevel[] = {"info", "warn", "error"};
    for (const auto& l : page.console_lines())
        std::printf("[%s] %s\n", kLevel[l.level], l.text.c_str());
    std::printf("(%zu console lines, title \"%s\")\n", page.console_lines().size(),
                page.page_title().c_str());
    return 0;
}
