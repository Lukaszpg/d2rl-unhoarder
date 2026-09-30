from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
for term in ('.version = UNHOARDER_VERSION_STRING','LOOT_PICKUP_GUARD_READY version=" UNHOARDER_VERSION_STRING "','Context->CheckExpectedBytes(NativeItemLookupRva,','NativePickupGuardQualified.store(true,std::memory_order_release)','QualifyGroundPickup(action,player,type,id)','PickupGuard::SameItemIdentity(header[0],header[2],id)','PickupGuard::GroundMode(header[3])','OriginalGetItemCode(unit)','ResolveGroundRule(rules.get(),ruleItem,resolvedRule)','matchedRule->show','return PickupGuard::Decision::Blocked','OriginalNativeActionDispatch(action,player,type,id);','NativePickupGuardQualified.store(false,std::memory_order_release)'):
    assert term in s,term
print('fail-open qualified pickup guard source contracts: ok')
