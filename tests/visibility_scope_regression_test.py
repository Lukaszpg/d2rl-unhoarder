from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
fn=s[s.index('void __fastcall HookSharedLabelPaint('):s.index('// Legacy record-wr',s.index('void __fastcall HookSharedLabelPaint('))]
decl='bool concealGroundVisuals=false;'
qual='if ((observe || tint || glyphObserve || cyan || hide) && (ground || neighbor)) {'
assert fn.count(decl)==1
assert fn.index(decl)<fn.index(qual)<fn.index('concealGroundVisuals=GroundVisibility::ConcealBulkVisuals(')<fn.index('if (concealGroundVisuals) {')<fn.index('VerifiedRuleGlyphColor=concealGroundVisuals ?')
assert 'GetAsyncKeyState(VK_MENU)' not in fn
assert 'OriginalSharedLabelPaint(rect,textArg,forwardedColor);' in fn
print('PASS: concealGroundVisuals declaration before ground qualification and native use')
