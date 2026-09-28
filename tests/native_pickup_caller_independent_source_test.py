from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text(encoding='utf-8')
h=(root/'src/native_pickup_guard_policy.hpp').read_text(encoding='utf-8')
qualifier=s[s.index('NativePickupDecision QualifyGroundPickup('):s.index('void RecordPickupDecision(')]
hook=s[s.index('void __fastcall HookNativeActionDispatch('):s.index('bool NativeActionCallMatches(',s.index('void __fastcall HookNativeActionDispatch('))]
assert 'std::uint32_t type,std::uint32_t id) noexcept {' in qualifier
assert 'PickupGuard::Candidate(action,type,id)' in qualifier
assert 'OtherCaller' not in qualifier and 'caller' not in qualifier.split('NativePickupDecision result{};')[0]
assert 'callerRva' in s[s.index('void RecordPickupDecision('):s.index('void DrainPickupDecisions(')]
assert '{0xFA115,NativeActionDispatchRva}' in s # deferred observed call edge
assert 'NativeActionCallMatches(NativeVerifiedPickupCallRva,' in s # immediate observed call edge
assert 'NativeActionCallMatches(0xFACB9,NativeItemLookupRva)' in s
assert 'PickupGuard::SameItemIdentity(header[0],header[2],id)' in qualifier
assert 'PickupGuard::GroundMode(header[3])' in qualifier
assert 'ResolveGroundRule(rules.get(),ruleItem,resolvedRule)' in qualifier and 'GroundRuleItem(result.code,unit,rules.get(),id)' in qualifier and 'matchedRule->show' in qualifier
assert 'NativePickupGuardQualified.load' in qualifier
assert 'ReadProcessMemory(GetCurrentProcess(),player,&playerType' in qualifier
assert 'ReadProcessMemory(GetCurrentProcess(),unit,header.data()' in qualifier
assert 'NativeActionPhase' not in qualifier and 'NativeActionDeadline' not in qualifier
assert 'RecordPickupDecision(id,caller,decision);' in hook
assert 'if(decision.reason==PickupGuard::Decision::Blocked)return;' in hook
assert 'OriginalNativeActionDispatch(action,player,type,id);' in hook
assert 'callerPolicy=none observedImmediate=D2R+0x101AE4 observedDeferred=D2R+0xFA11A' in s
assert 'expectedReturnRva' not in h and 'OtherCaller' not in h
print('1.0.0 caller independent native pickup guard regression: ok')
