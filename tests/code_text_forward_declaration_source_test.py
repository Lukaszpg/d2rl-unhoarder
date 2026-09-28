from pathlib import Path
s=(Path(__file__).resolve().parents[1]/"src/plugin.cpp").read_text()
decl="void CodeText(std::uint32_t code, char (&out)[5]) noexcept;"
definition="void CodeText(std::uint32_t code, char (&out)[5]) noexcept {"
assert s.count(decl)==1
assert s.count(definition)==1
assert s.index(decl)<s.index(definition)
assert "DrainGroundPropertyEvidence" not in s
assert "DrainGroundPropertySdkEvidence" not in s
assert s.index(definition)<s.index("CodeText(request->code,codeText);")
print("MSVC CodeText declaration precedes production uses; evidence drainers removed: ok")
