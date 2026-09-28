from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'src/plugin.cpp').read_text()
decl='void CodeText(std::uint32_t code, char (&out)[5]) noexcept;'
definition='void CodeText(std::uint32_t code, char (&out)[5]) noexcept {'
assert s.count(decl)==1
assert s.count(definition)==1
assert s.index(decl)<s.index('void DrainGroundPropertyEvidence() noexcept')
assert s.index(decl)<s.index('void DrainGroundPropertySdkEvidence() noexcept')
assert s.index('void DrainGroundPropertySdkEvidence() noexcept')<s.index(definition)
assert s.count('CodeText(sample.code,codeText);')==1
assert s.count('CodeText(sdk.code,codeText);')==1
print('MSVC CodeText declaration precedes both worker drainers: ok')
