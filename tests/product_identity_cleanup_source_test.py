from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text(); cm=(root/'CMakeLists.txt').read_text(); rc=(root/'src/plugin.rc').read_text(); readme=(root/'README.md').read_text()
assert '.id = "loot-filter"' in s and '.name = "Loot Filter"' in s
assert '.author = "MindH1ve"' in s
assert 'OUTPUT_NAME "loot-filter"' in cm and 'add_library(loot_filter SHARED' in cm
assert 'OriginalFilename", "loot-filter.dll"' in rc and 'CompanyName", "MindH1ve"' in rc
assert 'L"filter.json"' in s
assert 'L"loot-filter.json"' in s  # v1.0.0 migration fallback only
assert 'L"loot-filter-probe.json"' in s  # probe-era migration fallback only
assert 'GetModuleHandleW(L"loot-filter-probe.dll")' in s
worker=s[s.index('void RuntimeWorkerLoop('):s.index('void RuntimeWorkerStart(',s.index('void RuntimeWorkerLoop('))]
assert 'VK_F9' in worker
for key in ('VK_F6','VK_F7','VK_F8','VK_F10','VK_F11','VK_F12'):
    assert key not in worker,key
for prefix in ('LOOT_WORLD_PROBE','LOOT_LATENCY_','LOOT_SOCKET_PROBE_','LOOT_ETHEREAL_PROBE_','LOOT_IDENTIFIED_PROBE_','LOOT_IMAGE_DUMP_','LOOT_PICKUP_TRACE_'):
    assert f'std::strstr(message,"{prefix}")' not in s,prefix
assert 'loot-filter-probe.dll' not in rc
assert 'Legacy reverse-engineering capture hotkeys' in readme
print('1.0.0 product identity, author and diagnostic cleanup contract ok')
