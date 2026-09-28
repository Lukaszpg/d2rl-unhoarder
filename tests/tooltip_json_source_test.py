from pathlib import Path
import json
root=Path(__file__).resolve().parents[1]
p=(root/'src/plugin.cpp').read_text()

assert '.version = "1.0.0"' in p
assert 'block->contains("tooltip") && !(*block)["tooltip"].is_object()' in p
assert 'tooltip-cannot-be-mixed-with-flat-backgroundColor-or-textColor' in p
assert 'invalid-tooltip expected={backgroundColor?:RGBA(...),textColor?:RGBA(...)}' in p
assert 'key.key()!="backgroundColor" && key.key()!="textColor"' in p
assert 'const auto* tooltip = block->contains("tooltip")' in p
assert 'tooltip->contains("backgroundColor")' in p
assert 'tooltip->contains("textColor")' in p
assert 'invalid-tooltip-backgroundColor' in p
assert 'invalid-tooltip-textColor' in p
assert 'requires-code/conditions-and-action(show|name|tooltip|dropSound|minimapIcon)' in p
assert 'tooltip{backgroundColor,textColor}+RGBA(r,g,b,a)' in p
# Migration aliases intentionally remain so upgrading does not invalidate an
# existing filter; mixing old and new forms is refused above.
assert 'block->contains("backgroundColor") && !(*block)["backgroundColor"].is_string()' in p
assert 'block->contains("textColor") && !(*block)["textColor"].is_string()' in p

examples=list(root.glob('loot-filter.v3*.example.json'))
assert examples
found_tooltip=False
for path in examples:
    doc=json.loads(path.read_text())
    for outer in doc.get('rules', []):
        assert len(outer)==1
        rule=next(iter(outer.values()))
        assert 'backgroundColor' not in rule
        assert 'textColor' not in rule
        if 'tooltip' in rule:
            found_tooltip=True
            tooltip=rule['tooltip']
            assert isinstance(tooltip, dict) and tooltip
            assert set(tooltip) <= {'backgroundColor','textColor'}
assert found_tooltip
v3=json.loads((root/'loot-filter.v3.example.json').read_text())
assert v3['version']==3
for outer in v3['rules']:
    assert len(outer)==1 and next(iter(outer)) in {'show','hide'}
    body=next(iter(outer.values()))
    assert 'backgroundColor' not in body and 'textColor' not in body
print('1.0.0 JSON tooltip grouping + v3 wrappers + legacy migration aliases source contract ok')
