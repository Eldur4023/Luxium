#pragma once
// The cascade: which declarations win for an element, in CSS order
// (origin and !important, then specificity, then source order), turned into
// a ComputedStyle with inheritance, var() and em/rem resolution.

#include <functional>
#include <style/computed.hpp>
#include <style/stylesheet.hpp>

#include <string>
#include <vector>

namespace luxium::style {

// A style with only the inherited properties of `parent` (anonymous boxes).
ComputedStyle inherited_style(const ComputedStyle& parent);

class Resolver {
public:
    // The UA stylesheet is built in.
    Resolver();

    // The page's author rules, already parsed (Page owns them; they must
    // outlive the Resolver).  Parsed once per page load, not per layout.
    void set_author_rules(const std::vector<Rule>* rules) { author_ = rules ? rules : &kNoRules; }

    void set_viewport(float w, float h) { vw_ = w; vh_ = h; }
    // Called with every element's final style (Pseudo::None); the shell animates transitions through it.
    std::function<void(const dom::Node&, ComputedStyle&)> post_hook;
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
    float vw_ = 1180, vh_ = 762;
    mutable float root_em_ = 16;
};

} // namespace luxium::style
