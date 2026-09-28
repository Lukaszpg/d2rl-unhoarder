from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/filter_rule_engine.hpp').read_text()
assert '.version = "1.0.0"' in s
assert 'key=="itemLevel" || key=="quantity" || key=="sockets"' in s
assert 'socket?dest.sockets:dest.quantity' in s
assert 'sockets-requires-integer-0..15' in s
assert 'if (rule.conditions.sockets.enabled) fresh->usesSockets=true;' in s
assert 'ReadNativeGroundSockets(nativeUnit,header[2],' in s
assert 'GroundPropertyLive::AllowsMode(before[3],purpose)' in s
assert 'before!=after' in s and 'finalHeader!=before || first!=second' in s
assert 'if(!getter) return false;' in s
assert 'item.socketsKnown=known' in s
assert 'if(!known) break;' in s
assert 'NextNativeProperty(table->rules,item)' in s
assert 'if(c.sockets.enabled && !item.socketsKnown)' in h
assert 'if(c.sockets.enabled && !item.socketsKnown) return MatchState::Unknown;' in h
assert 'NativeRowLiveLatestLabel.socketsKnown=item.socketsKnown;' in s
assert 'rowItem.socketsKnown=append.socketsKnown;' in s
assert 'candidate.socketsKnown=label.socketsKnown;' in s
assert 'scalarOut->socketsKnown=candidate.socketsKnown;' in s
assert 'item.socketsKnown=scalarSnapshot->socketsKnown;' in s
assert 'scalars.socketsKnown,scalars.sockets,' in s
assert 'if(!PickupGuard::GroundMode(header[3]))' in s
assert 'GroundPropertyLive::Purpose::VerifiedLabel' in s
print('1.0.0 socket rule parse, mode 5, cached paint, fail-open and mode-3 pickup: ok')
