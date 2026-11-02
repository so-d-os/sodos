# rtw89_tables.py rtw8852b_table.c rfe cv [rtw8852b_rfk_table.c] > net/rtw8852b_tbl.h
#
# Tira do rtw8852b_table.c do Linux as tabelas BB, RADIO A/B e NCTL e ja
# resolve os if/elif/else por RFE e versao do chip (rtw89_phy_sel_headline
# e rtw89_phy_init_reg), deixando so as escritas que valem para esta placa.
# Com o rtw8852b_rfk_table.c, tambem tira as receitas de calibracao (RFK)
# como {flag, caminho, endereco, mascara, valor}; flag 0 = RF, 1 = BB com
# mascara, 2 = setar bits, 3 = limpar bits, 4 = esperar 'valor' us.
# O rtw89 e "GPL-2.0 OR BSD-3-Clause" (Realtek); usamos a BSD-3-Clause.
import re
import sys

src = open(sys.argv[1]).read()
rfe = int(sys.argv[2])
cv = int(sys.argv[3])

TABLES = [
    ('rtw89_8852b_phy_bb_regs', 'tbl_bb'),
    ('rtw89_8852b_phy_radioa_regs', 'tbl_radioa'),
    ('rtw89_8852b_phy_radiob_regs', 'tbl_radiob'),
    ('rtw89_8852b_phy_nctl_regs', 'tbl_nctl'),
]


def load(name):
    m = re.search(name + r'\[\] = \{(.*?)\n\};', src, re.S)
    return [(int(a, 16), int(d, 16))
            for a, d in re.findall(r'\{(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\}', m.group(1))]


def sel_headline(regs):
    n = 0
    while n < len(regs) and (regs[n][0] >> 28) == 0xF:
        n += 1
    if n == 0:
        return 0, 0
    target = [r[0] & 0x0FFFFFFF for r in regs[:n]]
    for cmp in ((rfe << 16) | cv, (rfe << 16) | 0xFF):
        if cmp in target:
            return n, target.index(cmp)
    for want in (rfe, 0xFF):
        best = None
        for i, t in enumerate(target):
            if (t >> 16) & 0xFF == want and (best is None or (t & 0xFF) >= (target[best] & 0xFF)):
                best = i
        if best is not None:
            return n, best
    raise SystemExit('sem headline para rfe %d cv %d' % (rfe, cv))


def resolve(regs):
    size, idx = sel_headline(regs)
    cfg_target = regs[idx][0] & 0x0FFFFFFF if size else 0
    out = []
    target = 0
    matched = True
    found = False
    for addr, data in regs[size:]:
        cond = addr >> 28
        if cond in (0x8, 0x9):
            target = addr & 0x0FFFFFFF
        elif cond == 0xA:
            matched = False
            if not found:
                raise SystemExit('else sem caso: %x' % addr)
        elif cond == 0xB:
            matched = True
            found = False
        elif cond == 0x4:
            if found:
                matched = False
            elif target == cfg_target:
                matched = True
                found = True
            else:
                matched = False
                found = False
        elif matched:
            out.append((addr, data))
    return out


print('/* rtw8852b_tbl.h - gerado por tools/rtw89_tables.py (RFE %d, cv %d).' % (rfe, cv))
print(' * Tabelas do rtw8852b_table.c do Linux 6.12, Copyright(c) Realtek,')
print(' * licenca BSD-3-Clause. Nao editar a mao. */')
print('#pragma once')
print('#include <stdint.h>')
print()
for name, cname in TABLES:
    regs = resolve(load(name))
    print('static const uint32_t %s[%d][2] = {' % (cname, len(regs)))
    for a, d in regs:
        print('  {0x%X, 0x%08X},' % (a, d))
    print('};')
    print()


RFK = ['afe_init_defs', 'dack_s0_1_defs', 'dack_s0_2_defs', 'dack_s0_3_defs',
       'dack_s1_1_defs', 'dack_s1_2_defs', 'dack_s1_3_defs',
       'check_addc_defs_a', 'check_addc_defs_b', 'check_dadc_en_defs_a',
       'check_dadc_en_defs_b', 'check_dadc_dis_defs_a', 'check_dadc_dis_defs_b']
FLAGS = {'WRF': 0, 'WM': 1, 'WS': 2, 'WC': 3, 'DELAY': 4}


def cval(e):
    e = re.sub(r'\bBIT\((\d+)\)', r'(1<<\1)', e)
    e = re.sub(r'\bGENMASK\((\d+),\s*(\d+)\)',
               lambda m: '(((1<<(%s+1))-1)^((1<<%s)-1))' % (m.group(1), m.group(2)), e)
    e = e.replace('RF_PATH_A', '0').replace('RF_PATH_B', '1')
    e = re.sub(r'\b(0x[0-9a-fA-F]+|\d+)[uU]?[lL]*\b', r'\1', e)
    return eval(e)


def split_args(a):
    out, depth, cur = [], 0, ''
    for c in a:
        if c == ',' and depth == 0:
            out.append(cur)
            cur = ''
            continue
        depth += (c == '(') - (c == ')')
        cur += c
    out.append(cur)
    return [x.strip() for x in out]


if len(sys.argv) > 4:
    rsrc = open(sys.argv[4]).read()
    for name in RFK:
        m = re.search(r'rtw8852b_' + name + r'\[\] = \{(.*?)\n\};', rsrc, re.S)
        rows = []
        for kind, args in re.findall(r'RTW89_DECL_RFK_(\w+)\((.*?)\),?\s*$', m.group(1), re.M):
            a = [cval(x) for x in split_args(args)]
            if kind == 'WRF':
                rows.append((0, a[0], a[1], a[2], a[3]))
            elif kind == 'WM':
                rows.append((1, 0, a[0], a[1], a[2]))
            elif kind in ('WS', 'WC'):
                rows.append((FLAGS[kind], 0, a[0], a[1], 0))
            else:
                rows.append((4, 0, 0, 0, a[0]))
        print('static const uint32_t rfk_%s[%d][5] = {' % (name.replace('_defs', ''), len(rows)))
        for r in rows:
            print('  {%d, %d, 0x%X, 0x%X, 0x%X},' % r)
        print('};')
        print()
