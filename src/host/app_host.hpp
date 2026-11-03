#pragma once
// Luxium app host (`luxium --host`): one browser process, one window per
// client app.  Protocol and lifecycle: docs/HOST.md.
#include <string>

namespace luxium::host {

std::string runtime_dir();    // $XDG_RUNTIME_DIR/luxium (0700)
std::string socket_path();    // <runtime_dir>/host.sock (0600)
std::string lock_path();      // <runtime_dir>/host.lock

int run();                    // blocks until the last client is gone; returns the exit code

} // namespace luxium::host
