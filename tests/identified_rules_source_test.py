from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
r=(p/'src/filter_rule_engine.hpp').read_text()
h=(p/'src/ground_identified_policy.hpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
assert 'NativeIdentifiedMask=0x00000010U' in h
assert 'GroundIdentified::FromNativeFlags(' in s
assert 'key=="identified"' in s
assert 'identified-requires-boolean-true-or-false' in s
assert 'dest.identifiedEnabled=true;' in s
assert 'dest.identifiedExpected=it.value().get<bool>();' in s
assert 'rule.conditions.identifiedEnabled) fresh->usesIdentified=true;' in s
assert 'NextProperty::Identified' in s and 'NextProperty::Identified' in r
assert 'if(!fields.identifiedKnown)' in s
assert 'GroundPropertyReader::ReadLe32(itemData.data()+0x18)' in s
assert 'GroundPropertyReader::ReadLe32(check.data()+0x18)' in s
assert 'if((includeEthereal || includeIdentified) &&' in s
assert 'if(includeIdentified && scalars.qualityKnown && scalars.itemLevelKnown)' in s
assert 'for(unsigned propertyGroup=0;propertyGroup<4 && table;++propertyGroup)' in s
assert 'GroundPropertyLive::Purpose::VerifiedLabel' in s
assert 'if(!PickupGuard::GroundMode(header[3]))' in s
assert 'scalarOut->identifiedKnown=candidate.identifiedKnown;' in s
assert 'item.identifiedKnown=scalarSnapshot->identifiedKnown;' in s
assert 'scalars.identifiedKnown,scalars.identified};' in s
assert 'if(c.identifiedEnabled && !item.identifiedKnown)' in r
assert 'if(c.identifiedEnabled && !item.identifiedKnown) return MatchState::Unknown;' in r
assert 'item.identified!=c.identifiedExpected) return MatchState::NoMatch;' in r
print('1.0.0 identified JSON native mode5 lazy fail-open and pickup propagation: ok')
