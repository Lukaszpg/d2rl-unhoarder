from pathlib import Path

root = Path(__file__).resolve().parents[1]
s = (root / "src/plugin.cpp").read_text()
readme = (root / "README.md").read_text()

canonical = 'const auto canonical=directory/L"filter.json";'
previous = 'const auto previousProduction=directory/L"loot-filter.json";'
probe = 'const auto legacyProbe=directory/L"loot-filter-probe.json";'
assert canonical in s
assert previous in s
assert probe in s
assert s.index(canonical) < s.index(previous) < s.index(probe)
assert 'using=loot-filter.json rename-to=filter.json' in s
assert 'using=loot-filter-probe.json rename-to=filter.json' in s
assert '- config: `filter.json`' in readme
assert 'New configurations should use `filter.json`.' in readme
print('1.0.0 filter.json canonical path and migration fallbacks contract ok')
