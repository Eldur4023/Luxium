#pragma once
// Computed-style dump for the css-tests: one JSON entry per [data-t] element
// with the properties Chromium's getComputedStyle reports the same way.

#include <dom/node.hpp>
#include <style/cascade.hpp>

#include <string>
#include <vector>

namespace luxium::style {

// The property names the probe compares (same list on the JS side).
const std::vector<std::string>& probe_properties();

std::string dump_styles_json(const dom::Node& root, const Resolver& resolver);

} // namespace luxium::style
