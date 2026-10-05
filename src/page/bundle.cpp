#include "page/bundle.hpp"

#include <page/doc_codec.hpp>
#include <page/luxp.hpp>
#include <script/bytecode_io.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace luxium::bundle {

namespace {

namespace fs = std::filesystem;


// Mounted bundles.  ponytail: global and never unmounted -- fine for one
// browser window; a tab model would own its mounts.
std::map<std::string, page::PageSections> g_pages;       // "dir/app.luxp/x.html"
std::map<std::string, std::string>  g_resources;   // "dir/app.luxp/img/a.png"
std::set<std::string>               g_prefixes;    // "dir/app.luxp/"

std::string norm(const std::string& p) {
    return fs::path(p).lexically_normal().generic_string();
}

std::optional<std::string> read_disk(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}




size_t count_nodes(const dom::Node& n) {
    size_t k = 1;
    for (const auto& c : n.children) k += count_nodes(*c);
    return k;
}

} // namespace

std::optional<std::string> read_file(const std::string& path) {
    for (const auto& prefix : g_prefixes)
        if (path.starts_with(prefix)) {
            auto it = g_resources.find(norm(path));
            if (it == g_resources.end()) return std::nullopt;
            return it->second;
        }
    return read_disk(path);
}

const page::PageSections* find_page(const std::string& path) {
    auto it = g_pages.find(norm(path));
    return it == g_pages.end() ? nullptr : &it->second;
}

std::optional<std::string> mount(const std::string& luxp_path, std::string* err) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return std::nullopt; };
    auto raw = read_disk(luxp_path);
    if (!raw) return fail("no se puede leer el archivo");
    page::Container c;
    std::string why;
    if (!page::decode_container(*raw, c, &why)) return fail(why);

    const std::string base = norm(luxp_path) + "/";
    auto key_of = [&](const std::string& name) -> std::optional<std::string> {
        const std::string k = norm(base + name);
        if (!k.starts_with(base)) return std::nullopt;   // a forged "../x" would land outside the bundle
        return k;
    };
    std::map<std::string, page::PageSections> pages;
    std::map<std::string, std::string> res;
    for (auto& [name, sections] : c.pages) {
        auto k = key_of(name);
        if (!k) return fail(".luxp con una ruta que sale del paquete");
        pages[*k] = std::move(sections);
    }
    for (auto& [name, bytes] : c.resources) {
        auto k = key_of(name);
        if (!k) return fail(".luxp con una ruta que sale del paquete");
        res[*k] = std::move(bytes);
    }
    const std::string entry = *key_of(c.pages[0].first);
    for (auto& [k, v] : pages) g_pages[k] = std::move(v);
    for (auto& [k, v] : res) g_resources[k] = std::move(v);
    g_prefixes.insert(base);
    return entry;
}

bool inspect(const std::string& luxp_path, std::string* report) {
    std::string err;
    auto entry = mount(luxp_path, &err);
    if (!entry) { *report = luxp_path + ": " + err + "\n"; return false; }
    const std::string prefix = norm(luxp_path) + "/";
    bool ok = true;
    for (const auto& [key, s] : g_pages) {
        if (!key.starts_with(prefix)) continue;
        *report += key.substr(prefix.size()) + (key == *entry ? "  (entrada)\n" : "\n");
        auto doc = page::decode_document(s.document, &err);
        if (!doc) { *report += "  RECHAZADO: " + err + "\n"; ok = false; continue; }
        std::vector<style::Rule> rules;
        if (!page::decode_sheet(s.sheet, rules, &err)) { *report += "  RECHAZADO: " + err + "\n"; ok = false; continue; }
        *report += "  documento: " + std::to_string(count_nodes(*doc)) + " nodos\n";
        *report += "  estilos:   " + std::to_string(rules.size()) + " reglas\n";
        if (s.program.empty()) { *report += "  programa:  ninguno\n"; continue; }
        auto prog = script::load_bytecode(s.program, &err);
        if (!prog) { *report += "  RECHAZADO: " + err + "\n"; ok = false; continue; }
        const auto caps = script::capabilities(*prog);
        size_t instrs = 0;
        for (const auto& f : prog->functions) instrs += f->code.size();
        *report += "  programa:  " + std::to_string(prog->functions.size()) + " funciones, " +
                   std::to_string(instrs) + " instrucciones (verificado)\n  nativas:  ";
        for (const auto& n : caps.natives) *report += " " + n;
        *report += "\n  metodos:  ";
        for (const auto& n : caps.methods) *report += " " + n;
        *report += "\n";
    }
    for (const auto& [key, bytes] : g_resources)
        if (key.starts_with(prefix))
            *report += "recurso " + key.substr(prefix.size()) + " (" + std::to_string(bytes.size()) + " bytes)\n";
    return ok;
}

} // namespace luxium::bundle
