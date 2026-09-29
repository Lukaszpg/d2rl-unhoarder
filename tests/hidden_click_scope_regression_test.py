from pathlib import Path
s=(Path(__file__).parents[1]/'src'/'plugin.cpp').read_text()
start=s.index('void __fastcall HookSharedLabelPaint(')
end=s.index('// Build-93847 glyph renderer ABI:', start)
body=s[start:end]
assert 'bool concealGroundVisuals=false;' in body
assert 'GroundVisibility::ConcealBulkVisuals(' in body
assert 'if (!concealGroundVisuals && OriginalSharedLabelPaint)' in body
assert 'OriginalSharedLabelPaint(rect,textArg,forwardedColor);' in body
assert 'HiddenGroundLastPainterSkipId' not in body
assert 'HiddenGroundLastPainterSkipCode' not in body
assert '.version = UNHOARDER_VERSION_STRING' in s
print('1.0.0 hidden painter production scope regression: ok')
