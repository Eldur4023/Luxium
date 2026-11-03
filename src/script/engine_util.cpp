// json, template, url, math, time, clipboard, Form and File.
#include "script/engine.hpp"
#include "script/engine_internal.hpp"

#include <lux_script/template.hpp>

#include <sys/stat.h>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace luxium::script {

using lux_script::Template;

Value Engine::file_value(const std::string& path) {
    const long long id = net_->next_obj++;
    FileInfo fi;
    fi.path = path;
    const size_t slash = path.find_last_of('/');
    fi.name = slash == std::string::npos ? path : path.substr(slash + 1);
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) fi.size = st.st_size;
    const size_t dot = fi.name.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : fi.name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    fi.type = ext == "png" ? "image/png" : ext == "jpg" || ext == "jpeg" ? "image/jpeg"
            : ext == "webp" ? "image/webp" : ext == "avif" ? "image/avif" : ext == "gif" ? "image/gif"
            : ext == "srt" ? "application/x-subrip" : ext == "vtt" ? "text/vtt"
            : ext == "json" ? "application/json" : ext == "txt" ? "text/plain" : "application/octet-stream";
    net_->files[id] = std::move(fi);
    while (net_->files.size() > 512) net_->files.erase(net_->files.begin());
    return lux_script::make_object("File", id);
}

void Engine::register_util() {
    // json
    reg("json_parse", [](Args& a, std::string& e) -> Value {
        if (!a[0].is_str()) { e = "json.parse() expects a string"; return Value::null(); }
        Value v;
        if (!Value::parse_json(a[0].as_str(), v)) { e = "json.parse(): invalid JSON"; return Value::null(); }
        return v;
    });
    reg("json_stringify", [](Args& a, std::string&) -> Value { return Value::str(a[0].to_json_text()); });

    // url
    reg("url_encode", [](Args& a, std::string&) -> Value { return Value::str(percent_encode(a[0].to_string())); });
    reg("url_decode", [](Args& a, std::string&) -> Value { return Value::str(percent_decode(a[0].to_string(), false)); });
    reg("url_parse_query", [](Args& a, std::string&) -> Value {
        std::string q = a[0].to_string();
        if (!q.empty() && (q[0] == '?' || q[0] == '#')) q.erase(0, 1);
        Value::Dict d;
        size_t i = 0;
        while (!q.empty()) {
            size_t amp = q.find('&', i);
            std::string kv = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
            size_t eq = kv.find('=');
            std::string k = percent_decode(kv.substr(0, eq), true);
            if (!k.empty()) d.set(std::move(k), Value::str(eq == std::string::npos ? "" : percent_decode(kv.substr(eq + 1), true)));
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
        return Value::dict(std::move(d));
    });

    // math / time
    auto num = [](Args& a, size_t i, std::string& e, const char* what, double& out) {
        if (a.size() <= i || !a[i].is_num()) { e = std::string(what) + " expects numbers"; return false; }
        out = a[i].as_float();
        return true;
    };
    auto whole = [](double d) -> Value {
        return std::fabs(d) < 9e15 ? Value::integer(static_cast<long long>(d)) : Value::real(d);
    };
    reg("math_floor", [num, whole](Args& a, std::string& e) -> Value { double x; return num(a, 0, e, "math.floor()", x) ? whole(std::floor(x)) : Value::null(); });
    reg("math_ceil",  [num, whole](Args& a, std::string& e) -> Value { double x; return num(a, 0, e, "math.ceil()", x) ? whole(std::ceil(x)) : Value::null(); });
    reg("math_round", [num, whole](Args& a, std::string& e) -> Value { double x; return num(a, 0, e, "math.round()", x) ? whole(std::round(x)) : Value::null(); });
    reg("math_abs", [num](Args& a, std::string& e) -> Value {
        double x;
        if (!num(a, 0, e, "math.abs()", x)) return Value::null();
        return a[0].is_int() ? Value::integer(std::llabs(a[0].as_int())) : Value::real(std::fabs(x));
    });
    reg("math_min", [num](Args& a, std::string& e) -> Value {
        double x, y;
        if (!num(a, 0, e, "math.min()", x) || !num(a, 1, e, "math.min()", y)) return Value::null();
        return y < x ? a[1] : a[0];
    });
    reg("math_max", [num](Args& a, std::string& e) -> Value {
        double x, y;
        if (!num(a, 0, e, "math.max()", x) || !num(a, 1, e, "math.max()", y)) return Value::null();
        return y > x ? a[1] : a[0];
    });
    reg("math_fixed", [num](Args& a, std::string& e) -> Value {
        double x;
        if (!num(a, 0, e, "math.fixed()", x) || !a[1].is_int()) { e = "math.fixed(x, digits) expects a number and an int"; return Value::null(); }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(std::clamp<long long>(a[1].as_int(), 0, 20)), x);
        return Value::str(buf);
    });
    reg("time_now", [](Args&, std::string&) -> Value {
        return Value::integer(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    });

    // clipboard
    reg("clipboard_write", [this](Args& a, std::string&) -> Value {
        if (page_.hooks().clipboard_set) page_.hooks().clipboard_set(a[0].to_string());
        return Value::null();
    });
    reg("clipboard_read", [this](Args&, std::string&) -> Value {
        return Value::str(page_.hooks().clipboard_get ? page_.hooks().clipboard_get() : std::string());
    });

    // template.*
    reg("template_escape", [](Args& a, std::string&) -> Value {
        std::string out;
        lux_script::escape_html(a[0].to_string(), out);
        return Value::str(std::move(out));
    });
    reg("template_render", [this](Args& a, std::string& e) -> Value {
        if (!a[0].is_str()) { e = "template.render(src, data) expects the template as a string"; return Value::null(); }
        std::vector<lux_script::TypedName> keys;
        std::vector<Value> values;
        std::string sig = a[0].as_str();
        if (a.size() > 1) {
            if (!a[1].is_dict()) { e = "template.render(): the data must be a Dict"; return Value::null(); }
            for (const auto& [k, v] : a[1].as_dict()) {
                const char* t = v.is_str() ? "string" : v.is_int() ? "int" : v.is_float() ? "float"
                              : v.is_bool() ? "bool" : v.is_list() ? "List" : v.is_dict() ? "Dict" : "Json";
                keys.push_back({k, t});
                values.push_back(v);
                sig += '\x01' + k + ':' + t;
            }
        }
        std::shared_ptr<Template> tpl;
        if (auto it = tpl_cache_.find(sig); it != tpl_cache_.end()) {
            tpl = std::static_pointer_cast<Template>(it->second);
        } else {
            tpl = std::make_shared<Template>();
            lux_script::DiagnosticBag diags;
            lux_script::TemplateEnv env;
            if (compiled_) env.fns = &compiled_->fn_sigs;
            static const std::string file = "<template>";
            if (!lux_script::compile_template(a[0].as_str(), file, "", keys, diags, *tpl, env)) {
                e = "template.render(): ";
                for (const auto& d : diags.items()) { e += d.message; break; }
                return Value::null();
            }
            if (tpl_cache_.size() > 512) tpl_cache_.clear();
            tpl_cache_[sig] = tpl;
        }
        std::string out, err;
        lux_script::NativeCtx ctx = make_ctx();
        std::vector<Template> one;   // eval_template_expr's index space (unused by the fast paths)
        if (!lux_script::render_template(*tpl, std::move(values), ctx,
                                         compiled_ ? &compiled_->functions : nullptr, out, err)) {
            e = "template.render(): " + err;
            return Value::null();
        }
        return Value::str(std::move(out));
    });

    // Form / File
    reg("form_data", [this](Args& a, std::string& e) -> Value {
        const long long id = net_->next_obj++;
        FormData& f = net_->forms[id];
        while (net_->forms.size() > 128) net_->forms.erase(net_->forms.begin());
        if (!a.empty()) {
            std::string err;
            dom::Node* form = arg_node(a[0], err, "form_data()");
            if (!form) { e = err; return Value::null(); }
            // Every named control under the form, document order.
            std::vector<dom::Node*> stack{form};
            while (!stack.empty()) {
                dom::Node* n = stack.back();
                stack.pop_back();
                for (size_t i = n->children.size(); i-- > 0;) stack.push_back(n->children[i].get());
                if (n == form || !n->is_element() || !n->has_attr("name") || n->has_attr("disabled")) continue;
                const std::string name = n->attr_or("name"), type = n->attr_or("type", "text");
                if (n->is_element("input") && (type == "checkbox" || type == "radio") && !n->has_attr("checked")) continue;
                if (n->is_element("input") && type == "file") {
                    for (const auto& p : input_files(n)) {
                        const size_t s = p.find_last_of('/');
                        f.files.push_back({name, p, s == std::string::npos ? p : p.substr(s + 1)});
                    }
                    continue;
                }
                if (n->is_element("input") || n->is_element("textarea") || n->is_element("select"))
                    f.fields.emplace_back(name, control_value(n));
            }
        }
        return lux_script::make_object("Form", id);
    });
    reg_method("Form", "add", [this](long long h, Args& a, std::string& e) -> Value {
        auto it = net_->forms.find(h);
        if (it == net_->forms.end()) { e = "this Form no longer exists"; return Value::null(); }
        it->second.fields.emplace_back(a[0].to_string(), a[1].to_string());
        return Value::null();
    });
    reg_method("Form", "add_file", [this](long long h, Args& a, std::string& e) -> Value {
        auto it = net_->forms.find(h);
        std::string type;
        long long fh = 0;
        if (it == net_->forms.end()) { e = "this Form no longer exists"; return Value::null(); }
        if (!lux_script::host_object_of(a[1], type, fh) || type != "File" || !net_->files.count(fh)) {
            e = "add_file(name, file) expects a File (from Element.files())";
            return Value::null();
        }
        const FileInfo& fi = net_->files[fh];
        it->second.files.push_back({a[0].to_string(), fi.path, fi.name});
        return Value::null();
    });
    auto file = [this](long long h, std::string& e) -> FileInfo* {
        auto it = net_->files.find(h);
        if (it == net_->files.end()) { e = "this File no longer exists"; return nullptr; }
        return &it->second;
    };
    reg_method("File", "name", [file](long long h, Args&, std::string& e) -> Value { auto* f = file(h, e); return f ? Value::str(f->name) : Value::null(); });
    reg_method("File", "path", [file](long long h, Args&, std::string& e) -> Value { auto* f = file(h, e); return f ? Value::str(f->path) : Value::null(); });
    reg_method("File", "size", [file](long long h, Args&, std::string& e) -> Value { auto* f = file(h, e); return f ? Value::integer(f->size) : Value::null(); });
    reg_method("File", "content_type", [file](long long h, Args&, std::string& e) -> Value { auto* f = file(h, e); return f ? Value::str(f->type) : Value::null(); });
}

} // namespace luxium::script
