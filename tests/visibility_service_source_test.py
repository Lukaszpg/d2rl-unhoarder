from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
a=s.index('UnhoarderVisibility::Decision __cdecl QueryGroundVisibility(');z=s.index('const UnhoarderVisibility::Service PublishedVisibilityService',a);body=s[a:z]
for term in ['code=CanonicalItemCode(code)','before[0]!=4','before[2]!=guid','before[3]!=3','ItemCodeTable::Lookup(*codesAtStart,before[1])','GroundRuleItem(code,mutableUnit,rules.get(),guid)','ResolveGroundRule(rules.get(),item,decision)','before!=after','rules!=PublishedFilterRules.load','return D::Unknown','return decision.show?D::Visible:D::Hidden']:
 assert term in body,term
assert 'WriteProcessMemory' not in body and 'InstallInlineHook' not in body
assert 'InstallNativePickupGuard' not in body
print('Read-only visibility: fresh identity/rules, stable headers, canonical codes and generation; visibility callback never intercepts pickup.')
