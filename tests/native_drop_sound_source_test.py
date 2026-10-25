from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'src/plugin.cpp').read_text()
for start,end in [('void __cdecl OnCompatibleInWorldLabel(', 'bool __cdecl OnCompatibleInWorldStyle('),
                  ('std::uint8_t __fastcall HookLabelFormatter(', 'void ArmLabelFormatter() noexcept {')]:
    body=s[s.index(start):s.index(end,s.index(start))]
    assert 'TryReplaceNativeDropSound' not in body and 'QueueNamedSound' not in body
assert 'ExamineGroundSoundCandidate(' not in s
assert 'trigger=first-ground-label' not in s
assert 'player(selected.dropSound.data(),unit,0,0)' in s
assert 'return result!=nullptr;' in s
assert '[](void* item) noexcept {OriginalNativeDropPlayer(item);}' in s
assert 'original(unit);' in (Path(__file__).resolve().parents[1]/'src/native_drop_sound.hpp').read_text()
assert 'NativeDropSubscribers.Dispatch(' in s
assert '!QualifyNamedSoundBackend() || !ArmNativeDropSoundHook()' in s
assert 'no-label-fallback=1' in s
assert 'QueueNamedSound(' not in s and 'struct SoundRequest' not in s
print('PASS audio is exclusive to the qualified native event; no label replay or native-pointer queue.')
