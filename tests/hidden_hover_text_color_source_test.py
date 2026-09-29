from pathlib import Path
src = (Path(__file__).parents[1] / "src" / "plugin.cpp").read_text(encoding="utf-8")
policy = (Path(__file__).parents[1] / "src" / "native_row_font_color_policy.hpp").read_text(encoding="utf-8")
assert "!rules->backgroundRules && !rules->textColorRules" in src
assert "if (!rule->hasBackground && !rule->hasTextColor) return false;" in src
assert "const bool changeBackground=rule->hasBackground;" in src
assert "BeginNativeRowFontDraw(append.unitId,append.code" in src
assert "NativeRowFontColorPolicy::EligibleGroundLabel(native)" in src
assert "fabs(rgba[i]-0.941f)" not in policy
assert "rgba[i]<0.f || rgba[i]>1.001f" in policy
print("hidden hover text color source contract: OK")
