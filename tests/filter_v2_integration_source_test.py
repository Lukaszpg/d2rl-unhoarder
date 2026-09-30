from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/filter_rule_engine.hpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
assert 'config["version"].get<int>() != 2' in s
assert 'ParseV2Conditions((*block)["conditions"],rule.conditions,' in s
assert 'std::atomic_store_explicit(&PublishedFilterRules,published,' in s
assert 'ResolveGroundRule(' in s and 'RuleEngine::ResolveMatchingRulesFailOpen' in s
assert 'if (rule.schema2 ? rule.conditions.Matches(item)' in h
assert 'seen.insert(codeValue)' in s and 'fresh->schema==1' in s
assert 'rule.show=fresh->schema==3 ? wrapperShow :' in s
assert 'if (rule.conditions.quantity.enabled) fresh->usesQuantity=true;' in s
assert 'unsupported-ground-condition:' in s
assert 'GroundRuleItem(code,unit,rules.get(),id)' in s
assert 'CachedGroundRuleItem(verifiedCode,verifiedClassId,' in s
assert 'ObserveGroundSoundIdentity(header[2],OriginalGetItemCode(unit),unit,header[1]);' in s
assert 'ResolveGroundRule(snapshot.get(),' in s
guard=s[s.index('PickupGuard::Decision QualifyGroundPickup('):s.index('void __fastcall HookNativeActionDispatch(')]
assert 'ResolveGroundRule(rules.get(),ruleItem,resolvedRule)' in guard
assert 'NativeActionPhase' not in guard
assert 'RecordPickupDecision' not in s
print('v1/v2 migration + v3 composite shared ground and pickup integration: ok')
