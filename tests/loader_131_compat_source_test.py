from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
checks={
 'plugin version':'.version = "1.0.0"' in s,
 'sound loader':'LOOT_COMPAT_SOUND_LOADER version=1.0.0' in s,
 'quantity bridge':'LOOT_COMPAT_QUANTITY_BRIDGE' in s,
 'exact loader 131 bridge':'GroundQuantityBridgeLoader131' in s and '0xFF,0x25,0xF2,0x51,0xB3,0x03,0x90,0x90,0x90,0x90' in s,
 'exact slot qualification':'else if (entry==GroundQuantityBridgeLoader131) requiredSlot=base+0x3E2A218;' in s,
 'native owner gates remain':'owner!=GetModuleHandleW(L"D2RCore.dll")' in s and 'owner!=GetModuleHandleW(L"d2rl-soe.dll")' in s,
 'paired audio identity recognition':'SoundLoaderIdentity::Classify(' in s and 'SoundLoaderIdentity::Layout::Unknown' in s,
 'original audio witnesses retained':'{0x1FCB26U,{0x48,0x8D,0x0D,0xA3,0x03,0x00,0x00}}' in s and '{0x1A0C43U,{0xE8,0x38,0xFB,0xFF,0xFF}}' in s,
 'audio call chain targets':'{0x1FCFE3U,0x1A0C00U}' in s and '{0x1A0C18U,0xD9760U}' in s and '{0x1A0C43U,0x1A0780U}' in s,
 'sound loader fingerprint fail closed':'LOOT_SOUND_QUALIFY refused=native-fingerprint-mismatch' in s and 'SoundLoaderBase.store(base,std::memory_order_release);' in s,
 'quantity fail closed':'reader-bridge-fingerprint-mismatch fallback=vanilla' in s,
 'never promote unknown target':'newBridge=unqualified statCalls=disabled' in s,
}
for k,v in checks.items(): print(('PASS ' if v else 'FAIL ')+k)
assert all(checks.values())
print(f'{len(checks)} loader compatibility source checks passed')
