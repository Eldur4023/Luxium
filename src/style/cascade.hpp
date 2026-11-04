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
    std::vector<int> layer;   // per rule: 0 = unlayered, n = the n-th @layer by first appearance
    void build(const std::vector<Rule>& rules);
};

class Resolver {
public:
    // The UA stylesheet is built in.
    Resolver();

    // The page's author rules, already parsed (Page owns them; they must
    // outlive the Resolver).  Parsed once per page load, not per layout.
    void set_author_rules(const std::vector<Rule>* rules);   // also reads the @property registrations
    struct RegisteredProp { std::string name, initial; bool inherits = true; };

    void set_viewport(float w, float h) { vw_ = w; vh_ = h; }
    // Query containers measured by the previous layout pass (container-type != normal): content-box size.
    struct Container { float w = 0, h = 0; uint8_t type = 1; std::string names; };
    void set_containers(std::unordered_map<const dom::Node*, Container> m) { containers_ = std::move(m); cache_.clear(); }
    // Called with every element's (and ::before/::after's: `pseudo` = Pseudo as int) final style; the shell animates through it.
    // Returns true when it changed the style (then the style gets a fresh identity).
    std::function<bool(const dom::Node&, ComputedStyle&, int pseudo)> post_hook;
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
    std::vector<RegisteredProp> registered_;
    unsigned pseudo_mask_ = 0;   // bit per Pseudo that some author selector uses
public:
    bool has_pseudo(Pseudo p) const { return (pseudo_mask_ >> unsigned(p)) & 1u; }
    bool pseudo_matches(const dom::Node& el, Pseudo p) const;   // some author rule for `el`::p applies
private:
    float vw_ = 1180, vh_ = 762;
    mutable float root_em_ = 16;
    std::unordered_map<const dom::Node*, Container> containers_;
    // nearest query container of `el` (itself only for its ::before/::after) named `name` ("" = any); null if none
    const Container* container_for(const dom::Node& el, Pseudo pseudo, const std::string& name) const;
    bool container_rule_ok(const std::string& conds, const dom::Node& el, Pseudo pseudo) const;
};

} // namespace luxium::style
