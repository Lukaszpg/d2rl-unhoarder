from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/plugin.cpp').read_text();h=(root/'src/maps_socket_runtime.hpp').read_text()
socket=s[s.index('bool ReadNativeGroundSockets('):s.index('bool ReadNativeVendorSellPrice(',s.index('bool ReadNativeGroundSockets('))]
assert 'if (!readSocket(first)) return false;' in socket
assert 'if (!readSocket(second)) return false;' in socket
assert 'before!=after' in socket and 'first!=second' in socket
assert 'first<0 || first>15' in socket
assert 'MapsSocket::Read(Context->exeBase' in socket
assert 'reinterpret_cast<Reader>(game+0x2f5020)(item,194,0)' in h
assert 'value<0 || value>15' in h
assert 'VerifyFileHash(state.maps,MapsHash)' in h and 'VerifyFileHash(state.core,CoreHash)' in h
assert 'maps.Reader()' in h and 'CoreProfile::Wrapper' in h and 'CoreProfile::Body' in h
assert 'InstallInlineHook' not in h and 'WriteProcessMemory' not in h
assert 'GroundQuantityReader.store(' not in socket
assert s.count('MapsSocket::Shutdown();')==1
print('PASS socket-only capability, retained double-read identity/range guards, exact owner/graph gates and conditional failure; no general stat admission or native patches.')
