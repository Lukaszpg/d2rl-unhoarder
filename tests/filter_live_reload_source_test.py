from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/filter_live_reload.hpp').read_text()
assert '.version = "1.0.0"' in s
assert '#include "filter_live_reload.hpp"' in s
assert 'GetAsyncKeyState(VK_F9)' in s
assert 'lastReloadChord=reloadChord;' in s
assert 'ruleFileWatcher.Observe(ReadFilterFileStamp(FilterConfigPath),' in s
assert 'ruleFileWatcher.Resync(ReadFilterFileStamp(FilterConfigPath));' in s
assert 'if (!ReloadFilterRules()) {' in s
assert 'LOOT_RELOAD_REFUSED trigger=%s reason=invalid-new-json-or-excel' in s
assert 'LOOT_RELOAD_OK version=1.0.0' in s
assert 'NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);' in s
assert 'GroundIdentities.fill({}); // no old styled-name/paint identity survives' in s
assert 'LOOT_RELOAD_SOUND retained-seen-ids=1' in s
assert 'SoundRegistryEpoch.fetch_add(1,std::memory_order_acq_rel);' in s
assert '!FilterLiveReloadAvailable.load(std::memory_order_acquire)' in s
assert 'FilterLiveReloadAvailable.store(true,std::memory_order_release)' in s
assert 'last-valid-rules-preserved=1' in s
assert 'if (!seeded_) {Resync(stamp);return false;}' in h
assert 'now-lastChanged_<settleMs' in h
assert 'unsettled_=false;' in h
# No native hook I/O, caller authorization or time-limited pickup suppression.
start=s.index('NativePickupDecision QualifyGroundPickup(')
end=s.index('void RecordPickupDecision(',start)
guard=s[start:end]
assert 'FilterConfigPath' not in guard
assert 'NativeActionPhase' not in guard
assert 'NativePickupGuardCandidates.fetch_add' in guard
print('1.0.0 live reload worker and pickup regression contracts: ok')
