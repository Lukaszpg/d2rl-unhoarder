from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text(encoding='utf-8')
cm=(root/'CMakeLists.txt').read_text(encoding='utf-8')
rc=(root/'src/plugin.rc').read_text(encoding='utf-8')
assert 'OUTPUT_NAME "unhoarder"' in cm
assert 'OriginalFilename", "unhoarder.dll"' in rc
assert 'InternalName", "unhoarder"' in rc
assert 'GetModuleHandleW(L"loot-filter.dll")' in s
assert 'GetModuleHandleW(L"d2rl-loot-filter.dll")' in s
assert '.id = "unhoarder"' in s
assert '.id = "loot-filter"' not in s
print('unhoarder DLL/plugin identity and old-production conflict guard contract ok')
