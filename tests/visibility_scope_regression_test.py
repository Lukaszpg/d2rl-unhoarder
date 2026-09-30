from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
start=s.index('void __cdecl UnHoarderSharedLabelPaintMiddleware(')
end=s.index('void __fastcall HookSharedLabelPaint(',start)
fn=s[start:end]
decl='bool concealGroundVisuals=false;'
qual='if (ground && (tint || hide || textColor)) {'
assert fn.count(decl)==1
assert fn.index(decl)<fn.index(qual)<fn.index('concealGroundVisuals=GroundVisibility::ConcealBulkVisuals(')<fn.index('VerifiedRuleGlyphColor=concealGroundVisuals ?')
assert 'GetAsyncKeyState(VK_MENU)' not in fn
assert 'if (!concealGroundVisuals && next)' in fn
assert 'next(nextContext,rect,textArg,forwardedColor);' in fn
print('PASS: concealGroundVisuals production qualification and native use')
