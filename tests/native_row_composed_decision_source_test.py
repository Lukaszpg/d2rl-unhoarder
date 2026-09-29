from pathlib import Path

s = Path("src/plugin.cpp").read_text(encoding="utf-8")
assert '.version = UNHOARDER_VERSION_STRING' in s
assert 'std::uint64_t appendSeq,const GroundRuleDecision& rule) noexcept {' in s
assert 'std::uint64_t appendSeq,const FilterNameRule& rule) noexcept {' not in s
assert 'BeginNativeRowFontDraw(append.unitId,append.code,' in s
assert 'append.appendSequence,*rule);' in s
assert 'GroundRuleDecision resolvedRule{};' in s
assert '&resolvedRule : nullptr;' in s
print('1.0.0 native row font draw consumes composed GroundRuleDecision: ok')
