from pathlib import Path

s = (Path(__file__).parents[1] / 'src' / 'plugin.cpp').read_text(encoding='utf-8')
start = s.index('void RememberGroundIdentity(')
end = s.index('\nbool GetGroundIdentity(', start)
body = s[start:end]

resolve = '''    const auto* selected=ResolveGroundRule(table.get(),scalars,resolvedRule) ?\n        &resolvedRule : nullptr;'''
guard = '''    if (!selected || (!selected->hasBackground &&\n        !selected->hasTextColor && selected->show)) {\n        BackgroundNoMatch.fetch_add(1,std::memory_order_relaxed);\n        return;\n    }'''

assert resolve in body
assert guard in body
assert body.index(guard) < body.index('selected->hasName')
# Regression for cleanup bug: removing a one-line diagnostic body must not leave
# a naked condition that captures the no-match guard as its body.
assert 'if(selected && (selected->hasBackground || selected->hasTextColor))\n    if (!selected ||' not in body
