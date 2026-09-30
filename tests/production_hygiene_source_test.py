from pathlib import Path

root = Path(__file__).resolve().parents[1]
plugin = (root / "src/plugin.cpp").read_text(encoding="utf-8")
rules = (root / "src/filter_rule_engine.hpp").read_text(encoding="utf-8")
live = (root / "src/ground_property_live_policy.hpp").read_text(encoding="utf-8")
renderer = (root / "src/minimap_overlay_renderer.cpp").read_text(encoding="utf-8")
renderer_h = (root / "src/minimap_overlay_renderer.hpp").read_text(encoding="utf-8")
workflow = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
readme = (root / "README.md").read_text(encoding="utf-8")

# Logging severity is explicit; message text is not an allowlist/router.
assert "void Emit(" not in plugin
assert "void LogInfo(const char* message)" in plugin
assert "void LogWarn(const char* message)" in plugin
assert "Context->LogInfo(message);" in plugin
assert "Context->LogWarn(message);" in plugin
assert "enum class LogLevel" in renderer_h
assert "LogLevel::Warning" in renderer
assert "LogLevel::Info" in renderer
assert "LOOT_FORMATTER_REFUSED" in plugin and 'LogWarn("LOOT_FORMATTER_REFUSED' in plugin
assert "LOOT_GEOMETRY_REFUSED" in plugin and 'LogWarn("LOOT_GEOMETRY_REFUSED' in plugin
assert "LOOT_RULES_WARNING" in plugin and 'LogWarn("LOOT_RULES_WARNING' in plugin
assert "LOOT_SOUND_PICKUP_OBSERVER_UNAVAILABLE" in plugin
assert 'LogWarn("LOOT_SOUND_PICKUP_OBSERVER_UNAVAILABLE' in plugin

# Dead development helpers and unreachable manual sound branches stay gone.
assert "bool MatchesCode(" not in plugin
assert "UsesNativeQualityLevel" not in rules
assert "constexpr bool Ready(" not in live
assert "manual-test" not in plugin
assert "request->automatic" not in plugin
assert "QualifyNamedSoundBackend(bool" not in plugin
assert "if(verbose)" not in plugin
assert "LOOT_COMPAT_SOUND_GUARD" not in plugin
assert "schema2" not in plugin
assert "schema2" not in rules
assert "usesConditions" in plugin
assert "usesConditions" in rules
assert "std::atomic<std::shared_ptr<const FilterRuleTable>> PublishedFilterRules" in plugin
assert "std::atomic_load_explicit(&PublishedFilterRules" not in plugin
assert "std::atomic_store_explicit(&PublishedFilterRules" not in plugin
assert "std::atomic_load_explicit(&TooltipPaintSubscribers" not in plugin
assert "std::atomic_store_explicit(&TooltipPaintSubscribers" not in plugin
assert "std::atomic_load_explicit(&TooltipGlyphSubscribers" not in plugin
assert "std::atomic_store_explicit(&TooltipGlyphSubscribers" not in plugin

# CI treats first-party UnHoarder warnings as errors; ImGui remains explicitly exempt in CMake.
assert "-DUNHOARDER_WARNINGS_AS_ERRORS=ON" in workflow
assert "target_compile_options(unhoarder_imgui PRIVATE /W3 /WX-)" in (root / "CMakeLists.txt").read_text(encoding="utf-8")

# Documentation uses normal Windows path notation.
assert r"<D2R_installation_directory>\mods\<your_mod_name>\d2rloader\config" in readme
assert r"<D2R_installation_directory>\\mods\\<your_mod_name>\\d2rloader\\config" not in readme

# Regression output must not pretend the current suite is tied to a historical product version.
for test in (root / "tests").glob("*_test.py"):
    if test.name == "production_hygiene_source_test.py":
        continue
    assert "1.0.0" not in test.read_text(encoding="utf-8"), test.name

print("production hygiene contract ok")
