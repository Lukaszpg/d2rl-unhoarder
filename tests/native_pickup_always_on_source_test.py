from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
assert '.version = "1.0.0"' in s
hook=s[s.index('void __fastcall HookNativeActionDispatch('):s.index('\nbool NativeActionCallMatches(',s.index('void __fastcall HookNativeActionDispatch('))]
assert 'if(action==PickupGuard::PickupAction && type==PickupGuard::ItemUnitType)' in hook
assert 'if(decision.reason==PickupGuard::Decision::Blocked)return;' in hook
assert 'OriginalNativeActionDispatch(action,player,type,id);' in hook
qual=s[s.index('NativePickupDecision QualifyGroundPickup('):s.index('void RecordPickupDecision(')]
for term in ('PickupGuard::Decision::LookupFailed','PickupGuard::Decision::NotGround','PickupGuard::Decision::NoHiddenRule','PickupGuard::Decision::Blocked','PickupGuard::GroundMode(header[3])','ResolveGroundRule(rules.get(),ruleItem,resolvedRule)'):
    assert term in qual,term
for timer in ('NativeActionPhase','NativeActionDeadline','GetAsyncKeyState','PickupClickMs'):
    assert timer not in qual,timer
worker=s[s.index('void RuntimeWorkerLoop('):s.index('// The best currently verified downstream',s.index('void RuntimeWorkerLoop('))]
assert 'DrainPickupDecisions();' not in worker
print('1.0.0 pickup guard remains phase-independent; probe status pump removed: ok')
