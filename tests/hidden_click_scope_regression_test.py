from pathlib import Path
s=(Path(__file__).parents[1]/'src'/'plugin.cpp').read_text()
body=s[s.index('void __fastcall HookSharedLabelPaint('):s.index('// 0.1.42 crash diagnosis:', s.index('void __fastcall HookSharedLabelPaint('))]
assert 'std::uint32_t concealedRecordId{};' in body
assert 'std::uint32_t concealedVerifiedCode{};' in body
assert 'concealedRecordId=recordId;' in body
assert 'concealedVerifiedCode=verifiedCode;' in body
assert 'HiddenGroundLastPainterSkipId.store(concealedRecordId' in body
assert 'HiddenGroundLastPainterSkipCode.store(concealedVerifiedCode' in body
assert 'HiddenGroundLastPainterSkipId.store(recordId' not in body
assert 'HiddenGroundLastPainterSkipCode.store(verifiedCode' not in body
assert '.version = "1.0.0"' in s
print('1.0.0 hidden painter audit identifier scope regression: ok')
