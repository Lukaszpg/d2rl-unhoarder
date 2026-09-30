from pathlib import Path
src=(Path(__file__).parents[1]/"src"/"plugin.cpp").read_text()
start=src.index('void __cdecl UnHoarderSharedLabelPaintMiddleware(')
end=src.index('void __fastcall HookSharedLabelPaint(', start)
body=src[start:end]
assert 'GroundVisibility::ConcealBulkVisuals(' in body
assert 'if (!concealGroundVisuals && next)' in body
assert 'next(nextContext,rect,textArg,forwardedColor);' in body
assert 'HiddenGroundInteractionPainterSkips' not in src
assert 'pickup-state-writes=0' in src
print('show:false interaction production contract: ok')
