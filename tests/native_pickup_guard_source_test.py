from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
for term in ('.version = "1.0.0"','LOOT_PICKUP_GUARD_READY version=1.0.0','Context->CheckExpectedBytes(NativeItemLookupRva,','NativePickupGuardQualified.store(lookupQualified','QualifyGroundPickup(action,player,type,id)','PickupGuard::SameItemIdentity(header[0],header[2],id)','PickupGuard::GroundMode(header[3])','OriginalGetItemCode(unit)','ResolveGroundRule(rules.get(),ruleItem,resolvedRule)','matchedRule->show','NativePickupGuardBlocked.fetch_add','OriginalNativeActionDispatch(action,player,type,id);','NativePickupGuardQualified.store(false,std::memory_order_release)'):
    assert term in s,term
print('1.0.0 fail-open qualified pickup guard source contracts: ok')
