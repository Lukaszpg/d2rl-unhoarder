from pathlib import Path

src=(Path(__file__).parents[1]/"src"/"plugin.cpp").read_text()
assert "HiddenGroundInteractionPainterSkips" in src
assert "if (concealGroundVisuals) {" in src
assert "HiddenGroundInteractionPainterSkips.fetch_add" in src
assert "else if (OriginalSharedLabelPaint)" in src
assert "OriginalSharedLabelPaint(rect,textArg,forwardedColor);" in src
assert "pickup-state-writes=0" in src
print("show:false interaction source contract: ok")
