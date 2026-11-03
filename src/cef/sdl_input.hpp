#pragma once
// SDL -> Chromium input translation, shared by the classic shell and the app
// host (src/host).  Header-only on purpose: pure functions.
#include <SDL.h>
#include <cstdint>

namespace luxium::cef_input {

// ── SDL -> Chromium key translation ─────────────────────────────────────────
// EVENTFLAG bits: 1 shift, 2 ctrl, 4 alt, 8 cmd, 16 caps, 32 num, 1024 repeat.
inline uint32_t cef_mods_flags(int sdl_mod, bool repeat) {
    uint32_t m = 0;
    if (sdl_mod & KMOD_SHIFT) m |= 1;
    if (sdl_mod & KMOD_CTRL)  m |= 2;
    if (sdl_mod & KMOD_ALT)   m |= 4;
    if (sdl_mod & KMOD_GUI)   m |= 8;
    if (sdl_mod & KMOD_CAPS)  m |= 16;
    if (sdl_mod & KMOD_NUM)   m |= 32;
    if (repeat)               m |= 1024;
    return m;
}

// Windows virtual key de las teclas que reenviamos.
inline uint32_t cef_vk(SDL_Keycode s) {
    if (s >= 'a' && s <= 'z') return (uint32_t)(s - 32);
    if (s >= '0' && s <= '9') return (uint32_t)s;
    switch (s) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB:       return 0x09;
    case SDLK_ESCAPE:    return 0x1B;
    case SDLK_SPACE:     return 0x20;
    case SDLK_LEFT:      return 0x25;
    case SDLK_UP:        return 0x26;
    case SDLK_RIGHT:     return 0x27;
    case SDLK_DOWN:      return 0x28;
    case SDLK_PAGEUP:    return 0x21;
    case SDLK_PAGEDOWN:  return 0x22;
    case SDLK_HOME:      return 0x24;
    case SDLK_END:       return 0x23;
    case SDLK_INSERT:    return 0x2D;
    case SDLK_DELETE:    return 0x2E;
    case SDLK_LSHIFT:    return 0xA0;
    case SDLK_RSHIFT:    return 0xA1;
    case SDLK_LCTRL:     return 0xA2;
    case SDLK_RCTRL:     return 0xA3;
    case SDLK_LALT:      return 0xA4;
    case SDLK_RALT:      return 0xA5;
    default: break;
    }
    if (s >= SDLK_F1 && s <= SDLK_F12) return 0x70 + (s - SDLK_F1);
    if (s >= 32 && s < 127) return (uint32_t)s;   // resto de ASCII
    return 0;
}

// Keysym X11 equivalente (los keycodes de SDL son ASCII/XK para lo basico).
inline uint32_t cef_x_keysym(SDL_Keycode s) {
    if (s >= 'a' && s <= 'z') return (uint32_t)s;
    if (s >= 'A' && s <= 'Z') return (uint32_t)(s + 32);
    if (s >= '0' && s <= '9') return (uint32_t)s;
    if (s >= 32 && s < 127)   return (uint32_t)s;
    switch (s) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0xFF0D;
    case SDLK_BACKSPACE: return 0xFF08;
    case SDLK_TAB:       return 0xFF09;
    case SDLK_ESCAPE:    return 0xFF1B;
    case SDLK_SPACE:     return 0x0020;
    case SDLK_LEFT:      return 0xFF51;
    case SDLK_UP:        return 0xFF52;
    case SDLK_RIGHT:     return 0xFF53;
    case SDLK_DOWN:      return 0xFF54;
    case SDLK_PAGEUP:    return 0xFF55;
    case SDLK_PAGEDOWN:  return 0xFF56;
    case SDLK_HOME:      return 0xFF50;
    case SDLK_END:       return 0xFF57;
    case SDLK_INSERT:    return 0xFF63;
    case SDLK_DELETE:    return 0xFFFF;
    case SDLK_LSHIFT:    return 0xFFE1;
    case SDLK_RSHIFT:    return 0xFFE2;
    case SDLK_LCTRL:     return 0xFFE3;
    case SDLK_RCTRL:     return 0xFFE4;
    case SDLK_LALT:      return 0xFFE9;
    case SDLK_RALT:      return 0xFFEA;
    default: break;
    }
    if (s >= SDLK_F1 && s <= SDLK_F12) return 0xFFBE + (s - SDLK_F1);
    return 0;
}


// Chromium's cursor type (cef_cursor_type_t) -> an SDL system cursor.
inline SDL_SystemCursor cursor_for(int t) {
    static const SDL_SystemCursor map[] = {
        SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_CROSSHAIR, SDL_SYSTEM_CURSOR_HAND, SDL_SYSTEM_CURSOR_IBEAM,
        SDL_SYSTEM_CURSOR_WAIT, SDL_SYSTEM_CURSOR_ARROW,                                // pointer cross hand ibeam wait help
        SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENS, SDL_SYSTEM_CURSOR_SIZENESW, SDL_SYSTEM_CURSOR_SIZENWSE,
        SDL_SYSTEM_CURSOR_SIZENS, SDL_SYSTEM_CURSOR_SIZENWSE, SDL_SYSTEM_CURSOR_SIZENESW, SDL_SYSTEM_CURSOR_SIZEWE,
        SDL_SYSTEM_CURSOR_SIZENS, SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENESW, SDL_SYSTEM_CURSOR_SIZENWSE,
        SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENS,                              // column row resize
        SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL,
        SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL,
        SDL_SYSTEM_CURSOR_SIZEALL, SDL_SYSTEM_CURSOR_SIZEALL,                            // panning, move
        SDL_SYSTEM_CURSOR_IBEAM, SDL_SYSTEM_CURSOR_CROSSHAIR, SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_HAND,
        SDL_SYSTEM_CURSOR_WAITARROW, SDL_SYSTEM_CURSOR_NO, SDL_SYSTEM_CURSOR_ARROW,      // vtext cell menu alias progress nodrop copy
    };
    constexpr int kNotAllowed = 37;
    if (t == kNotAllowed) return SDL_SYSTEM_CURSOR_NO;
    if (t >= 0 && t < (int)(sizeof map / sizeof map[0])) return map[t];
    return SDL_SYSTEM_CURSOR_ARROW;
}

} // namespace luxium::cef_input
