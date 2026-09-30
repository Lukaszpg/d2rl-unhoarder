from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = (root / "src" / "plugin.cpp").read_text(encoding="utf-8")

for forbidden in (
    "InWorldBackend",
    "TryAttachInWorldBackend",
    "StandaloneInWorldIdentityHook",
    "InWorldRenderScopeApi",
    "EnableAutomaticNativeHover",
    "HookNativeRowAppend",
    "HookNativeRowRenderer",
):
    assert forbidden not in src, forbidden

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
):
    assert required in src, required

join = src[src.index("void __cdecl OnInWorldGameJoined"):
           src.index("void RegisterInWorldLifecycle")]
assert "QualifyGroundQuantityReader();" in join
assert "ResetMinimapTracking();" in join
assert "ActivateConfiguredFilter(true)" in join

print("standalone runtime has no legacy mod-specific dependency")
