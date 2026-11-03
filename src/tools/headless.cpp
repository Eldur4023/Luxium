// luxium_headless -- the script engine's test harness: no window, no render.
//
//   luxium_headless <page.html | http://url> [step ...]
//
// Loads the page (parse + compile + run), plays the steps in order, then
// prints the console the way the shell's pane would.  Exit code 1 if a step
// failed (expect:), 2 on usage errors.
//
// Steps (a bare word is the legacy form: click the element with that id):
//   click:ID            click the element with that id (click event + default action)
//   dblclick:ID         dblclick event
//   type:ID=TEXT        set the value, fire input then change
//   set:ID=TEXT         set the value silently
//   key:KEY[@ID]        keydown + keyup (KEY is a DOM key: Enter, ArrowLeft, a, Space...)
//   wait:MS             run the engine loop for MS milliseconds
//   until:TEXT[:MS]     run the loop until a console line contains TEXT (default 5000 ms)
//   text:ID             print the element's text
//   attr:ID.NAME        print an attribute
//   count:SELECTOR      print how many elements match
//   expect:TEXT         exit 1 unless a console line contains TEXT
//   back                the page's own history back (popstate)
//   print:TEXT          print a marker line

#include "page/page.hpp"
#include "net/fetch.hpp"
#include "style/selector.hpp"

#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using luxium::dom::Node;

// LUXIUM_FAKE_MEDIA=1: a scripted <video> (10 s, 2 audio tracks, 2 subtitle
// tracks, a cue between 2 and 3 s) so the media API and its events are testable
// without FFmpeg or a window.
class FakeMedia : public luxium::MediaApi {
public:
    luxium::MediaState state(const Node* n) override {
        P& p = players_[n];
        const auto now = std::chrono::steady_clock::now();
        if (!p.paused && !p.ended) {
            p.time += std::chrono::duration<double>(now - p.at).count();
            if (p.time >= 10) { p.time = 10; p.ended = true; p.paused = true; }
        }
        p.at = now;
        luxium::MediaState s;
        s.known = true; s.paused = p.paused; s.ended = p.ended; s.time = p.time; s.duration = 10;
        s.seeking = p.seeking > 0;
        if (p.seeking > 0) --p.seeking;
        s.audio_count = 2; s.audio_cur = p.audio; s.audio_label = "track" + std::to_string(p.audio);
        s.sub_count = 2; s.sub_cur = p.sub; s.sub_label = "sub" + std::to_string(p.sub);
        if (p.sub > 0 && p.time >= 2 && p.time < 3) s.cue = "cue text";
        return s;
    }
    void play(const Node* n) override { P& p = players_[n]; if (p.ended) { p.time = 0; p.ended = false; } p.paused = false; p.at = std::chrono::steady_clock::now(); }
    void pause(const Node* n) override { players_[n].paused = true; }
    void seek(const Node* n, double t) override { P& p = players_[n]; p.time = t; p.ended = false; p.seeking = 2; }
    void cycle_audio(const Node* n) override { P& p = players_[n]; p.audio = p.audio % 2 + 1; }
    void cycle_subtitle(const Node* n) override { P& p = players_[n]; p.sub = (p.sub + 1) % 3; }
private:
    struct P { bool paused = true, ended = false; double time = 0; int audio = 1, sub = 0, seeking = 0;
               std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now(); };
    std::map<const Node*, P> players_;
};

static std::vector<Node*> select(luxium::page::Page& page, const std::string& sel) {
    std::vector<Node*> out;
    auto list = luxium::style::parse_selector_list(sel);
    std::vector<Node*> stack;
    if (page.page_root()) stack.push_back(page.page_root());
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->is_element())
            for (const auto& s : list) if (luxium::style::matches(s, *n)) { out.push_back(n); break; }
        for (size_t i = n->children.size(); i-- > 0;) stack.push_back(n->children[i].get());
    }
    return out;
}

static Node* by_id(luxium::page::Page& page, const std::string& id) {
    auto v = select(page, "#" + id);
    return v.empty() ? nullptr : v[0];
}

static void set_value(Node* n, const std::string& v) {
    // Same representation the engine reads: attributes (textarea: its text).
    if (n->is_element("textarea")) {
        n->children.clear();
        auto t = std::make_unique<Node>(luxium::dom::NodeType::Text);
        t->text = v;
        n->append(std::move(t));
        return;
    }
    for (auto& [k, val] : n->attrs) if (k == "value") { val = v; return; }
    n->attrs.emplace_back("value", v);
}

static bool console_has(luxium::page::Page& page, const std::string& needle) {
    for (const auto& l : page.console_lines())
        if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

static void spin(luxium::page::Page& page, int ms, const std::string& until = {}) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        page.tick();
        if (!until.empty() && console_has(page, until)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: luxium_headless <page.html|http://url> [step ...]\n");
        return 2;
    }
    luxium::page::Page page;
    FakeMedia fake_media;
    if (std::getenv("LUXIUM_FAKE_MEDIA")) page.set_media(&fake_media);
    const std::string target = argv[1];
    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
        auto f = luxium::net::Fetch::start(target);
        while (!f->done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!f->ok()) { std::fprintf(stderr, "cannot fetch '%s': %s\n", target.c_str(), f->error().c_str()); return 2; }
        page.load_string(f->body(), target);
    } else if (!page.load_file(target)) {
        std::fprintf(stderr, "cannot read '%s'\n", target.c_str());
        return 2;
    }
    spin(page, 1);   // coroutines that did not suspend have finished; let the first tick run

    int rc = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string step = argv[i];
        auto colon = step.find(':');
        const std::string cmd = colon == std::string::npos ? "click" : step.substr(0, colon);
        const std::string arg = colon == std::string::npos ? step : step.substr(colon + 1);
        if (step == "back") { std::printf("back -> %s\n", page.history_go(-1) ? "ok" : "no history"); spin(page, 5); continue; }
        if (cmd == "click" || cmd == "dblclick") {
            Node* n = by_id(page, arg);
            if (!n) { std::printf("no element #%s\n", arg.c_str()); rc = 1; continue; }
            if (cmd == "click") page.click(n); else page.dispatch("dblclick", n);
            spin(page, 5);
        } else if (cmd == "type" || cmd == "set") {
            auto eq = arg.find('=');
            Node* n = by_id(page, arg.substr(0, eq));
            if (!n || eq == std::string::npos) { std::printf("bad step %s\n", step.c_str()); rc = 1; continue; }
            set_value(n, arg.substr(eq + 1));
            if (cmd == "type") {
                page.dispatch("input", n);
                page.dispatch("change", n);
            }
            spin(page, 5);
        } else if (cmd == "key") {
            auto at = arg.find('@');
            Node* n = at == std::string::npos ? nullptr : by_id(page, arg.substr(at + 1));
            luxium::EventData d;
            d.key = arg.substr(0, at);
            d.code = d.key == " " ? "Space" : d.key;
            page.key_event(true, d, n);
            page.key_event(false, d, n);
            spin(page, 5);
        } else if (cmd == "wait") {
            spin(page, std::atoi(arg.c_str()));
        } else if (cmd == "until") {
            auto c2 = arg.rfind(':');
            int ms = 5000;
            std::string needle = arg;
            if (c2 != std::string::npos && arg.find_first_not_of("0123456789", c2 + 1) == std::string::npos) {
                ms = std::atoi(arg.c_str() + c2 + 1);
                needle = arg.substr(0, c2);
            }
            spin(page, ms, needle);
            if (!console_has(page, needle)) { std::printf("until: '%s' never appeared\n", needle.c_str()); rc = 1; }
        } else if (cmd == "text") {
            Node* n = by_id(page, arg);
            std::printf("text #%s = [%s]\n", arg.c_str(), n ? n->text_content().c_str() : "<missing>");
        } else if (cmd == "attr") {
            auto dot = arg.rfind('.');
            Node* n = by_id(page, arg.substr(0, dot));
            std::printf("attr %s = [%s]\n", arg.c_str(), n ? n->attr_or(arg.substr(dot + 1)).c_str() : "<missing>");
        } else if (cmd == "count") {
            std::printf("count %s = %zu\n", arg.c_str(), select(page, arg).size());
        } else if (cmd == "expect") {
            if (!console_has(page, arg)) { std::printf("expect FAILED: no console line contains '%s'\n", arg.c_str()); rc = 1; }
            else std::printf("expect ok: %s\n", arg.c_str());
        } else if (cmd == "print") {
            std::printf("%s\n", arg.c_str());
        } else {
            std::printf("unknown step '%s'\n", step.c_str());
            rc = 2;
        }
    }

    static const char* kLevel[] = {"info", "warn", "error"};
    for (const auto& l : page.console_lines())
        std::printf("[%s] %s\n", kLevel[l.level], l.text.c_str());
    std::printf("(%zu console lines, title \"%s\")\n", page.console_lines().size(),
                page.page_title().c_str());
    return rc;
}
