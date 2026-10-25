from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
for term in ('.version = UNHOARDER_VERSION_STRING','LOOT_PICKUP_GUARD_READY version=" UNHOARDER_VERSION_STRING "','Context->CheckExpectedBytes(NativeItemLookupRva,','NativePickupGuardQualified.store(true,std::memory_order_release)','QualifyGroundPickup(action,player,type,id)','PickupGuard::SameItemIdentity(header[0],header[2],id)','PickupGuard::GroundMode(header[3])','LookupItemCode(header[1])','ResolveGroundRule(rules.get(),ruleItem,resolvedRule)','matchedRule->show','return PickupGuard::Decision::Blocked','OriginalNativeActionDispatch(action,player,type,id);','NativePickupGuardQualified.store(false,std::memory_order_release)'):
    assert term in s,term
assert 'TimeDateStamp!=0x6AB3782C' not in s
assert 'nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64' in s
assert 'nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC' in s
assert 'Context->CheckExpectedBytes(NativeActionDispatchRva,' in s
assert 'dispatchWitnesses' in s
assert 'reason=PE-header-or-image-size-invalid forward-only=1' in s
print('fail-open qualified pickup guard source contracts: ok')
