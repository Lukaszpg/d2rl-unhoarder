from pathlib import Path
s = (Path(__file__).parents[1] / 'src' / 'plugin.cpp').read_text(encoding='utf-8')
start = s.index('void RememberGroundIdentity(')
end = s.index('\nbool GetGroundIdentity(', start)
body = s[start:end]
resolve = '''    const auto* selected=ResolveGroundRule(table.get(),scalars,resolvedRule) ?\n        &resolvedRule : nullptr;'''
guard = '''    if (!selected || (!selected->hasBackground &&\n        !selected->hasTextColor && selected->show)) {\n        return;\n    }'''
assert resolve in body
assert guard in body
assert body.index(guard) < body.index('selected->hasName')
assert 'if(selected && (selected->hasBackground || selected->hasTextColor))\n    if (!selected ||' not in body
print('unmatched ground identity null-guard contract: ok')
