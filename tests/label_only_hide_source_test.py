from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
assert s.count('InstallNativePickupGuard(')==2 # definition plus keyboard-only installation
assert 'override=keyboard-alt input=keyboard-only' in s
assert 'const auto& dispatchEntry=NativePickupProfile::Dispatch;' in s
a=s.index('// A hidden-hover label'); b=s.index('if (!rule->hasBackground',a)
assert 'if (!rule->show) return false;' in s[a:b]
assert 'SuppressHiddenHover(' not in s[a:b]
assert 'ConcealBulkVisuals(' in s
print('hide presentation remains unchanged; guarded keyboard ALT pickup policy is installed')
