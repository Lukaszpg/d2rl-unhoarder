from pathlib import Path

root = Path(__file__).resolve().parents[1]
plugin = (root / "src/plugin.cpp").read_text(encoding="utf-8")
rc = (root / "src/plugin.rc").read_text(encoding="utf-8")

# Official D2RLoader/PluginSDK 0.3.0 source names.
assert "D2RL_PLUGIN_ABI_VERSION" in plugin
assert ".abiVersion = D2RL_PLUGIN_ABI_VERSION" in plugin
assert "context->abiVersion != D2RL_PLUGIN_ABI_VERSION" in plugin
assert "const D2RL::LifecycleService*" in plugin
assert "const D2RL::ThreadService*" in plugin
assert "const D2RL::InventoryService*" in plugin
assert "const D2RL::ItemService*" in plugin
assert "HasLifecycleServiceField" in plugin
assert "HasThreadServiceField" in plugin
assert "HasInventoryServiceField" in plugin
assert "HasItemServiceField" in plugin
assert "Context->QueryService(&service)" in plugin
assert "Context->QueryService(&inventory)" in plugin
assert "Context->QueryService(&items)" in plugin
assert "context->QueryService(&soundThreads)" in plugin

# Removed SDK 0.1-era source names must not creep back in.
for stale in (
    "D2RL_PLUGIN_API_VERSION",
    ".apiVersion =",
    "context->apiVersion",
    "ServiceV1",
    "ServiceV1Version",
    "ServiceV1RequiredSize",
):
    assert stale not in plugin, stale

assert "D2RL_PLUGIN_RESOURCE_DWORD(D2RL_PLUGIN_ABI_VERSION)" in rc
assert "D2RL_PLUGIN_API_VERSION" not in rc
print("official PluginSDK 0.3.0 source-name migration contract ok")
