// Shell side of forms: SDL keyboard/mouse/IME events -> Page's form model, the <select> popup, the file
// chooser (zenity), the text cursor and caret blink.  Own-engine pages only (CEF handles its own input).

#include "shell/shell.hpp"

#include <layout/box.hpp>
#include <paint/raster.hpp>

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>

namespace luxium::shell {

using page::Page;

namespace {

int mods_of(uint16_t m) {
    return ((m & KMOD_SHIFT) ? 1 : 0) | ((m & KMOD_CTRL) ? 2 : 0) | ((m & KMOD_ALT) ? 4 : 0) | ((m & KMOD_GUI) ? 8 : 0);
}

// SDL keysym -> DOM KeyboardEvent.key
std::string dom_key(const SDL_Keysym& k) {
    switch (k.sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return "Enter";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_TAB: return "Tab";
    case SDLK_SPACE: return " ";
    case SDLK_LEFT: return "ArrowLeft";
    case SDLK_RIGHT: return "ArrowRight";
    case SDLK_UP: return "ArrowUp";
    case SDLK_DOWN: return "ArrowDown";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_DELETE: return "Delete";
    case SDLK_INSERT: return "Insert";
    default: break;
    }
    if (k.sym >= SDLK_F1 && k.sym <= SDLK_F12) return "F" + std::to_string(k.sym - SDLK_F1 + 1);
    if (k.sym >= 32 && k.sym < 127) {
        char c = char(k.sym);
        if ((k.mod & KMOD_SHIFT) && c >= 'a' && c <= 'z') c = char(c - 32);
        return std::string(1, c);
    }
    return SDL_GetKeyName(k.sym);
}

std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

// accept=".mp4,.mkv,image/png,image/*" -> zenity --file-filter pattern list
std::string accept_patterns(const std::string& accept) {
    std::string out, tok;
    auto flush = [&] {
        while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        if (tok.empty()) return;
        if (tok[0] == '.') out += " *" + tok;
        else if (tok == "image/*") out += " *.png *.jpg *.jpeg *.webp *.gif *.avif";
        else if (tok == "video/*") out += " *.mp4 *.mkv *.webm *.avi *.mov";
        else if (tok == "audio/*") out += " *.mp3 *.ogg *.flac *.wav *.m4a";
        else if (auto sl = tok.find('/'); sl != std::string::npos) out += " *." + (tok.substr(sl + 1) == "jpeg" ? std::string("jpg") : tok.substr(sl + 1));
        tok.clear();
    };
    for (char c : accept) { if (c == ',') flush(); else tok += c; }
    flush();
    return out;
}

} // namespace

struct Shell::FileJob {
    std::future<std::vector<std::string>> result;
    dom::Node* node = nullptr;
};

const render::HitBox* Shell::control_hit(const dom::Node* n) const {
    for (auto it = layout_.hits.rbegin(); it != layout_.hits.rend(); ++it)
        if (it->node == n && it->ctl >= 0) return &*it;
    return nullptr;
}

float Shell::control_text_width(const render::HitBox& h, const std::string& shown, int byte) const {
    if (shown.empty() || byte <= 0) return 0;
    std::string pre;
    if (h.password) {
        int n = 0;
        for (int i = 0; i < byte && i < int(shown.size()); ++i) if ((static_cast<unsigned char>(shown[i]) & 0xC0) != 0x80) ++n;
        for (int i = 0; i < n; ++i) pre += "\xE2\x80\xA2";
    } else pre = shown.substr(0, size_t(std::min<int>(byte, int(shown.size()))));
    return font_->shape_at(pre, render::FaceKey{h.face}, h.px).width;
}

int Shell::control_caret_at(const render::HitBox& h, float doc_x) const {
    const std::string v = page_.form_value(h.node);
    const float local = doc_x - h.tx + h.node->scroll_x;
    int best = 0;
    float prev = 0;
    for (int i = 0; i < int(v.size());) {
        int j = i + 1;
        while (j < int(v.size()) && (static_cast<unsigned char>(v[j]) & 0xC0) == 0x80) ++j;
        const float w = control_text_width(h, v, j);
        if (local < (prev + w) / 2) return i;
        best = j; prev = w; i = j;
    }
    return best;
}

// ── select popup ────────────────────────────────────────────────────────────

void Shell::open_select_popup(const render::HitBox& h) {
    dom::Node* sel = const_cast<dom::Node*>(h.node);
    if (sel->has_attr("disabled")) return;
    popup_ = Popup{};
    popup_.node = sel;
    popup_.px = h.px; popup_.face = h.face;
    popup_.item_h = std::max(18.f, std::round(h.px * 1.5f));
    std::vector<dom::Node*> st{sel};
    std::vector<dom::Node*> opts;
    while (!st.empty()) {   // options in document order
        dom::Node* n = st.back(); st.pop_back();
        if (n != sel && n->is_element() && n->name == "option") { opts.push_back(n); continue; }
        for (size_t i = n->children.size(); i-- > 0;) st.push_back(n->children[i].get());
    }
    for (auto* o : opts) {
        std::string t = o->has_attr("label") ? o->attr_or("label") : o->text_content();
        popup_.items.push_back(t);
    }
    popup_.hover = std::max(0, page_.select_index(sel));
    popup_.x = h.x; popup_.y = h.y + h.h; popup_.w = std::max(h.w, 60.f);
    for (const auto& t : popup_.items)
        popup_.w = std::max(popup_.w, font_->shape_at(t, render::FaceKey{h.face}, h.px).width + 24.f);
    need_paint_ = true;
}

void Shell::draw_select_popup() {
    if (!popup_.node) return;
    const bool dark = page_.dark_scheme();
    const uint32_t bg = dark ? 0xFF2B2B2B : 0xFFFFFFFF, fg = dark ? 0xFFFFFFFF : 0xFF000000, hi = dark ? 0xFF0B57D0 : 0xFF1A73E8,
                   edge = dark ? 0xFF6B6B6B : 0xFF9E9E9E;
    const float h = popup_.item_h * float(popup_.items.size()) + 2;
    const float top = float(url_h());
    float y = popup_.y - scroll_ + top;
    if (y + h > float(win_h_ - bottom_h()) && popup_.y - scroll_ - popup_.item_h * 0 > h) y = popup_.y - scroll_ + top - h - 0;   // flip above
    paint::DisplayList dl;
    auto rect = [&](float x0, float y0, float w0, float h0, uint32_t c) {
        paint::Item it; it.kind = paint::Item::Kind::Rect; it.x = x0; it.y = y0; it.w = w0; it.h = h0; it.color = c;
        dl.push_back(std::move(it));
    };
    rect(popup_.x, y, popup_.w, h, edge);
    rect(popup_.x + 1, y + 1, popup_.w - 2, h - 2, bg);
    const render::FaceKey face{popup_.face};
    const render::FaceMetrics m = font_->metrics(face, popup_.px);
    for (size_t i = 0; i < popup_.items.size(); ++i) {
        const float iy = y + 1 + float(i) * popup_.item_h;
        const bool on = int(i) == popup_.hover;
        if (on) rect(popup_.x + 1, iy, popup_.w - 2, popup_.item_h, hi);
        auto run = std::make_shared<render::ShapedRun>(font_->shape_at(popup_.items[i], face, popup_.px));
        paint::Item t; t.kind = paint::Item::Kind::Text; t.x = popup_.x + 8; t.baseline = iy + (popup_.item_h - (m.ascent + m.descent)) / 2 + m.ascent;
        t.run = run; t.face = face; t.px = popup_.px; t.color = on ? 0xFFFFFFFF : fg;
        dl.push_back(std::move(t));
    }
    paint::Canvas cv;
    cv.px = std::span<uint32_t>(fb_.data(), fb_.size());
    cv.w = win_w_; cv.h = win_h_;
    cv.clip = {0, url_h(), win_w_, win_h_ - bottom_h()};
    cv.ox = 0; cv.oy = 0;
    paint::rasterize(dl, cv, *font_);
}

bool Shell::popup_input_key(int sym) {
    const int n = int(popup_.items.size());
    switch (sym) {
    case SDLK_DOWN: popup_.hover = std::min(n - 1, popup_.hover + 1); break;
    case SDLK_UP: popup_.hover = std::max(0, popup_.hover - 1); break;
    case SDLK_HOME: popup_.hover = 0; break;
    case SDLK_END: popup_.hover = n - 1; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: {
        dom::Node* s = popup_.node;
        const int pick = popup_.hover;
        close_select_popup();
        page_.select_option(s, pick);
        break;
    }
    case SDLK_ESCAPE: case SDLK_TAB: close_select_popup(); break;
    default: return true;
    }
    need_paint_ = true;
    return true;
}

// ── file chooser ────────────────────────────────────────────────────────────

void Shell::open_file_dialog(dom::Node* input) {
    if (file_job_ || input->has_attr("disabled")) return;
    std::string cmd = std::getenv("LUXIUM_ZENITY") ? std::getenv("LUXIUM_ZENITY") : "zenity";
    cmd += " --file-selection --title=" + shell_quote(app_ ? "Elegir archivo" : "Choose file");
    if (input->has_attr("multiple")) cmd += " --multiple --separator=" + shell_quote("\n");
    const std::string pats = accept_patterns(input->attr_or("accept"));
    if (!pats.empty()) cmd += " --file-filter=" + shell_quote("Archivos |" + pats) + " --file-filter=" + shell_quote("Todos | *");
    cmd += " 2>/dev/null";
    auto job = std::make_shared<FileJob>();
    job->node = input;
    job->result = std::async(std::launch::async, [cmd] {
        std::vector<std::string> paths;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            std::string out;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
            pclose(p);
            size_t a = 0;
            while (a < out.size()) {
                size_t b = out.find('\n', a);
                if (b == std::string::npos) b = out.size();
                if (b > a) paths.push_back(out.substr(a, b - a));
                a = b + 1;
            }
        }
        return paths;
    });
    file_job_ = std::move(job);
}

// ── mouse ───────────────────────────────────────────────────────────────────

bool Shell::page_mouse_down(const SDL_MouseButtonEvent& b) {
    if (b.button != SDL_BUTTON_LEFT) return false;
    if (b.y < url_h() || b.y >= win_h_ - bottom_h()) return false;
    const float dx = float(b.x), dy = float(b.y - url_h()) + scroll_;
    if (popup_.node) {   // the popup owns the next click
        const float top = popup_.y;
        if (dx >= popup_.x && dx < popup_.x + popup_.w && dy >= top && dy < top + popup_.item_h * float(popup_.items.size()) + 2) {
            const int idx = std::clamp(int((dy - top - 1) / popup_.item_h), 0, int(popup_.items.size()) - 1);
            dom::Node* s = popup_.node;
            close_select_popup();
            page_.select_option(s, idx);
        } else close_select_popup();
        return true;
    }
    const dom::Node* under = nullptr;
    for (const auto& a : layout_.areas)
        if (dx >= a.x && dx < a.x + a.w && dy >= a.y && dy < a.y + a.h) under = a.node;
    for (auto it = layout_.hits.rbegin(); it != layout_.hits.rend(); ++it)   // a control wins over what is behind it
        if ((it->ctl >= 0 || it->node->name == "label") && dx >= it->x && dx < it->x + it->w && dy >= it->y && dy < it->y + it->h) { under = it->node; break; }
    if (!under) { page_.blur(); return false; }
    page_.activate(const_cast<dom::Node*>(under));
    dom::Node* f = page_.focused();
    if (f && Page::is_text_control(f)) {
        if (const render::HitBox* h = control_hit(f)) {
            const int pos = control_caret_at(*h, dx);
            if (b.clicks >= 3) page_.edit_select_all();
            else if (b.clicks == 2) { page_.edit_set_caret(pos, false); page_.edit_select_word(); }
            else page_.edit_set_caret(pos, (SDL_GetModState() & KMOD_SHIFT) != 0);
            form_drag_ = true;
        }
    } else if (f && f->name == "select") {
        if (const render::HitBox* h = control_hit(f)) open_select_popup(*h);
    } else if (f && f->name == "input" && f->attr_or("type") == "file") {
        open_file_dialog(f);
    }
    need_paint_ = true;
    return false;   // the click still reaches script handlers
}

void Shell::page_mouse_move(int x, int y, bool left_down) {
    const float dx = float(x), dy = float(y - url_h()) + scroll_;
    if (popup_.node) {
        const float top = popup_.y;
        if (dx >= popup_.x && dx < popup_.x + popup_.w && dy >= top && dy < top + popup_.item_h * float(popup_.items.size()) + 2) {
            const int idx = std::clamp(int((dy - top - 1) / popup_.item_h), 0, int(popup_.items.size()) - 1);
            if (idx != popup_.hover) { popup_.hover = idx; need_paint_ = true; }
        }
    }
    if (form_drag_ && left_down && page_.focused() && Page::is_text_control(page_.focused())) {
        if (const render::HitBox* h = control_hit(page_.focused()))
            page_.edit_set_caret(control_caret_at(*h, dx), true);
    }
}

// ── keyboard ────────────────────────────────────────────────────────────────

bool Shell::page_key_down(const SDL_KeyboardEvent& k) {
    dom::Node* f = page_.focused();
    const bool shift = (k.keysym.mod & KMOD_SHIFT) != 0, ctrl = (k.keysym.mod & KMOD_CTRL) != 0;
    // F-keys (devtools, reload...) and Ctrl+R/L/U stay with the shell
    if ((k.keysym.sym >= SDLK_F1 && k.keysym.sym <= SDLK_F12)) return false;
    {
        page::FormEvent ev{page::FormEvent::Kind::KeyDown, f ? f : page_.page_root()->find_first("body")};
        ev.key = dom_key(k.keysym); ev.mods = mods_of(k.keysym.mod);
        if (!page_.dispatch(ev)) return true;   // the script took it
    }
    if (popup_.node) return popup_input_key(k.keysym.sym);
    if (k.keysym.sym == SDLK_TAB) { page_.focus_next(shift); need_paint_ = true; return true; }
    if (!f) return false;
    const int sym = k.keysym.sym;
    if (Page::is_text_control(f)) {
        switch (sym) {
        case SDLK_LEFT: page_.edit_move(-1, ctrl, shift); return true;
        case SDLK_RIGHT: page_.edit_move(+1, ctrl, shift); return true;
        case SDLK_HOME: page_.edit_home_end(false, shift); return true;
        case SDLK_END: page_.edit_home_end(true, shift); return true;
        case SDLK_UP: if (f->name == "textarea") { page_.edit_home_end(false, shift); return true; } return false;
        case SDLK_DOWN: if (f->name == "textarea") { page_.edit_home_end(true, shift); return true; } return false;
        case SDLK_BACKSPACE: page_.edit_backspace(ctrl); return true;
        case SDLK_DELETE: page_.edit_delete(ctrl); return true;
        case SDLK_RETURN: case SDLK_KP_ENTER: page_.edit_enter(); return true;
        case SDLK_ESCAPE: page_.blur(); return true;
        case SDLK_a: if (ctrl) { page_.edit_select_all(); return true; } break;
        case SDLK_c: if (ctrl) { const std::string s = page_.edit_selection(); if (!s.empty()) SDL_SetClipboardText(s.c_str()); return true; } break;
        case SDLK_x: if (ctrl) { const std::string s = page_.edit_selection(); if (!s.empty()) { SDL_SetClipboardText(s.c_str()); page_.edit_replace_selection(""); } return true; } break;
        case SDLK_v: if (ctrl) { if (char* t = SDL_GetClipboardText()) { page_.edit_insert(t); SDL_free(t); } return true; } break;
        default: break;
        }
        return false;
    }
    if (f->name == "select") {
        const int cur = page_.select_index(f);
        if (sym == SDLK_DOWN && (k.keysym.mod & KMOD_ALT)) { if (const auto* h = control_hit(f)) open_select_popup(*h); return true; }
        if (sym == SDLK_DOWN) { page_.select_option(f, cur + 1); return true; }
        if (sym == SDLK_UP) { page_.select_option(f, cur - 1); return true; }
        if (sym == SDLK_SPACE || sym == SDLK_RETURN) { if (const auto* h = control_hit(f)) open_select_popup(*h); return true; }
        return false;
    }
    const bool check = f->name == "input" && (f->attr_or("type") == "checkbox" || f->attr_or("type") == "radio");
    if (check && sym == SDLK_SPACE) { page_.activate(f); page_.click(f); return true; }
    if (f->name == "input" && f->attr_or("type") == "file" && (sym == SDLK_SPACE || sym == SDLK_RETURN)) { open_file_dialog(f); return true; }
    if ((f->name == "button" || f->name == "a" || f->name == "summary" || (f->name == "input" && !check)) &&
        (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || (sym == SDLK_SPACE && f->name != "a"))) {
        page_.click(f);
        return true;
    }
    return false;
}

bool Shell::page_input(const SDL_Event& e) {
    if (engine_cef_ || url_focus_ || !page_.page_root()) return false;
    switch (e.type) {
    case SDL_TEXTINPUT:
        if (page_.focused() && Page::is_text_control(page_.focused())) {
            dom::g_ime = {};
            page_.edit_insert(e.text.text);
            return true;
        }
        return false;
    case SDL_TEXTEDITING:
        if (page_.focused() && Page::is_text_control(page_.focused())) {
            dom::g_ime.node = page_.focused();
            dom::g_ime.text = e.edit.text;
            page_.page_mutated();   // repaint with the composition shown at the caret
            return true;
        }
        return false;
    case SDL_KEYDOWN: return page_key_down(e.key);
    case SDL_KEYUP: {
        dom::Node* f = page_.focused();
        if (e.key.keysym.sym >= SDLK_F1 && e.key.keysym.sym <= SDLK_F12) return false;
        page::FormEvent ev{page::FormEvent::Kind::KeyUp, f ? f : page_.page_root()->find_first("body")};
        ev.key = dom_key(e.key.keysym); ev.mods = mods_of(e.key.keysym.mod);
        page_.dispatch(ev);
        return false;
    }
    case SDL_MOUSEBUTTONDOWN: return page_mouse_down(e.button);
    case SDL_MOUSEBUTTONUP: form_drag_ = false; return false;
    case SDL_MOUSEMOTION: page_mouse_move(e.motion.x, e.motion.y, (e.motion.state & SDL_BUTTON_LMASK) != 0); return false;
    default: return false;
    }
}

// ── once per frame ──────────────────────────────────────────────────────────

void Shell::form_frame() {
    if (engine_cef_ || !page_.page_root()) return;
    page_.drop_stale_focus();
    // the file chooser finished
    if (file_job_ && file_job_->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto paths = file_job_->result.get();
        dom::Node* n = file_job_->node;
        file_job_.reset();
        if (!paths.empty()) {
            page_.set_form_files(n, paths);
            page::FormEvent in{page::FormEvent::Kind::Input, n}; in.value = page_.form_value(n);
            page_.dispatch(in);
            page::FormEvent ch{page::FormEvent::Kind::Change, n}; ch.value = in.value;
            page_.dispatch(ch);
        }
    }
    dom::Node* f = page_.focused();
    const bool want = f && Page::is_text_control(f) && !url_focus_;
    SDL_Window* kf = SDL_GetKeyboardFocus();
    const bool ours = !kf || kf == window_;   // with several windows only the focused one drives the IME
    if (want && ours && !text_input_on_) { SDL_StartTextInput(); text_input_on_ = true; }
    else if ((!want || !ours) && text_input_on_) { SDL_StopTextInput(); text_input_on_ = false; }
    if (!want) return;
    const render::HitBox* h = control_hit(f);
    if (!h) return;
    // keep the caret inside the field
    const std::string v = page_.form_value(f);
    const float cw = h->w - (h->tx - h->x) * 2;
    const float cx = control_text_width(*h, v, f->caret);
    float sx = f->scroll_x;
    if (f->name != "textarea") {
        if (cx - sx > cw - 2) sx = cx - cw + 2;
        if (cx - sx < 0) sx = cx;
        const float total = control_text_width(*h, v, int(v.size()));
        sx = std::clamp(sx, 0.f, std::max(0.f, total - cw + 2));
    }
    if (sx != f->scroll_x) { f->scroll_x = sx; page_.page_mutated(); }
    // where the IME candidate window goes
    SDL_Rect r{int(h->tx + cx - sx), int(h->ty - scroll_) + url_h(), 1, int(h->th)};
    SDL_SetTextInputRect(&r);
    // blink: 530 ms like Chromium; every flip re-lays out, so only while a field has focus
    const uint32_t now = SDL_GetTicks();
    if (now - caret_tick_ > 530) {
        caret_tick_ = now;
        f->state ^= dom::Node::CaretOn;
        page_.page_mutated();
    }
}

// The pointer shape follows CSS `cursor`; `auto` means I-beam over text fields and a hand over links.
void Shell::update_cursor_shape() {
    if (engine_cef_) return;
    int kind = 0;
    if (mouse_y_ >= url_h() && mouse_y_ < win_h_ - bottom_h() && !url_focus_) {
        const float dx = float(mouse_x_), dy = float(mouse_y_ - url_h()) + scroll_;
        const render::HitBox* top = nullptr;
        for (const auto& a : layout_.areas)
            if (dx >= a.x && dx < a.x + a.w && dy >= a.y && dy < a.y + a.h) top = &a;
        if (top) {
            kind = top->cursor;
            if (kind == 0)
                for (const dom::Node* n = top->node; n; n = n->parent)
                    if (n->is_element() && n->name == "a" && n->has_attr("href")) { kind = 1; break; }
                    else if (Page::is_text_control(n) && !n->has_attr("disabled")) { kind = 2; break; }
        }
        if (popup_.node) kind = 3;
    }
    if (kind == cursor_kind_) return;
    cursor_kind_ = kind;
    if (!cursor_text_) {
        cursor_text_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
        cursor_no_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NO);
    }
    SDL_Cursor* c = kind == 1 || kind == 5 || kind == 6 ? cursor_hand_ : kind == 2 ? cursor_text_ : kind == 4 ? cursor_no_ : SDL_GetDefaultCursor();
    SDL_SetCursor(c ? c : SDL_GetDefaultCursor());
}

} // namespace luxium::shell
