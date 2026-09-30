#pragma once
// Per-item bulk GROUND-LABEL visual masking. This policy intentionally does
// not consult keyboard state (the show-items UI may be toggled). The caller
// must also preserve native paint execution, layout and hover hit testing.
namespace SoE::LootFilter::GroundVisibility {
constexpr bool ConcealBulkVisuals(bool enabled, bool groundCaller,
    bool pairedRecord, bool readableColor, bool verifiedIdentity,
    bool ruleHidden, bool exactName) noexcept {
    return enabled && groundCaller && pairedRecord && readableColor &&
           verifiedIdentity && ruleHidden && exactName;
}
} // namespace SoE::LootFilter::GroundVisibility

