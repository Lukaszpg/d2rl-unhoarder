from pathlib import Path

root = Path(__file__).resolve().parents[1]
plugin = (root / "src/plugin.cpp").read_text(encoding="utf-8")
renderer = (root / "src/minimap_overlay_renderer.cpp").read_text(encoding="utf-8")
renderer_h = (root / "src/minimap_overlay_renderer.hpp").read_text(encoding="utf-8")
readme = (root / "README.md").read_text(encoding="utf-8")

orphaned = [
    "src/automap_projection_probe_policy.hpp",
    "src/ground_property_candidate_compare.hpp",
    "src/hidden_pickup_trace_policy.hpp",
    "src/minimap_world_position_probe_policy.hpp",
    "src/native_action_trace_policy.hpp",
    "src/world_probe_evidence.hpp",
    "src/world_probe_label_stack.hpp",
    "src/world_probe_upstream_calls.hpp",
    "tests/automap_projection_probe_policy_test.cpp",
    "tests/minimap_world_position_probe_policy_test.cpp",
]
for path in orphaned:
    assert not (root / path).exists(), path

dead_runtime_counters = [
    "FormatterCalls",
    "GeometryTotalCalls", "GeometrySourceCalls", "GeometryValidPairs",
    "GeometryIdMismatches", "GeometryLookups", "GeometryNoRule",
    "GeometryReadFailures", "GeometryNoTerminator", "GeometryWrites",
    "GeometryObserved", "FilterLookups", "FilterMatches", "FilterNoRule",
    "FilterWrites", "FilterGuardFailures", "FilterReloadSucceeded",
    "FilterReloadRefused", "GroundPropertyLiveReads", "GroundPropertyLiveUnknown",
    "GroundEtherealRuleReads", "GroundEtherealRuleUnknown", "GroundEtherealMode5Reads",
    "GroundIdentifiedRuleReads", "GroundIdentifiedRuleUnknown",
    "GroundIdentifiedMode5Reads", "GroundSocketRuleReads",
    "GroundSocketRuleUnknown", "GroundSocketMode5Reads", "SoundPickupPolls",
    "SoundPickupPollSuccess", "SoundPickupPollRejected", "SoundPickupResets",
    "SoundPickupBusy", "SoundSeenTotal", "SoundBaselineTotal",
    "SoundQualifiedNew", "SoundQueued", "SoundQueueRejected", "SoundPlayed",
    "SoundUnknown", "SoundCancelled", "SoundCacheContention", "SoundCacheFull",
    "SoundAlreadySeen", "SoundObservedAlt", "NativeActionDispatchInstalled",
]
for name in dead_runtime_counters:
    assert name not in plugin, name

for stale in (
    "LOOT_CONFIG_LEGACY_PATH",
    "LOOT_NATIVE_ACTION_REFUSED",
    "LOOT_NATIVE_ACTION_PARTIAL",
    "LOOT_NATIVE_ACTION_UNAVAILABLE",
    'GetModuleHandleW(L"loot-filter-probe.dll")',
    'GetModuleHandleW(L"d2rl-loot-filter-probe.dll")',
    "sourceMarker=unhoarder-prod-v1",
):
    assert stale not in plugin, stale

for stale in (
    "PublishedFrameCount", "DrawnMarkers", "AutomapVisibilitySuppressions",
    "PresentCalls", "DirectQueueCaptures", "RendererInitAttempts",
    "RendererInitFailures", "RenderedFrames", "LastInitFailureStage",
    "GetDiagnostics",
):
    assert stale not in renderer, stale
assert "struct Diagnostics" not in renderer_h
assert "GetDiagnostics" not in renderer_h

old_examples = list(root.glob("loot-filter.v3*.example.json"))
new_examples = list(root.glob("unhoarder.v3*.example.json"))
assert not old_examples
assert len(new_examples) == 8
assert "unhoarder.v3.example.json" in readme
assert "loot-filter.v3.example.json" not in readme

print("final production cleanup contract ok")
