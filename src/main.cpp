// luxium -- the browser.
//
//   luxium <page.html>                 the window
//   luxium --check <dir|pages...>      lint: load every page, report errors
//   luxium <page.html> --headless      console only, no window
//   luxium --inspect <app.luxp>        verify its bytecode, list what it can do
//   luxium --dump <app.luxp>           print the entry page's tree and rules
//   luxium <page.html> --click id --shot out.ppm
//                                      simulate clicks, dump one frame
//
// Loads a local page, runs its LuxScript, and shows it: HTML + text layout,
// a console pane for log output, click handlers and timers live.

#include <page/bundle.hpp>
#include <page/doc_codec.hpp>
#include <page/page.hpp>
#include <shell/shell.hpp>
#include <style/cascade.hpp>
#include <style/dump.hpp>
#ifdef LUXIUM_CEF
#include <cef/cef_backend.hpp>
#include <host/app_host.hpp>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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
#ifdef LUXIUM_CEF
    // CEF relanza este mismo binario como subproceso (renderer, GPU,
    // utility...): si esta invocacion es una de esas, CEF la atiende aqui
    // dentro y NUNCA se llega al parser de argumentos.
    if (const int cef_code = luxium::cef::early(argc, argv); cef_code >= 0)
        return cef_code;
#endif

#ifdef LUXIUM_CEF
    if (argc >= 2 && std::strcmp(argv[1], "--host") == 0)   // app host: one browser, one window per client app
        return luxium::host::run();
#endif

    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) {   // the entry page's tree and rule count
        std::string err;
        auto entry = luxium::bundle::mount(argv[2], &err);
        const auto* sections = entry ? luxium::bundle::find_page(*entry) : nullptr;
        if (!sections) { std::fprintf(stderr, "luxium --dump: %s\n", entry ? "no entry page" : err.c_str()); return 1; }
        auto doc = luxium::page::decode_document(sections->document, &err);
        std::vector<luxium::style::Rule> rules;
        if (!doc || !luxium::page::decode_sheet(sections->sheet, rules, &err)) { std::fprintf(stderr, "luxium --dump: %s\n", err.c_str()); return 1; }
        doc->print();
        std::printf("-- %zu rules\n", rules.size());
        for (const auto& r : rules)
            for (const auto& d : r.decls) std::printf("%s:%s%s\n", d.name.c_str(), d.value.c_str(), d.important ? "!" : "");
        return 0;
    }

    if (argc == 3 && std::strcmp(argv[1], "--inspect") == 0) {
        std::string report;
        const bool ok = luxium::bundle::inspect(argv[2], &report);
        std::fputs(report.c_str(), stdout);
        return ok ? 0 : 1;
    }

    std::vector<std::string> paths;
    const char* shot = nullptr;
    const char* cefshot = nullptr;
    const char* boxes = nullptr;
    const char* styles = nullptr;
    int win_w = 1180, win_h = 820;
    const char* cefprobe = nullptr;
    std::vector<std::string> clicks;
    struct At { int x, y; unsigned ms; };
    std::vector<At> click_at;
    unsigned wait_ms = 0;
    bool check = false, headless = false, log_events = false;
    const char* script = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--headless") == 0) headless = true;
        else if (std::strcmp(a, "--check") == 0) check = true;
        else if (std::strcmp(a, "--click") == 0 && i + 1 < argc) clicks.push_back(argv[++i]);
        else if (std::strcmp(a, "--click-at") == 0 && i + 1 < argc) {
            int x = 0, y = 0, at = 0;
            if (std::sscanf(argv[++i], "%d,%d@%d", &x, &y, &at) >= 2) click_at.push_back({x, y, static_cast<unsigned>(at)});
        }
        else if (std::strcmp(a, "--script") == 0 && i + 1 < argc) script = argv[++i];
        else if (std::strcmp(a, "--log-events") == 0) log_events = true;
        else if (std::strcmp(a, "--wait") == 0 && i + 1 < argc) wait_ms = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (std::strcmp(a, "--cefshot") == 0 && i + 1 < argc) cefshot = argv[++i];
        else if (std::strcmp(a, "--boxes") == 0 && i + 1 < argc) boxes = argv[++i];
        else if (std::strcmp(a, "--styles") == 0 && i + 1 < argc) styles = argv[++i];
        else if (std::strcmp(a, "--size") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) { win_w = 1180; win_h = 820; }
        }
        else if (std::strcmp(a, "--cefprobe") == 0 && i + 1 < argc) cefprobe = argv[++i];
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::fprintf(stderr,
                "uso: luxium <page.html|dir> [--check] [--headless] "
                "[--click id] [--click-at x,y[@ms]] [--wait ms] [--size WxH] [--shot out.ppm [--boxes out.json]] [--cefshot out.ppm [--cefprobe out.json]]\n");
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

    if (styles) {   // computed styles of every [data-t] element, no window
        luxium::style::Resolver res;
        res.set_viewport(static_cast<float>(win_w), static_cast<float>(win_h - 58));
        res.set_author_rules(&page.rules());
        std::ofstream(styles) << luxium::style::dump_styles_json(*page.page_root(), res);
        return 0;
    }

    if (headless) {
        static const char* kLevel[] = {"info", "warn", "error"};
        for (const auto& l : page.console_lines())
            std::printf("[%s] %s\n", kLevel[l.level], l.text.c_str());
        return 0;
    }

    std::string title = "Luxium";
    if (!page.page_title().empty()) title += " \xE2\x80\x94 " + page.page_title();
    luxium::shell::Shell shell(page, {win_w, win_h,
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                      title});
    // http(s):// goes through the navigation flow (fetch on a worker thread);
    // sin argumentos, la pantalla de inicio; el resto, carga directa.
    if (is_url) shell.navigate_to(path, true);
    else if (path == "luxium:inicio") shell.navigate_address(path, true);
    if (log_events)   // prints every event the page dispatches: what scripts would receive
        page.event_tap = [](const std::string& kind, const luxium::dom::Node* t, const luxium::EventData& d) {
            if (kind == "mousemove") return;
            std::printf("event %s <%s id=%s> key=\"%s\" value=\"%s\"\n", kind.c_str(), t ? t->name.c_str() : "?",
                        t ? t->attr_or("id").c_str() : "", d.key.c_str(), d.value.c_str());
        };
    for (const auto& id : clicks) page.click_by_id(id);
    for (const auto& c : click_at) shell.add_shot_click(c.x, c.y, c.ms);
    shell.set_shot_wait(wait_ms);
    if (script && !shell.load_shot_script(script)) { std::fprintf(stderr, "luxium: cannot read script '%s'\n", script); return 1; }
    if (boxes) shell.set_boxes_out(boxes);
    if (cefprobe) shell.set_probe_out(cefprobe);
    if (cefshot) return shell.cef_shot(path, cefshot);
    if (shot) return shell.screenshot(shot);   // SDL dummy driver, one frame
    return shell.run();
}
