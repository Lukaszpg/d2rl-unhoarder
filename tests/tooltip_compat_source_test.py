from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = (root / "src/plugin.cpp").read_text(encoding="utf-8")
hdr = (root / "interop/unhoarder_tooltip_compat_v1.hpp").read_text(encoding="utf-8")
docs = (root / "docs/tooltip-compat.md").read_text(encoding="utf-8")

for term in (
    "PluginCommunicationService",
    "DiagnosticsService",
    "DiagnoseTrackedTooltipOwner",
    "TryAttachForeignSharedLabelPaint",
    "TryAttachForeignGlyphRenderer",
    "PublishTooltipCompatService",
    "unhoarder-tooltip-compat",
    "ForeignPaintCompatLease",
    "ForeignGlyphCompatLease",
    "UnHoarderSharedLabelPaintMiddleware",
    "UnHoarderGlyphRendererMiddleware",
):
    assert term in src, term

assert "SharedLabelPaintRva = 0x001FA8E0ULL" in hdr
assert "GlyphRendererRva    = 0x00658510ULL" in hdr
assert "Calling it zero times suppresses" in docs
assert "Do not use" in docs and "private MinHook detour" in docs
assert "GetProcAddress" not in hdr
assert "decode a foreign detour" in docs
print("tooltip compatibility v1 source contract: ok")
