# rtw89_fn.py arquivo.c nome...: imprime o corpo de funcoes C pelo nome.
# Ajuda no porte do rtw89 (rode na pasta drivers/net/wireless/realtek/rtw89).
import re
import sys

src = open(sys.argv[1]).read()
for name in sys.argv[2:]:
    m = re.search(r'^(?:[a-z][^\n;]*\b)?' + re.escape(name) + r'\([^;{]*\)\s*\{', src, re.M)
    if not m:
        print('/* nao achei', name, '*/')
        continue
    i = m.end()
    depth = 1
    while depth:
        depth += {'{': 1, '}': -1}.get(src[i], 0)
        i += 1
    print(src[m.start():i])
    print()
