#pragma once
// Value parsing shared by the property table and its tests: tokenising,
// lengths (units, calc), colors, gradients.

#include <style/computed.hpp>

#include <optional>
#include <string>
#include <vector>

namespace luxium::style {

// What a relative unit needs to resolve.
struct Env {
    const ComputedStyle* parent = nullptr;
    float em = 16;          // font-size em refers to (own, or parent's for font-size)
    float root_em = 16;
    float vw = 1180, vh = 762;
    float cqw = -1, cqh = -1;   // query container size for cq* units (-1: none -> viewport)
};

// Top-level whitespace split ('/' and ',' come out as their own tokens);
// parentheses and quotes stay glued.
std::vector<std::string> tokens(const std::string& v);
// Split at top-level commas.
std::vector<std::string> split_commas(const std::string& v);

std::string lower_ascii(std::string s);
std::string trim_ws(const std::string& s);

bool parse_length(const std::string& tok, const Env& env, Len& out, bool allow_auto = false);
bool parse_number(const std::string& tok, float& out);
bool parse_angle(const std::string& tok, float& deg);
// `currentcolor` sets *is_current (and returns true with out = 0).
bool parse_color(const std::string& tok, uint32_t& out, bool* is_current = nullptr);
bool parse_gradient(const std::string& value, const Env& env, uint32_t current, Gradient& out);
bool parse_shadow_list(const std::string& value, const Env& env, uint32_t current,
                       std::vector<Shadow>& out);
// `filter` / `backdrop-filter` function list (url() references are not supported: false).
bool parse_filter_list(const std::string& value, const Env& env, uint32_t current, std::vector<FilterFn>& out);

} // namespace luxium::style
