from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'src/plugin.cpp').read_text()
assert '.version = "1.0.0"' in s
assert 'void ObserveMinimapItemPosition(' in s
assert 'ObserveMinimapItemPosition(nativeUnit,code,unitId,classId)' in s
assert 'ObserveMinimapItemPosition(unit,code,recordId,header[1])' in s
assert 'first.data()+0x38' in s
assert 'GroundCandidatePageReadable(pathAddress,MinimapPathBytes)' in s
assert 'std::memcmp(pathFirst.data(),pathLast.data()' in s
assert 'std::memcmp(first.data()+0x38,last.data()+0x38' in s
assert 'UpdateMinimapProjectionItem(sample);' in s
worker=s[s.index('void RuntimeWorkerLoop('):s.index('// The best currently verified downstream',s.index('void RuntimeWorkerLoop('))]
for key in ('VK_F6','VK_F7','VK_F8','VK_F10','VK_F11','VK_F12'):
    assert key not in worker,key
assert 'VK_F9' in worker
print('1.0.0 production world-position tracking has no probe hotkeys: ok')
