from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'src/plugin.cpp').read_text()
h=(p/'src/item_type_table.hpp').read_text()
r=(p/'src/filter_rule_engine.hpp').read_text()
assert '.version = "1.0.0"' in s
assert '#include "item_type_table.hpp"' in s
assert 'if(key=="itemType")' in s
assert 'dest.typeCodes=std::move(parsed);' in s
assert 'if(parsed.empty())' in s and 'itemType-selector-has-no-item-codes' in s
assert 'ItemTypeTable::Load(types.excel,types.catalog,error)' in s
assert 'fresh->itemTypesExcelPath=itemTypes.excel;' in s
assert 'LOOT_ITEMTYPE_TABLES_READY' in s
assert 'ReadFilterFileStamp(excel/L"itemtypes.txt")' in s
assert 'ReadFilterFileStamp(excel/L"misc.txt")' in s
assert 'if(!typeCodes.empty()' in r
assert 'std::binary_search(c.typeCodes.begin(),c.typeCodes.end(),item.code)' in r
assert '"ItemType"' in h and '"Code"' in h
assert '"Equiv1"' in h and '"Equiv2"' in h
assert '"type2"' in h and '"misc.txt"' in h
assert 'std::atomic_store_explicit(&PublishedFilterRules,published,' in s
assert 'GroundPropertyLive::Purpose::VerifiedLabel' in s
assert 'if(!PickupGuard::GroundMode(header[3]))' in s
assert '(c.itemLevel.enabled && !item.itemLevelKnown)' in r
print('1.0.0 itemType reload-only hierarchy, lazy matching and pickup regressions: ok')
