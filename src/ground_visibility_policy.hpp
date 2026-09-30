#pragma once
// Per-item bulk GROUND-LABEL visual masking. This policy intentionally does
// not consult keyboard state (the show-items UI may be toggled). The caller
// must also preserve native paint execution, layout and hover hit testing.
namespace UnHoarder::GroundVisibility {
constexpr bool ConcealBulkVisuals(bool enabled, bool groundCaller,
    bool pairedRecord, bool readableColor, bool verifiedIdentity,
    bool ruleHidden, bool exactName) noexcept {
    return enabled && groundCaller && pairedRecord && readableColor &&
           verifiedIdentity && ruleHidden && exactName;
}
// The hover renderer is never suppressed from a rule or row address alone.
// Every argument is evidence gathered from the same live renderer invocation.
constexpr bool SuppressHiddenHover(bool enabled, bool currentItem,
    bool exactAppend, bool sameRow, bool sameText, bool sameGeneration,
    bool ruleHidden) noexcept {
    return enabled && currentItem && exactAppend && sameRow && sameText &&
        sameGeneration && ruleHidden;
}
} // namespace UnHoarder::GroundVisibility

