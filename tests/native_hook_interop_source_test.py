from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text()
n=(root/'src/native_hook_subscribers.hpp').read_text()
assert 'GetModuleHandleW(L"d2rl-ruffneckk-mapsense.dll")' not in s
assert 'DiagnoseTrackedTooltipOwner(AutomapRenderUnitRva' in s
assert 'DiagnoseTrackedTooltipOwner(NativeHookCompat::DropAudioRva' in s
assert 'NativeHookCompat::Valid(lease.Get())' in s
assert 'TryAttachForeignAutomap(' in s and 'TryAttachForeignDropAudio(' in s
assert 'DetachForeignNativeHookCompat();' in s
assert 'PublishedNativeHookService' in s and 'PublishNativeHookService();' in s
assert 'NativeDropSubscribers.Dispatch(' in s and 'NativeAutomapSubscribers.Dispatch(' in s
cache=s[s.index('void ObserveAutomapGroundUnit(const void* unit) noexcept {'):s.index('void RuntimeWorkerStart() noexcept;')]
assert cache.count('AutomapNegativeCache[negSlot]')==2
assert cache.count('MinimapProjectionItemMutex.try_lock()')>=2
assert 'subscribers_.load(std::memory_order_acquire)==0' in n
assert 'idle_.wait(' in n and 'executing_' in n
print('Ownership-gated native hook service, safe automap cache, and listener quiescence sources present.')
