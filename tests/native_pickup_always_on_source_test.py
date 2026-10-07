from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath("src/plugin.cpp").read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
hook=s[s.index("void __fastcall HookNativeActionDispatch("):s.index("bool NativeCallMatches(",s.index("void __fastcall HookNativeActionDispatch("))]
assert "const bool pickup=action==PickupGuard::PickupAction" in hook
assert "type==PickupGuard::ItemUnitType;" in hook
assert "if(pickup)" in hook
assert "QualifyGroundPickup(action,player,type,id)==PickupGuard::Decision::Blocked" in hook
assert "OriginalNativeActionDispatch(action,player,type,id);" in hook
qual=s[s.index("PickupGuard::Decision QualifyGroundPickup("):s.index("void __fastcall HookNativeActionDispatch(")]
for term in ("PickupGuard::Decision::LookupFailed","PickupGuard::Decision::NotGround","PickupGuard::Decision::NoHiddenRule","PickupGuard::Decision::Blocked","PickupGuard::GroundMode(header[3])","ResolveGroundRule(rules.get(),ruleItem,resolvedRule)"):
    assert term in qual,term
for timer in ("NativeActionPhase","NativeActionDeadline","GetAsyncKeyState","PickupClickMs"):
    assert timer not in qual,timer
assert "RecordPickupDecision" not in s and "DrainPickupDecisions" not in s
print("pickup guard remains phase-independent; diagnostic queue removed: ok")
