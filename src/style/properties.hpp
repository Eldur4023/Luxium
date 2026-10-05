#pragma once
// The property table: applies ONE declaration (already var()-substituted) to
// a ComputedStyle.  Shorthands expand to longhands here.

#include <style/computed.hpp>
#include <style/values.hpp>

#include <string>

namespace luxium::style {

// Returns false if the property is unknown or the value invalid (the
// declaration is then ignored, like a browser does).
bool apply_property(ComputedStyle& s, const Env& env, const std::string& name,
                    const std::string& value);

// font-size must be applied before everything that uses em.
bool is_font_size_property(const std::string& name);

// Copies the named longhand (or all longhands of a shorthand) from `src`:
// implements inherit / initial / unset.
bool copy_property(ComputedStyle& dst, const ComputedStyle& src, const std::string& name);

// Does this property inherit by default?
bool property_inherits(const std::string& name);

} // namespace luxium::style
