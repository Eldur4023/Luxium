// Luxium's template surface: there are no templates in a browser.
//
// The emitter's render() path needs these symbols to link, and a page that
// calls render() must get a precise diagnostic at compile time -- not a link
// error, not a silent half-feature.  So every entry point fails with that
// message; escape_html stays real because it is useful glue, not server
// surface.

#include <lux_script/template.hpp>
#include <lux_script/natives.hpp>

namespace lux_script {

bool compile_template(const std::string&, const std::string& file,
                      const std::string&, const std::vector<TypedName>&,
                      DiagnosticBag& diags, Template&, const TemplateEnv&) {
    diags.error(SourceLoc{&file, 1, 1},
                "render() does not exist in Luxium: a page renders its own DOM");
    return false;
}

bool render_template(const Template&, std::vector<Value>, NativeCtx&,
                     const FunctionTable*, std::string& out, std::string& error) {
    error = "templates do not exist in Luxium";
    out.clear();
    return false;
}

void escape_html(const std::string& in, std::string& out) {
    out.reserve(out.size() + in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&#39;";  break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
}

} // namespace lux_script
