from pathlib import Path
root=Path(__file__).resolve().parents[1]
plugin=(root/'src/plugin.cpp').read_text()
header=(root/'src/item_code_table.hpp').read_text()
assert 'GetItemCodeRva' not in plugin
assert 'HookGetItemCode(' not in plugin
assert 'OriginalGetItemCode' not in plugin
assert 'NativeGetItemCode' not in plugin
assert '0x36EF50' not in plugin
assert 'LookupItemCode(std::uint32_t classId)' in plugin
assert 'ItemCodeTable::Lookup(*snapshot,classId)' in plugin
assert 'const D2RL::DataTableService* ItemCodeDataTables{};' in plugin
assert 'TableId::Items' in plugin
assert 'findRowByCode(context,Bank::Rotw,TableId::Items' in plugin
assert 'service->getTable(context,Bank::Rotw,TableId::Items' in plugin
assert 'OnItemCodeTablesLoaded' in plugin
assert 'registerDataTablesLoadedListener' in plugin
assert 'unregisterDataTablesLoadedListener' in plugin
assert 'PublishedItemCodes.store({},std::memory_order_release);' in plugin
assert 'code-offset-not-uniquely-qualified' in plugin
assert 'compiled-row-code-index-mismatch' in plugin
assert 'ItemCodeTable::PadForSdk(code)' in plugin
assert 'classId<table.codes.size()' in header
assert 'const auto code=CanonicalItemCode(LookupItemCode(header[1]));' in plugin
assert 'const auto code=CanonicalItemCode(LookupItemCode(snapshot.classId));' in plugin
assert 'codesAtStart!=PublishedItemCodes.load' in plugin
print('Item codes use only the compiled SDK class ID table, never the native helper.')
