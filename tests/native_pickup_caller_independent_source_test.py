from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text(encoding='utf-8')
h=(root/'src/native_pickup_guard_policy.hpp').read_text(encoding='utf-8')
qualifier=s[s.index('PickupGuard::Decision QualifyGroundPickup('):s.index('void __fastcall HookNativeActionDispatch(')]
hook=s[s.index('void __fastcall HookNativeActionDispatch('):s.index('bool NativeCallMatches(',s.index('void __fastcall HookNativeActionDispatch('))]
assert 'std::uint32_t type,std::uint32_t id) noexcept {' in qualifier
assert 'PickupGuard::Candidate(action,type,id)' in qualifier
assert 'caller' not in qualifier
assert '0xFA115,0xFBF30,NativeVerifiedPickupCallRva' in s
assert 'NativeCallMatches(0xFACB9,NativeItemLookupRva)' in s
assert 'PickupGuard::SameItemIdentity(header[0],header[2],id)' in qualifier
assert 'PickupGuard::GroundMode(header[3])' in qualifier
assert 'ResolveGroundRule(rules.get(),ruleItem,resolvedRule)' in qualifier
assert 'GroundRuleItem(code,unit,rules.get(),id)' in qualifier
assert 'matchedRule->show' in qualifier
assert 'NativePickupGuardQualified.load' in qualifier
assert 'ReadProcessMemory(GetCurrentProcess(),player,&playerType' in qualifier
assert 'ReadProcessMemory(GetCurrentProcess(),unit,header.data()' in qualifier
assert 'NativeActionPhase' not in qualifier and 'NativeActionDeadline' not in qualifier
assert 'QualifyGroundPickup(action,player,type,id)==PickupGuard::Decision::Blocked' in hook
assert 'OriginalNativeActionDispatch(action,player,type,id);' in hook
assert 'RecordPickupDecision' not in s and 'DrainPickupDecisions' not in s
assert 'expectedReturnRva' not in h and 'OtherCaller' not in h
print('1.0.0 caller-independent native pickup guard, no diagnostic bookkeeping: ok')
