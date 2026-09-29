#pragma once
// Portable, immutable, first-match filter decisions. No D2R code, SDK handles,
// native pointers, I/O, timers or allocations during rule evaluation.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace SoE::LootFilter::RuleEngine {
struct NumberTest {
    bool enabled{};
    bool hasEq{}, hasMin{}, hasMax{};
    std::uint32_t eq{}, min{}, max{};
    bool minInclusive{true}, maxInclusive{true};
    constexpr bool Matches(std::uint32_t value) const noexcept {
        return !enabled || ((!hasEq || value==eq) &&
            (!hasMin || (minInclusive?value>=min:value>min)) &&
            (!hasMax || (maxInclusive?value<=max:value<max)));
    }
};
struct Item {
    std::uint32_t code{};
    bool classIdKnown{};
    std::uint32_t classId{};
    bool quantityKnown{};
    std::uint32_t quantity{};
    // Quality/item level: guarded native scalar reader. Socket count: guarded
    // qualified GetUnitStat(item_numsockets/194) on verified labels (3/5),
    // strict ground (3) for pickup. Ethereal uses qualified native item-data
    // flag 0x00400000; identified uses guarded bit 0x00000010 in modes 3/5.
    bool qualityKnown{};
    std::uint32_t quality{}; // D2RL::Items::Quality numeric enum
    bool itemLevelKnown{};
    std::uint32_t itemLevel{};
    bool socketsKnown{};
    std::uint32_t sockets{};
    bool etherealKnown{};
    bool ethereal{};
    bool identifiedKnown{};
    bool identified{};
};
struct Conditions {
    std::vector<std::uint32_t> codes;       // OR among entries
    std::vector<std::uint32_t> baseCodes;   // from Weapons/Armor name -> code, OR
    std::vector<std::uint32_t> typeCodes;   // from ItemTypes hierarchy -> item code, OR
    NumberTest quantity;
    // Known-value matching: unknown is NEVER treated as zero/false. The
    // active parser enables quality/itemLevel/sockets/ethereal/identified.
    std::vector<std::uint32_t> qualities;
    NumberTest itemLevel;
    NumberTest sockets;
    bool etherealEnabled{}, etherealExpected{};
    bool identifiedEnabled{}, identifiedExpected{};
    bool superiorEnabled{}, superiorExpected{};
    bool Matches(const Item& item) const noexcept {
        if (!codes.empty()) {
            bool found=false;
            for (const auto code:codes) if (code==item.code) { found=true; break; }
            if (!found) return false;
        }
        if (!baseCodes.empty()) {
            bool found=false;
            for (const auto code:baseCodes)
                if (code==item.code) { found=true; break; }
            if (!found) return false;
        }
        // All itemType selectors compile to SORTED codes at reload; no
        // O(number-of-items-in-category) scan on every label draw.
        if(!typeCodes.empty() &&
           !std::binary_search(typeCodes.begin(),typeCodes.end(),item.code))
            return false;
        if (quantity.enabled &&
            (!item.quantityKnown || !quantity.Matches(item.quantity))) return false;
        if (!qualities.empty()) {
            if (!item.qualityKnown) return false;
            bool found=false;
            for (const auto quality:qualities)
                if (quality==item.quality) { found=true; break; }
            if (!found) return false;
        }
        if (itemLevel.enabled &&
            (!item.itemLevelKnown || !itemLevel.Matches(item.itemLevel)))
            return false;
        if (sockets.enabled &&
            (!item.socketsKnown || !sockets.Matches(item.sockets)))
            return false;
        if (etherealEnabled &&
            (!item.etherealKnown || item.ethereal!=etherealExpected))
            return false;
        if (identifiedEnabled &&
            (!item.identifiedKnown || item.identified!=identifiedExpected))
            return false;
        if (superiorEnabled &&
            (!item.qualityKnown || (item.quality==3U)!=superiorExpected))
            return false;
        return true;
    }
};
// Version 1's first matching code remains the decision. Version 2's first
// matching conditions block wins, including Show-only blocks. Nothing falls
// through to a later Hide or style block after the first match.
template<class Rule>
const Rule* FirstMatch(const std::vector<Rule>& rules,const Item& item) noexcept {
    for (const auto& rule:rules) {
        if (rule.schema2 ? rule.conditions.Matches(item) : rule.code==item.code)
            return &rule;
    }
    return nullptr;
}
// Inspect first-match order using only the cheap, already-known properties.
// A later rarity/ilvl rule must not force a native memory read for an item
// already decided by an earlier code-only rule. Conversely,
// an applicable property rule with unknown data must stop evaluation before
// a later generic show:false fallback: unknown always stays visible.
inline bool ContainsCode(const std::vector<std::uint32_t>& values,
    std::uint32_t value) noexcept {
    for (const auto v:values) if(v==value) return true;
    return false;
}
inline bool CheapConditionsCannotMatch(const Conditions& c,
    const Item& item) noexcept {
    if(!c.codes.empty() && !ContainsCode(c.codes,item.code)) return true;
    if(!c.baseCodes.empty() && !ContainsCode(c.baseCodes,item.code))
        return true;
    if(!c.typeCodes.empty() &&
       !std::binary_search(c.typeCodes.begin(),c.typeCodes.end(),item.code))
        return true;
    if(c.quantity.enabled && item.quantityKnown &&
       !c.quantity.Matches(item.quantity)) return true;
    return false;
}

// Three-state ordered evaluation is required for fail-open native properties:
// an unresolved potentially-matching block stops evaluation so a later broad
// Hide cannot accidentally win while a native property is unavailable.
enum class MatchState : std::uint8_t { NoMatch, Match, Unknown };

inline MatchState EvaluateConditionsFailOpen(const Conditions& c,
    const Item& item) noexcept {
    if(CheapConditionsCannotMatch(c,item)) return MatchState::NoMatch;
    // First reject every predicate whose value is already known. This is
    // important when one field is unavailable but another known field proves
    // the block cannot match: the known mismatch must win over Unknown.
    if(c.quantity.enabled && item.quantityKnown &&
       !c.quantity.Matches(item.quantity)) return MatchState::NoMatch;
    if(!c.qualities.empty() && item.qualityKnown &&
       !ContainsCode(c.qualities,item.quality)) return MatchState::NoMatch;
    if(c.itemLevel.enabled && item.itemLevelKnown &&
       !c.itemLevel.Matches(item.itemLevel)) return MatchState::NoMatch;
    if(c.sockets.enabled && item.socketsKnown &&
       !c.sockets.Matches(item.sockets)) return MatchState::NoMatch;
    if(c.etherealEnabled && item.etherealKnown &&
       item.ethereal!=c.etherealExpected) return MatchState::NoMatch;
    if(c.identifiedEnabled && item.identifiedKnown &&
       item.identified!=c.identifiedExpected) return MatchState::NoMatch;
    if(c.superiorEnabled && item.qualityKnown &&
       (item.quality==3U)!=c.superiorExpected) return MatchState::NoMatch;

    // Only after all known facts are compatible do missing required values
    // make this potentially-matching block unresolved.
    if(c.quantity.enabled && !item.quantityKnown) return MatchState::Unknown;
    if((!c.qualities.empty() || c.superiorEnabled) && !item.qualityKnown)
        return MatchState::Unknown;
    if(c.itemLevel.enabled && !item.itemLevelKnown) return MatchState::Unknown;
    if(c.sockets.enabled && !item.socketsKnown) return MatchState::Unknown;
    if(c.etherealEnabled && !item.etherealKnown) return MatchState::Unknown;
    if(c.identifiedEnabled && !item.identifiedKnown) return MatchState::Unknown;
    return MatchState::Match;
}

template<class Rule>
constexpr bool Continues(const Rule& rule) noexcept {
    if constexpr (requires { rule.continueEvaluation; })
        return static_cast<bool>(rule.continueEvaluation);
    else
        return false;
}

template<class Rule>
MatchState EvaluateRuleFailOpen(const Rule& rule,const Item& item) noexcept {
    if(!rule.schema2)
        return rule.code==item.code ? MatchState::Match : MatchState::NoMatch;
    return EvaluateConditionsFailOpen(rule.conditions,item);
}

// Visit every matching block in order until a matching non-Continue block, or
// until an unresolved potentially-matching property block. The caller merges
// actions, so later matching blocks can override fields supplied by earlier
// continued blocks while unrelated fields carry forward.
template<class Rule,class Apply,class OnUnknown>
bool ResolveMatchingRulesFailOpen(const std::vector<Rule>& rules,
    const Item& item,Apply&& apply,OnUnknown&& onUnknown) noexcept {
    bool matched{};
    for(const auto& rule:rules) {
        const auto state=EvaluateRuleFailOpen(rule,item);
        if(state==MatchState::NoMatch) continue;
        if(state==MatchState::Unknown) {
            onUnknown();
            return matched;
        }
        matched=true;
        apply(rule);
        if(!Continues(rule)) return true;
    }
    return matched;
}

template<class Rule,class Apply>
bool ResolveMatchingRulesFailOpen(const std::vector<Rule>& rules,
    const Item& item,Apply&& apply) noexcept {
    return ResolveMatchingRulesFailOpen(rules,item,
        static_cast<Apply&&>(apply),[]() noexcept {});
}

inline bool UsesNativeQualityLevel(const Conditions& c) noexcept {
    return !c.qualities.empty() || c.itemLevel.enabled || c.superiorEnabled;
}
template<class Rule>
bool NeedsNativeQualityLevel(const std::vector<Rule>& rules,
    const Item& item) noexcept {
    // Continue-aware: after a fully known continued match, later rules may
    // still require quality/ilvl.
    for(const auto& rule:rules) {
        if(!rule.schema2) {
            if(rule.code==item.code && !Continues(rule)) return false;
            if(rule.code==item.code) continue;
            continue;
        }
        const auto& c=rule.conditions;
        if(CheapConditionsCannotMatch(c,item)) continue;
        if(c.quantity.enabled) {
            if(!item.quantityKnown) return false;
            if(!c.quantity.Matches(item.quantity)) continue;
        }
        if(!c.qualities.empty() && item.qualityKnown &&
           !ContainsCode(c.qualities,item.quality)) continue;
        if(c.itemLevel.enabled && item.itemLevelKnown &&
           !c.itemLevel.Matches(item.itemLevel)) continue;
        if(c.sockets.enabled && item.socketsKnown &&
           !c.sockets.Matches(item.sockets)) continue;
        if(c.etherealEnabled && item.etherealKnown &&
           item.ethereal!=c.etherealExpected) continue;
        if(c.identifiedEnabled && item.identifiedKnown &&
           item.identified!=c.identifiedExpected) continue;
        if(c.superiorEnabled && item.qualityKnown &&
           (item.quality==3U)!=c.superiorExpected) continue;
        if((!c.qualities.empty() || c.itemLevel.enabled || c.superiorEnabled) &&
           (!item.qualityKnown || (c.itemLevel.enabled && !item.itemLevelKnown)))
            return true;
        if(c.Matches(item) && !Continues(rule)) return false;
    }
    return false;
}
// Inspect ordered rules in order, choosing only the first missing property
// that could affect the decision. Reader groups execute lazily in rule
// order without forcing property work for a preceding code-only match.
enum class NextProperty : std::uint8_t { None, QualityLevel, Sockets, Ethereal, Identified };
template<class Rule>
NextProperty NextNativeProperty(const std::vector<Rule>& rules,
    const Item& item) noexcept {
    for(const auto& rule:rules) {
        if(!rule.schema2) {
            if(rule.code==item.code && !Continues(rule))
                return NextProperty::None;
            continue;
        }
        const auto& c=rule.conditions;
        if(CheapConditionsCannotMatch(c,item)) continue;
        if(c.quantity.enabled) {
            // Quantity is populated before this staged native-reader loop.
            // If it is unavailable, fail open instead of reading unrelated
            // native properties or falling through to a later Hide.
            if(!item.quantityKnown) return NextProperty::None;
            if(!c.quantity.Matches(item.quantity)) continue;
        }
        if(!c.qualities.empty() && item.qualityKnown &&
           !ContainsCode(c.qualities,item.quality)) continue;
        if(c.itemLevel.enabled && item.itemLevelKnown &&
           !c.itemLevel.Matches(item.itemLevel)) continue;
        if(c.sockets.enabled && item.socketsKnown &&
           !c.sockets.Matches(item.sockets)) continue;
        if(c.etherealEnabled && item.etherealKnown &&
           item.ethereal!=c.etherealExpected) continue;
        if(c.identifiedEnabled && item.identifiedKnown &&
           item.identified!=c.identifiedExpected) continue;
        if(c.superiorEnabled && item.qualityKnown &&
           (item.quality==3U)!=c.superiorExpected) continue;
        if((!c.qualities.empty() && !item.qualityKnown) ||
           (c.itemLevel.enabled && !item.itemLevelKnown) ||
           (c.superiorEnabled && !item.qualityKnown))
            return NextProperty::QualityLevel;
        if(c.sockets.enabled && !item.socketsKnown)
            return NextProperty::Sockets;
        if(c.etherealEnabled && !item.etherealKnown)
            return NextProperty::Ethereal;
        if(c.identifiedEnabled && !item.identifiedKnown)
            return NextProperty::Identified;
        if(c.Matches(item) && !Continues(rule)) return NextProperty::None;
        // A matching Continue block deliberately falls through so a later
        // block can request another property.
    }
    return NextProperty::None;
}
template<class Rule>
const Rule* FirstMatchFailOpenProperties(const std::vector<Rule>& rules,
    const Item& item) noexcept {
    for(const auto& rule:rules) {
        const auto state=EvaluateRuleFailOpen(rule,item);
        if(state==MatchState::NoMatch) continue;
        if(state==MatchState::Unknown) return nullptr;
        return &rule;
    }
    return nullptr;
}
} // namespace SoE::LootFilter::RuleEngine
