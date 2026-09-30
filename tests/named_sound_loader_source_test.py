from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/named_sound_loader_identity.hpp').read_text()
checks={
 'plugin 1.0.0':'.version = UNHOARDER_VERSION_STRING' in s,
 'old and new paired PE IDs':'0x6AAFC972U && imageSize==0x5602000U' in h and '0x6AB3782CU && imageSize==0x5643000U' in h,
 'old sound chain preserved':'0x1FCFE3U,{0xE8,0x18,0x3C,0xFA,0xFF}' in s and '0x1A0C43U,{0xE8,0x38,0xFB,0xFF,0xFF}' in s,
 'verify exact chain':'actual!=static_cast<std::int64_t>(edge.target)' in s,
 'check soundplay marker':'std::memcmp(marker,"soundplay",sizeof(marker))!=0' in s,
 'verify executable page':'player-page-not-executable' in s and 'VirtualQuery(' in s,
 'unknown PE refuses':'refused=unrecognized-loader-image-pair' in s,
 'new unknown bytes refuse':'refused=native-fingerprint-mismatch' in s,
 'sound only enabled after last check':s.index('SoundLoaderBase.store(base,std::memory_order_release);')>s.index('if(!pageOk) {'),
 'retains quantity 131':'GroundQuantityBridgeLoader131' in s,
 'retains show-hide ground suppression':'GroundVisibility::ConcealBulkVisuals(' in s and 'if (!concealGroundVisuals && next)' in s,
}
for k,v in checks.items(): print(('PASS' if v else 'FAIL')+' '+k)
assert all(checks.values())
