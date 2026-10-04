// luxium -- the browser.
//
//   luxium <page.html>                 the window
//   luxium --check <dir|pages...>      lint: load every page, report errors
//   luxium <page.html> --headless      console only, no window
//   luxium <page.html> --click id --shot out.ppm
//                                      simulate clicks, dump one frame
//
// Loads a local page, runs its LuxScript, and shows it: HTML + text layout,
// a console pane for log output, click handlers and timers live.

#include <page/page.hpp>
#include <shell/shell.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int run_check(const std::vector<std::string>& paths) {
    std::vector<std::string> files;
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(p))
                if (e.path().extension() == ".html" || e.path().extension() == ".htm")
                    files.push_back(e.path().string());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    int bad = 0;
    for (const auto& f : files) {
        luxium::page::Page pg;
        if (!pg.load_file(f)) {
            std::printf("%-42s cannot read\n", f.c_str());
            ++bad;
            continue;
        }
        int errs = 0, warns = 0;
        for (const auto& l : pg.console_lines()) {
            if (l.level == 2) ++errs;
            if (l.level == 1) ++warns;
        }
        std::printf("%-42s %s\n", f.c_str(),
                    errs ? "ERRORES" : "ok");
        if (errs || warns)
            for (const auto& l : pg.console_lines())
                if (l.level >= (errs ? 2 : 1))
                    std::printf("    [%s] %s\n", l.level == 2 ? "error" : "warn",
                                l.text.c_str());
        bad += errs;
    }
    std::printf("%d error(es) en %zu pagina(s)\n", bad, files.size());
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> paths;
    const char* shot = nullptr;
    std::vector<std::string> clicks;
    bool check = false, headless = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--headless") == 0) headless = true;
        else if (std::strcmp(a, "--check") == 0) check = true;
        else if (std::strcmp(a, "--click") == 0 && i + 1 < argc) clicks.push_back(argv[++i]);
        else if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::fprintf(stderr,
                "uso: luxium <page.html|dir> [--check] [--headless] "
                "[--click id] [--shot out.ppm]\n");
            return 0;
        } else paths.push_back(a);
    }
    if (check && paths.empty()) {
        std::fprintf(stderr, "uso: luxium --check <dir|pages...>\n");
        return 2;
    }

    if (check) return run_check(paths);

    const std::string path = paths.empty() ? "luxium:inicio" : paths[0];
    const bool is_url = path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
    luxium::page::Page page;
    if (!is_url && path != "luxium:inicio" && !page.load_file(path)) {
        std::fprintf(stderr, "luxium: cannot read '%s'\n", path.c_str());
        return 1;
    }

    if (headless) {
        static const char* kLevel[] = {"info", "warn", "error"};
        for (const auto& l : page.console_lines())
            std::printf("[%s] %s\n", kLevel[l.level], l.text.c_str());
        return 0;
    }

    std::string title = "Luxium";
    if (!page.page_title().empty()) title += " \xE2\x80\x94 " + page.page_title();
    luxium::shell::Shell shell(page, {1180, 820,
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                      title});
    // http(s):// goes through the navigation flow (fetch on a worker thread);
    // sin argumentos, la pantalla de inicio; el resto, carga directa.
    if (is_url) shell.navigate_to(path, true);
    else if (path == "luxium:inicio") shell.navigate_address(path, true);
    for (const auto& id : clicks) page.click_by_id(id);
    if (shot) return shell.screenshot(shot);   // SDL dummy driver, one frame
    return shell.run();
}
