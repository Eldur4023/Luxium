// libluxium_cef.so entry point: hands the lazy loader (cef_lazy.cpp) the real
// implementations from cef_backend.cpp.
#include "cef_api.hpp"

extern "C" __attribute__((visibility("default"))) const luxium::cef::Api* luxium_cef_api() {
    using namespace luxium::cef;
    static const Api api = {
#define X(id, ret, name, params, args, dflt) static_cast<ret (*) params>(&luxium::cef::name),
        LUXIUM_CEF_API(X)
#undef X
        &luxium::cef::early,
        &luxium::cef::flush_all,
        &luxium::cef::shutdown,
    };
    return &api;
}
