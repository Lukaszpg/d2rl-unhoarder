#pragma once
#include <cstdint>

namespace UnHoarder::NativePickupGuardPolicy {
// Build 93847: action 0x16, UnitAny type 4.
inline constexpr std::uint32_t PickupAction=22;
inline constexpr std::uint32_t ItemUnitType=4;
inline constexpr std::uint32_t OnGroundMode=3;
// One terminal reason for every action-22/type-4 dispatch, including when
// the original is forwarded. The guard is independent of diagnostic state.
enum class Decision : std::uint8_t {
    Blocked, GuardInactive, InvalidTargetId,
    VisibilityNotArmed, RulesModeInactive, CodeReaderUnavailable,
    NullPlayer, Reentrant, NoHiddenRules, InvalidPlayer, LookupFailed,
    InvalidUnit, NotGround, InvalidCode, NoHiddenRule
};
constexpr const char* DecisionName(Decision value) noexcept {
    switch(value) {
    case Decision::Blocked: return "hidden-ground-rule";
    case Decision::GuardInactive: return "guard-inactive";
    case Decision::InvalidTargetId: return "zero-target-id";
    case Decision::VisibilityNotArmed: return "visibility-not-armed";
    case Decision::RulesModeInactive: return "rules-mode-inactive";
    case Decision::CodeReaderUnavailable: return "code-reader-unavailable";
    case Decision::NullPlayer: return "null-player";
    case Decision::Reentrant: return "reentrant";
    case Decision::NoHiddenRules: return "no-active-hidden-rules";
    case Decision::InvalidPlayer: return "invalid-player";
    case Decision::LookupFailed: return "lookup-failed";
    case Decision::InvalidUnit: return "invalid-unit-identity";
    case Decision::NotGround: return "not-ground";
    case Decision::InvalidCode: return "invalid-item-code";
    case Decision::NoHiddenRule: return "visible-or-no-hidden-rule";
    }
    return "unknown";
}
constexpr bool Candidate(std::uint32_t action,std::uint32_t type,
    std::uint32_t id) noexcept {
    return action==PickupAction && type==ItemUnitType && id!=0;
}
constexpr bool SameItemIdentity(std::uint32_t type,std::uint32_t id,
    std::uint32_t requestedId) noexcept {
    return type==ItemUnitType && id!=0 && id==requestedId;
}
constexpr bool GroundMode(std::uint32_t mode) noexcept {
    return mode==OnGroundMode;
}
} // namespace UnHoarder::NativePickupGuardPolicy
