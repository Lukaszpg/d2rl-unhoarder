from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/ground_property_live_policy.hpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
assert 'enum class Purpose : std::uint8_t { StrictGround, VerifiedLabel };' in h
assert 'mode==GroundMode ||' in h
assert 'purpose==Purpose::VerifiedLabel && mode==PresentingMode' in h
assert 'GroundPropertyLive::AllowsMode(mode,purpose)' in s
assert 'GroundPropertyLive::AllowsMode(header[3],purpose)' in s
assert 'before!=after' in s
assert 'GroundRuleItem(code,unit,rules.get(),id)' in s
assert 'if(!PickupGuard::GroundMode(header[3]))' in s
assert s.count('GroundPropertyLive::Purpose::VerifiedLabel')>=5
assert 'LOOT_LATENCY_STAGE' not in s
print('1.0.0 mode-5 presentation-only gate: ok')
