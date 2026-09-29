from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/ground_property_live_policy.hpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
assert '#include "ground_property_live_policy.hpp"' in s
assert 'RuleEngine::NeedsNativeQualityLevel(table->rules,item)' in s
assert 'RuleEngine::ResolveMatchingRulesFailOpen(table->rules,item,' in s
assert 'GroundPropertyLive::Validate(quality,level)' in s
assert 'GroundCandidatePageReadable(address,length)' in s
assert 'ReadProcessMemory(GetCurrentProcess(),nativeUnit,after.data(),' in s
assert 'before!=after' in s
assert 'GroundPropertyLiveReads.fetch_add' in s
assert 'if (!rule.conditions.qualities.empty()) fresh->usesQuality=true;' in s
assert 'if (rule.conditions.itemLevel.enabled) fresh->usesItemLevel=true;' in s
assert 'if(key=="rarity")' in s
assert 'key=="itemLevel" || key=="quantity"' in s
assert 'item.itemLevelKnown=fields.itemLevelKnown;' in s
assert 'NativeRowLiveLatestLabel.qualityKnown=item.qualityKnown;' in s
assert 'candidate.qualityKnown=label.qualityKnown;' in s
assert 'rowItem.itemLevelKnown=append.itemLevelKnown;' in s
assert 'scalarOut->qualityKnown=candidate.qualityKnown;' in s
assert '&verifiedProperties);' in s
assert s.count('verifiedQuantity,snapshot.get(),&verifiedProperties)')==1
assert 'CachedGroundRuleItem(verifiedCode,verifiedClassId,' in s
assert 'GroundRuleItem(code,unit,rules.get(),id)' in s
resolver=s[s.index('bool ResolveGroundRule('):s.index('// Backend selection occurs after plugin startup')]
assert 'RuleEngine::ResolveMatchingRulesFailOpen(table->rules,item,' in resolver
assert 'MergeGroundRuleAction(output,rule);' in resolver
assert 'key=="ethereal"' in s[s.index('bool ParseV2Conditions('):s.index('// RGBA is a JSON STRING:')]
assert 'key=="identified"' in s[s.index('bool ParseV2Conditions('):s.index('// RGBA is a JSON STRING:')]
assert 'key=="sockets"' in s[s.index('bool ParseV2Conditions('):s.index('// RGBA is a JSON STRING:')]
assert 'q?rawQuality:0U' in h and 'l?rawLevel:0U' in h
print('1.0.0 quality/ilvl rule path, composition and fail-open bridges: ok')
