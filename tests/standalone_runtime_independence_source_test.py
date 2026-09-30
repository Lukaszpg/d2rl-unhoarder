from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = (root / "src" / "plugin.cpp").read_text(encoding="utf-8")
compat = (root / "interop" / "in_world_item_label_compat_v1.hpp").read_text(encoding="utf-8")

# Old product-specific discovery / fallback ownership stays gone.
for forbidden in (
    "InWorldBackend",
    "TryAttachInWorldBackend",
    "StandaloneInWorldIdentityHook",
    "InWorldRenderScopeApi",
    'd2rl-soe.dll',
    "SoEGetInWorld",
):
    assert forbidden not in src, forbidden

# Standalone bulk-ground behavior remains native to UnHoarder. Hidden-hover
# interoperability is optional and discovered from the actual tracked hook owner.
for required in (
    "HookLabelFormatter",
    "HookInnerNameWriter",
    "UnHoarderSharedLabelPaintMiddleware",
    "UnHoarderGlyphRendererMiddleware",
    "SyncFilterBackgroundState",
    "SyncFilterTextColorState",
    "SyncFilterVisibilityState",
    "ObserveGroundSoundIdentity",
    "UpdateMinimapProjectionIconRule",
    "QualifyGroundQuantityReader();",
    "PublishTooltipCompatService",
    "TryAttachInWorldLabelCompat",
    "EnableAutomaticNativeHover",
    "HookNativeRowAppend",
    "HookNativeRowRenderer",
    "DiagnoseTrackedTooltipOwner",
):
    assert required in src, required

assert '"in-world-item-label-compat"' in compat
assert "soe" not in compat.lower()

join = src[src.index("void __cdecl OnInWorldGameJoined"):
           src.index("void RegisterInWorldLifecycle")]
assert "QualifyGroundQuantityReader();" in join
assert "ResetMinimapTracking();" in join
assert "TryAttachInWorldLabelCompat();" in join
assert "ActivateConfiguredFilter(true)" in join

print("standalone runtime remains product-independent; hidden hover uses generic optional interop")
