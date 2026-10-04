// CEF backend stub: this binary was built without third_party/cef.
// http pages fall back to our own engine + curl.  See cef_backend.hpp.

#include "cef_backend.hpp"

namespace luxium::cef {

bool built() { return false; }
int early(int, char**) { return -1; }
void init(unsigned long, unsigned long) {}
void attach(unsigned long, unsigned long, const std::string&) {}
void navigate(const std::string&) {}
void go_back() {}
void go_forward() {}
void reload() {}
void set_view(int, int) {}
void mouse_move(int, int, bool, uint32_t) {}
void mouse_button(int, int, int, bool, int, uint32_t) {}
void mouse_wheel(int, int, int, int, uint32_t) {}
void key(int, uint32_t, uint32_t, uint32_t, const char*) {}
void set_focus(bool) {}
bool copy_frame(uint8_t*, int, int, int, int*, int*) { return false; }
int cursor() { return 0; }
int paints() { return 0; }
bool pop_console(int*, std::string*) { return false; }
std::string url() { return {}; }
std::string title() { return {}; }
bool loaded() { return false; }
void shutdown() {}

} // namespace luxium::cef
