from pathlib import Path
base=Path(__file__).resolve().parents[1]
s=(base/'src/plugin.cpp').read_text()
h=(base/'src/filter_rule_engine.hpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
assert 'RuleEngine::NeedsNativeQualityLevel(table->rules,item)' in s
assert 'ResolveMatchingRulesFailOpen' in h
assert 'ReadNativeGroundQualityLevel(' in s
assert 'if((table->usesQuality || table->usesItemLevel) &&' in s
assert s.count('const auto scalars=GroundRuleItem(code,unit,table.get(),recordId,') == 1
assert 'GroundPropertyLive::Purpose::VerifiedLabel' in s
assert 'if(CheapConditionsCannotMatch(c,item)) continue;' in h
assert 'if(c.Matches(item) && !Continues(rule)) return false;' in h
assert 'return MatchState::Unknown;' in h
assert 'A matching Continue block deliberately falls through' in h
print('property reads are ordered, Continue-aware, lazy and fail-open')
