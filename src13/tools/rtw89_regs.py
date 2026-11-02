# rtw89_regs.py nomes.txt: valores de #define e enums do rtw89 como #define
# do C, para net/rtw89_reg.h. Rode na pasta drivers/net/wireless/realtek/rtw89.
# Defines de varias linhas saem como FALTA e vao a mao.
import re
import sys
import glob

d = {}
for f in glob.glob('*.h') + glob.glob('*.c'):
    for m in re.finditer(r'^#define\s+([A-Za-z0-9_]+)\s+(.+?)\s*$', open(f).read(), re.M):
        d.setdefault(m.group(1), m.group(2))

for f in glob.glob('*.h'):
    for body in re.findall(r'enum\s+\w+\s*\{(.*?)\}', open(f).read(), re.S):
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        body = re.sub(r'//.*', '', body)
        prev = None
        for item in body.split(','):
            item = item.strip()
            if not item or item.startswith('#'):
                continue
            if '=' in item:
                k, v = [x.strip() for x in item.split('=', 1)]
            else:
                k = item.split()[0]
                v = '(%s)+1' % prev if prev else '0'
            d.setdefault(k, v)
            prev = k


def ev(name, depth=0):
    if depth > 40:
        raise Exception('loop')
    e = re.sub(r'/\*.*?\*/', '', d[name]).strip().rstrip('\\')
    e = re.sub(r'\bBIT\((\d+)\)', r'(1<<\1)', e)
    e = re.sub(r'\bGENMASK\((\d+),\s*(\d+)\)',
               lambda m: '(((1<<(%s+1))-1)^((1<<%s)-1))' % (m.group(1), m.group(2)), e)
    e = re.sub(r'\b(0x[0-9a-fA-F]+|\d+)[uU]?[lL]*\b', r'\1', e)
    e = re.sub(r'\b([A-Za-z_][A-Za-z0-9_]*)\b',
               lambda m: str(ev(m.group(1), depth + 1)), e)
    return eval(e)


out = []
for n in open(sys.argv[1]).read().split():
    try:
        out.append('#define %-36s 0x%Xu' % (n, ev(n)))
    except Exception as x:
        out.append('/* FALTA %s: %s */' % (n, x))
print('\n'.join(out))
