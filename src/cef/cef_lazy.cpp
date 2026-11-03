// The CEF API as the rest of Luxium sees it (cef_backend.hpp), forwarding to
// libluxium_cef.so, which is dlopen()ed on the first call that needs Chromium.
// Nothing here runs Chromium code until then: no libcef.so mapped, no
// subprocess.  CEF re-executes this binary with --type=... for its helpers;
// only those invocations load the library from early().
#include "cef_api.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace luxium::cef {

namespace {

int g_argc = 0;
char** g_argv = nullptr;

std::string plugin_path() {
    char exe[4096] = {0};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string(".");
    const size_t sl = dir.find_last_of('/');
    return (sl == std::string::npos ? std::string(".") : dir.substr(0, sl)) + "/libluxium_cef.so";
}

const Api* g_api = nullptr;

const Api* api() {
    static std::once_flag once;
    std::call_once(once, [] {
        void* h = dlopen(plugin_path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { std::fprintf(stderr, "luxium: cannot load the Chromium backend: %s\n", dlerror()); return; }
        auto get = reinterpret_cast<const Api* (*)()>(dlsym(h, "luxium_cef_api"));
        g_api = get ? get() : nullptr;
        if (g_api) g_api->early(g_argc, g_argv);   // the browser process: returns -1 and remembers argv for init()
    });
    return g_api;
}

} // namespace

bool built() { return access(plugin_path().c_str(), R_OK) == 0; }

int early(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--type=", 7) == 0) {   // a Chromium helper process
            const Api* a = api();
            return a ? a->early(argc, argv) : 1;
        }
    return -1;
}

void flush_all() { if (g_api) g_api->flush_all(); }
void shutdown() { if (g_api) g_api->shutdown(); }

// Only these start Chromium; every other call (focus changes, getters, close...) is a no-op while it is not loaded.
static bool starts_chromium(const char* n) {
    return !std::strcmp(n, "init") || !std::strcmp(n, "attach") || !std::strcmp(n, "open_window") ||
           !std::strcmp(n, "set_profile_root");
}

#define X(id, ret, name, params, args, dflt) \
    ret name params { const Api* api_ = starts_chromium(#name) ? api() : g_api; return api_ ? api_->id args : dflt; }
LUXIUM_CEF_API(X)
#undef X

} // namespace luxium::cef
