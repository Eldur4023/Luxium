#pragma once
// Mounting .luxp files (the format itself: page/luxp.hpp).
//
// Mounted at "dir/app.luxp", page "x.html" answers to "dir/app.luxp/x.html",
// so links and relative resource paths resolve exactly as they did on disk.
// Producing a .luxp is Lux's job (lux pack, or render() for a client that
// accepts application/x-luxp); the browser only opens and verifies them.

#include <optional>
#include <string>
#include <vector>

#include <page/luxp.hpp>

namespace luxium::bundle {

// Every resource read goes through here.  Inside a mounted .luxp ONLY the
// bundle answers (a "../" path never escapes to disk); elsewhere, the disk.
std::optional<std::string> read_file(const std::string& path);

// A page of a mounted bundle, or null.
const page::PageSections* find_page(const std::string& path);

// Mounts a .luxp; returns the entry page path or nullopt (err filled).
std::optional<std::string> mount(const std::string& luxp_path, std::string* err);

// Verifies every section and reports, per page, what it is made of and
// everything its program can possibly do.  false if anything is rejected.
bool inspect(const std::string& luxp_path, std::string* report);

} // namespace luxium::bundle
