from pathlib import Path

root = Path(__file__).resolve().parents[1]
s = (root / "src/plugin.cpp").read_text(encoding="utf-8")
readme = (root / "README.md").read_text(encoding="utf-8")

start = s.index("bool ResolveFilterConfigPath() noexcept")
end = s.index("std::string PathUtf8(", start)
resolver = s[start:end]

assert "Context->pluginConfigPath" in resolver
assert "std::filesystem::path(Context->pluginConfigPath).parent_path()" in resolver
assert 'FilterConfigPath=configDirectory/L"filter.json";' in resolver
assert "GetModuleFileNameW" not in resolver
assert 'L"loot-filter.json"' not in resolver
assert 'L"loot-filter-probe.json"' not in resolver

assert r"d2rloader\config" in readme
assert "- config: `d2rloader/config/filter.json`" in readme
assert "no longer reads filter JSON files from the `plugins` directory" in readme
print("filter.json resolves from D2RLoader mod config directory")
