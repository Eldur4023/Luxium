// Renderer-side media bridge.  Everything here runs on the renderer's main
// (Blink) thread, the same one that runs page scripts, so the V8 callbacks
// need no locking of their own; the Players have their own threads inside.

#include "media_bridge.hpp"

#include "media/player.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace luxium::cef::bridge {
namespace {

const char kShim[] =
#include "shim_js.inc"
;

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// All the players of one frame's V8 context.
struct Ctx {
    std::map<int, std::unique_ptr<media::Player>> players;
    int next_id = 1;
    media::Player* get(int id) {
        auto it = players.find(id);
        return it == players.end() ? nullptr : it->second.get();
    }
};

std::map<std::string, std::unique_ptr<Ctx>>& contexts() {
    static std::map<std::string, std::unique_ptr<Ctx>> m;
    return m;
}

class MediaV8 : public CefV8Handler {
public:
    explicit MediaV8(Ctx* c) : c_(c) {}

    bool Execute(const CefString& name, CefRefPtr<CefV8Value>, const CefV8ValueList& a,
                 CefRefPtr<CefV8Value>& ret, CefString&) override {
        const std::string n = name;
        auto id = [&]() { return a.empty() ? 0 : a[0]->GetIntValue(); };
        auto num = [&](size_t i) { return a.size() > i ? a[i]->GetDoubleValue() : 0.0; };
        auto flag = [&](size_t i) { return a.size() > i && a[i]->GetBoolValue(); };
        if (n == "canPlay") {
            ret = CefV8Value::CreateString(media::can_play(a.empty() ? "" : a[0]->GetStringValue().ToString()));
            return true;
        }
        if (n == "create") {
            const int i = c_->next_id++;
            c_->players[i] = std::make_unique<media::Player>();
            ret = CefV8Value::CreateInt(i);
            return true;
        }
        if (n == "poll") { ret = CefV8Value::CreateString(poll()); return true; }
        media::Player* p = c_->get(id());
        if (!p) { ret = CefV8Value::CreateNull(); return true; }
        if (n == "open") {
            p->open(a[1]->GetStringValue(), a[2]->GetStringValue(), flag(3));
        } else if (n == "close") {
            c_->players.erase(id());
        } else if (n == "play") { p->play();
        } else if (n == "pause") { p->pause();
        } else if (n == "seek") { p->seek(num(1));
        } else if (n == "setVolume") { p->set_volume(num(1));
        } else if (n == "setMuted") { p->set_muted(flag(1));
        } else if (n == "setRate") { p->set_rate(num(1));
        } else if (n == "setLoop") { p->set_loop(flag(1));
        } else if (n == "time") {
            ret = CefV8Value::CreateDouble(p->current_time());
        } else if (n == "state") {
            ret = CefV8Value::CreateString(state(*p));
        } else if (n == "ranges") {
            const auto r = num(1) != 0 ? p->seekable() : p->buffered();
            ret = CefV8Value::CreateArray((int)r.size());
            for (size_t i = 0; i < r.size(); ++i) {
                auto pair = CefV8Value::CreateArray(2);
                pair->SetValue(0, CefV8Value::CreateDouble(r[i].start));
                pair->SetValue(1, CefV8Value::CreateDouble(r[i].end));
                ret->SetValue((int)i, pair);
            }
        } else if (n == "frame") {
            frame(*p, a, ret);
        } else {
            return false;
        }
        return true;
    }

private:
    static std::string state(media::Player& p) {
        char b[256];
        const double d = p.duration();
        std::string dur = std::isnan(d) ? "" : std::isinf(d) ? ",\"d\":null" : (std::snprintf(b, sizeof b, ",\"d\":%.6f", d), std::string(b));
        std::snprintf(b, sizeof b, "{\"p\":%s,\"e\":%s,\"s\":%s,\"r\":%d,\"n\":%d,\"c\":%d,\"w\":%d,\"h\":%d,\"v\":%s",
                      p.paused() ? "true" : "false", p.ended() ? "true" : "false", p.seeking() ? "true" : "false",
                      p.ready_state(), p.network_state(), p.error_code(), p.video_width(), p.video_height(),
                      p.has_video() ? "true" : "false");
        return std::string(b) + dur + ",\"m\":\"" + json_escape(p.error_message()) + "\"}";
    }

    std::string poll() {
        std::string out;
        for (auto& [id, p] : c_->players) {
            for (auto& e : p->poll()) {
                out += out.empty() ? "[" : ",";
                out += "[" + std::to_string(id) + ",\"" + e.type + "\",\"" + json_escape(e.detail) + "\"]";
            }
        }
        if (!out.empty()) out += "]";
        return out;
    }

    // frame(id, afterSeq, buffer) -> null | ["grow", bytes] | [seq,w,h,dw,pts,matrix,full,bytes]
    static void frame(media::Player& p, const CefV8ValueList& a, CefRefPtr<CefV8Value>& ret) {
        auto f = p.frame((uint64_t)(a.size() > 1 ? a[1]->GetDoubleValue() : 0));
        if (!f) { ret = CefV8Value::CreateNull(); return; }
        const size_t bytes = f->data.size();
        if (a.size() < 3 || !a[2]->IsArrayBuffer() || a[2]->GetArrayBufferByteLength() < bytes) {
            ret = CefV8Value::CreateArray(2);
            ret->SetValue(0, CefV8Value::CreateString("grow"));
            ret->SetValue(1, CefV8Value::CreateDouble((double)bytes));
            return;
        }
        std::memcpy(a[2]->GetArrayBufferData(), f->data.data(), bytes);
        ret = CefV8Value::CreateArray(8);
        ret->SetValue(0, CefV8Value::CreateDouble((double)f->seq));
        ret->SetValue(1, CefV8Value::CreateInt(f->w));
        ret->SetValue(2, CefV8Value::CreateInt(f->h));
        ret->SetValue(3, CefV8Value::CreateInt(f->display_w));
        ret->SetValue(4, CefV8Value::CreateDouble(f->pts));
        ret->SetValue(5, CefV8Value::CreateInt(f->matrix));
        ret->SetValue(6, CefV8Value::CreateInt(f->full_range ? 1 : 0));
        ret->SetValue(7, CefV8Value::CreateDouble((double)bytes));
    }

    Ctx* c_;
    IMPLEMENT_REFCOUNTING(MediaV8);
};

bool wants_shim(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 ||
           url.rfind("file://", 0) == 0 || url.rfind("data:", 0) == 0 || url == "about:blank" || url.empty();
}

} // namespace

void on_context_created(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> ctx) {
    if (!wants_shim(frame->GetURL().ToString())) return;
    auto c = std::make_unique<Ctx>();
    CefRefPtr<MediaV8> h = new MediaV8(c.get());
    CefRefPtr<CefV8Value> obj = CefV8Value::CreateObject(nullptr, nullptr);
    for (const char* n : {"canPlay", "create", "open", "close", "play", "pause", "seek", "setVolume", "setMuted",
                          "setRate", "setLoop", "time", "state", "ranges", "poll", "frame"})
        obj->SetValue(n, CefV8Value::CreateFunction(n, h), V8_PROPERTY_ATTRIBUTE_NONE);
    ctx->GetGlobal()->SetValue("__luxiumMedia", obj, V8_PROPERTY_ATTRIBUTE_NONE);
    contexts()[frame->GetIdentifier().ToString()] = std::move(c);
    CefRefPtr<CefV8Value> rv;
    CefRefPtr<CefV8Exception> ex;
    if (!ctx->Eval(kShim, "luxium://media-shim.js", 1, rv, ex) && ex)
        std::fprintf(stderr, "[media-shim] %s:%d %s\n", ex->GetScriptResourceName().ToString().c_str(),
                     ex->GetLineNumber(), ex->GetMessage().ToString().c_str());
}

void on_context_released(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context>) {
    contexts().erase(frame->GetIdentifier().ToString());   // stops every player of the frame
}

} // namespace luxium::cef::bridge
