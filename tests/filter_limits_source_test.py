from pathlib import Path

source = (Path(__file__).parents[1] / "src" / "plugin.cpp").read_text(encoding="utf-8")

assert "constexpr std::size_t MaximumFilterFileBytes = 4 * 1024 * 1024;" in source
assert "constexpr std::size_t MaximumFilterRules = 4096;" in source
assert "over-4MiB" in source
assert "over-4096-rules" in source
assert "over-64KiB" not in source
assert "over-256-rules" not in source
print("filter limits source test: OK")
