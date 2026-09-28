from pathlib import Path
p = Path(__file__).resolve().parents[1]
s = (p / 'src/plugin.cpp').read_text()
r = (p / 'src/filter_rule_engine.hpp').read_text()
h = (p / 'src/base_name_table.hpp').read_text()
assert '.version = "1.0.0"' in s
assert 'if(key=="code" || key=="baseName")' in s
assert 'key=="classId"' not in s[s.index('bool ParseV2Conditions('):s.index('// RGBA is a JSON STRING:')]
assert 'dest.baseCodes=std::move(parsed);' in s
assert 'BaseNameTable::DiscoverExcel(FilterConfigPath)' in s
assert 'BaseNameTable::LoadPair(names.excel,names.catalog,error)' in s
assert 'baseNamesExcelPath=baseNames.excel;' in s
assert 'ReadFilterFileStamp(excel/L"weapons.txt")' in s
assert 'ReadFilterFileStamp(excel/L"armor.txt")' in s
assert 'if(!c.baseCodes.empty() && !ContainsCode(c.baseCodes,item.code))' in r
assert 'if (!baseCodes.empty())' in r
assert 'classIds' not in r
assert 'headings[i]=="name"' in h and 'headings[i]=="code"' in h
assert '"weapons.txt"' in h and '"armor.txt"' in h
assert 'ReadNativeGroundQualityLevel(' in s
assert 'GroundPropertyLive::Purpose::VerifiedLabel' in s
assert 'PickupGuard::GroundMode(header[3])' in s
assert 'LOOT_BASENAME_TABLES_PATH' in s
print('1.0.0 baseName Excel column, lazy + mode-5 + pickup regressions: ok')
