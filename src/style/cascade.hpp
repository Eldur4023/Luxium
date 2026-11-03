#pragma once
// The cascade: which declarations win for an element, in CSS order
// (origin and !important, then specificity, then source order), turned into
// a ComputedStyle with inheritance, var() and em/rem resolution.

#include <functional>
#include <unordered_map>
#include <style/computed.hpp>
#include <style/stylesheet.hpp>

#include <string>
#include <vector>

namespace luxium::style {

// A style with only the inherited properties of `parent` (anonymous boxes).
ComputedStyle inherited_style(const ComputedStyle& parent);

// Rules bucketed by what their rightmost compound must match (id, first class, tag): an element only tries
// the rules that can possibly apply instead of every rule of the sheet.
struct RuleIndex {
    std::unordered_map<std::string, std::vector<int>> by_id, by_class, by_tag;
    std::vector<int> universal;
    void build(const std::vector<Rule>& rules);
};

class Resolver {
public:
    // The UA stylesheet is built in.
    Resolver();

    // The page's author rules, already parsed (Page owns them; they must
    // outlive the Resolver).  Parsed once per page load, not per layout.
    void set_author_rules(const std::vector<Rule>* rules) { author_ = rules ? rules : &kNoRules; author_idx_.build(*author_); }

    void set_viewport(float w, float h) { vw_ = w; vh_ = h; }
    // Called with every element's final style (Pseudo::None); the shell animates transitions through it.
    // Returns true when it changed the style (then the style gets a fresh identity).
    std::function<bool(const dom::Node&, ComputedStyle&)> post_hook;
    float viewport_w() const { return vw_; }
    float viewport_h() const { return vh_; }

    // `parent` null = the root element.  `pseudo` selects ::before/::after
    // (the element itself is still `el`; `parent` is then el's own style).
    ComputedStyle compute(const dom::Node& el, const ComputedStyle* parent,
                          Pseudo pseudo = Pseudo::None) const;

private:
    static inline const std::vector<Rule> kNoRules;
    const std::vector<Rule>& ua_;
    const std::vector<Rule>* author_ = &kNoRules;
    RuleIndex author_idx_;
    // Elements that match the same rules under the same parent style compute the same style: remember it.
    mutable std::unordered_map<std::string, ComputedStyle> cache_;
    mutable uint32_t next_id_ = 0;
    const RuleIndex& ua_idx_;
    float vw_ = 1180, vh_ = 762;
    mutable float root_em_ = 16;
};

} // namespace luxium::style
