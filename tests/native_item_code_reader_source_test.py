from pathlib import Path
root=Path(__file__).resolve().parents[1]
plugin=(root/'src/plugin.cpp').read_text()
start=plugin.index('constexpr std::uintptr_t GetItemCodeRva = 0x36EF50;')
assert 'ExpectedGetItemCode' in plugin[start:start+400]
assert 'InstallInlineHook(GetItemCodeRva' not in plugin
assert 'HookGetItemCode(' not in plugin
assert 'OriginalGetItemCode' not in plugin
assert 'NativeGetItemCode = reinterpret_cast<GetItemCodeFn>(Base + GetItemCodeRva);' in plugin
assert 'CheckExpectedBytes(GetItemCodeRva, ExpectedGetItemCode.data(),' in plugin
assert 'ItemCodeReaderReady.store(true, std::memory_order_release);' in plugin
assert 'LOOT_FILTER_INACTIVE native-item-code-reader-owned-or-fingerprint-mismatch' in plugin
assert 'NativeGetItemCode = nullptr;' in plugin
assert 'NativeGetItemCode(unit)' in plugin
assert 'NativeGetItemCode(mutableUnit)' in plugin
assert 'LOOT_ITEM_CODE_READER_READY mode=direct-verified-native-call' in plugin
print('Native GetItemCode is called only through a fingerprint-qualified direct reader; no 0x36EF50 detour.')
