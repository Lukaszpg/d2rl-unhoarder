#pragma once
#include "filter_rule_engine.hpp"
namespace UnHoarder::GroundActionDemand {
// Conservative: an unknown predicate keeps the full visibility query required.
// No order shortcut is taken; even an unreachable Hide keeps the slow path.
template<class Rule>
bool HidePossible(const std::vector<Rule>& rules,const RuleEngine::Item& item) noexcept {
    for(const auto& rule:rules)
        if(!rule.show && RuleEngine::EvaluateRuleFailOpen(rule,item)!=RuleEngine::MatchState::NoMatch)return true;
    return false;
}
// Conservative action reachability. Unknown fields remain potentially matching;
// false proves that no completion can supply either requested geometry action.
// No native value or visibility/pickup decision is cached or inferred here.
template<class Rule>
bool GeometryPossible(const std::vector<Rule>& rules,const RuleEngine::Item& item) noexcept {
    for(const auto& rule:rules)
        if((rule.hasName || rule.hasMinimapIcon) &&
           RuleEngine::EvaluateRuleFailOpen(rule,item)!=RuleEngine::MatchState::NoMatch)return true;
    return false;
}
}
