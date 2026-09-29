from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
r=(p/'src/filter_rule_engine.hpp').read_text()
mask=(p/'src/ground_ethereal_policy.hpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
assert 'key=="ethereal"' in s
assert 'ethereal-requires-boolean-true-or-false' in s
assert 'it.value().is_boolean()' in s
assert 'dest.etherealExpected=it.value().get<bool>();' in s
assert 'if (rule.conditions.etherealEnabled) fresh->usesEthereal=true;' in s
assert 'NativeEtherealMask = 0x00400000U' in mask
assert 'GroundEthereal::FromNativeFlags(' in s
assert 'GroundPropertyReader::ReadLe32(itemData.data()+0x18)' in s
assert 'GroundPropertyReader::ReadLe32(check.data()+0x18)' in s
assert 'if(includeEthereal &&' in s
assert 'before!=after' in s
assert 'GroundPropertyLive::AllowsMode(mode,purpose)' in s
assert 'if(next==RuleEngine::NextProperty::Ethereal)' in s
assert 'if(!fields.etherealKnown)' in s
assert 'item.etherealKnown=true;' in s
assert 'item.ethereal=fields.ethereal;' in s
assert 'for(unsigned propertyGroup=0;propertyGroup<4 && table;++propertyGroup)' in s
assert 'if(c.etherealEnabled && !item.etherealKnown)' in r
assert 'if(c.etherealEnabled && !item.etherealKnown) return MatchState::Unknown;' in r
assert 'item.ethereal!=c.etherealExpected) return MatchState::NoMatch;' in r
assert 'NativeRowLiveLatestLabel.etherealKnown=item.etherealKnown;' in s
assert 'candidate.etherealKnown=label.etherealKnown;' in s
assert 'rowItem.etherealKnown=append.etherealKnown;' in s
assert 'scalarOut->etherealKnown=candidate.etherealKnown;' in s
assert 'item.etherealKnown=scalarSnapshot->etherealKnown;' in s
assert 'scalars.etherealKnown,scalars.ethereal,' in s
assert 'if(!PickupGuard::GroundMode(header[3]))' in s
assert 'GroundPropertyLive::Purpose::VerifiedLabel' in s
assert 'GroundPropertyLive::Purpose::StrictGround' in s
print('1.0.0 ethereal JSON/guarded native mask/mode-5 propagation/fail-open/pickup: ok')
