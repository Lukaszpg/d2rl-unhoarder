"""Generic hidden-hover interoperability contract."""
from pathlib import Path

r=Path(__file__).resolve().parents[1]
p=(r/"src/plugin.cpp").read_text()
h=(r/"interop/in_world_item_label_compat_v1.hpp").read_text()
q=(r/"src/ground_quantity_label.hpp").read_text()

assert '"in-world-item-label-compat"' in h
assert "SupportedEntryRva = 0x000C0420ULL" in h
assert "sizeof(Service) == 64" in h
assert "soe" not in h.lower()

for token in (
    "InWorldFormatterRva = 0xC0420",
    "DiagnoseTrackedTooltipOwner(",
    "PluginCommunication::Acquire(",
    "InWorldCompat::ServiceName",
    "OnCompatibleInWorldLabel",
    "OnCompatibleInWorldStyle",
    "ObserveGroundSoundIdentity(",
    "ObserveNativeRowLiveLabel(",
    "ObserveNativeRowLiveReplacement(",
    "NativeRowRendererRva=0x8DA7E0",
    "NativeRowAppendRva=0x880160",
    "EnableAutomaticNativeHover()",
    "TryForwardNativeRowFontGlyph(",
    "LOOT_INWORLD_COMPAT_READY",
):
    assert token in p, token

attach=p[p.index("bool TryAttachInWorldLabelCompat()"):
         p.index("bool TryAttachForeignSharedLabelPaint(")]
for forbidden in ('d2rl-soe.dll','SoEGetInWorld','providerPluginId = "soe"'):
    assert forbidden not in attach

join=p[p.index("void __cdecl OnInWorldGameJoined"):
       p.index("void RegisterInWorldLifecycle")]
assert "TryAttachInWorldLabelCompat()" in join
assert "ResetNativeRowLiveSession()" in join
assert "EnableAutomaticNativeHover()" in join

unload=p[p.index("D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin"):]
assert unload.index("ResetNativeRowLiveSession()") < unload.index("DetachInWorldLabelCompat()")
assert unload.index("DetachInWorldLabelCompat()") < unload.index("Context = nullptr")

assert '#include "hover_label_style.hpp"' in q
assert "BuildHover(" in q
print("PASS generic hidden-hover observation/style/sound pipeline")
