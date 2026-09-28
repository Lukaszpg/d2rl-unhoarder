from pathlib import Path
import json
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text()
h=(root/'src/filter_rule_engine.hpp').read_text()

assert '.version = "1.0.0"' in s
assert 'config["version"].get<int>() != 3' in s
assert 'schema3-requires-exactly-one-wrapper={show:object}|{hide:object}' in s
assert 'const bool hasShow=entry.contains("show");' in s
assert 'const bool hasHide=entry.contains("hide");' in s
assert 'wrapperShow=hasShow;' in s
assert 'block=&entry[hasShow?"show":"hide"];' in s
assert 'block->contains("continue") && !(*block)["continue"].is_boolean()' in s
assert 'rule.continueEvaluation=fresh->schema==3 && block->contains("continue")' in s
assert 'schema3-flat-colors-unsupported use-tooltip-object' in s
assert 'if(block->contains("conditions")) {' in s  # optional = catch-all if absent
assert 'RuleEngine::ResolveMatchingRulesFailOpen(table->rules,item,' in s
assert 'MergeGroundRuleAction(output,rule);' in s
assert 'output.show=rule.show;' in s
for field in ('hasName','hasBackground','hasTextColor','hasDropSound','hasMinimapIcon'):
    assert f'if(rule.{field})' in s
assert 'ResolveMatchingRulesFailOpen' in h
assert 'if(!Continues(rule)) return true;' in h
assert 'if(state==MatchState::Unknown) {' in h and 'onUnknown();' in h
assert 'A matching Continue block deliberately falls through' in h

example=json.loads((root/'loot-filter.v3.example.json').read_text())
assert example['version']==3
assert all(len(rule)==1 and next(iter(rule)) in {'show','hide'} for rule in example['rules'])
cont=json.loads((root/'loot-filter.v3.continue.example.json').read_text())
assert cont['rules'][0]['show']['continue'] is True
assert list(cont['rules'][-1])==['hide'] and cont['rules'][-1]['hide']=={}
print('1.0.0 PoE-style show/hide wrappers + Continue composition source contract ok')
