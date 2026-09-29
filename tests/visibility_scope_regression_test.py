from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
start=s.index('void __fastcall HookSharedLabelPaint(')
end=s.index('// Build-93847 glyph renderer ABI:',start)
fn=s[start:end]
decl='bool concealGroundVisuals=false;'
qual='if (ground && (tint || hide || textColor)) {'
assert fn.count(decl)==1
assert fn.index(decl)<fn.index(qual)<fn.index('concealGroundVisuals=GroundVisibility::ConcealBulkVisuals(')<fn.index('VerifiedRuleGlyphColor=concealGroundVisuals ?')
assert 'GetAsyncKeyState(VK_MENU)' not in fn
assert 'if (!concealGroundVisuals && OriginalSharedLabelPaint)' in fn
assert 'OriginalSharedLabelPaint(rect,textArg,forwardedColor);' in fn
print('PASS: concealGroundVisuals production qualification and native use')
