from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text()

assert '(block->contains("ruleName") && !(*block)["ruleName"].is_string())' in s
assert 'key.key()!="ruleName" && key.key()!="conditions"' in s
assert 'fields={ruleName?,conditions?,continue?,name?,tooltip?,dropSound?,minimapIcon?}' in s
assert 'syntax=schema3:{show|hide:{ruleName?,conditions?,continue?,name?,tooltip?,dropSound?,minimapIcon?}}' in s
# ruleName is metadata only: it is accepted but never copied into FilterNameRule/runtime decisions.
struct=(root/'src/plugin.cpp').read_text().split('struct FilterNameRule',1)[1].split('};',1)[0]
assert 'ruleName' not in struct
print('1.0.0 ruleName metadata source contract ok')
