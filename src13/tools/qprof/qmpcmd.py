#!/usr/bin/env python3
"""qmpcmd.py SOCK 'hmp cmd' ['hmp cmd' ...] - roda comandos HMP via QMP.
Atalho: 'type:TEXTO' digita o texto (sendkey por letra)."""
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
f = s.makefile("rwb", buffering=0)
f.readline()
def c(m):
    f.write((json.dumps(m) + "\n").encode())
    while True:
        r = json.loads(f.readline())
        if "event" not in r:
            return r
c({"execute": "qmp_capabilities"})
KEYS = {" ": "spc", "\n": "ret", ".": "dot", ":": "shift-semicolon", "\\": "backslash", "/": "slash", "-": "minus"}
def hmp(line):
    return c({"execute": "human-monitor-command", "arguments": {"command-line": line}}).get("return", "")
for a in sys.argv[2:]:
    if a.startswith("type:"):
        for ch in a[5:].replace("\\n", "\n"):
            k = KEYS.get(ch) or (("shift-" + ch.lower()) if ch.isupper() else ch)
            hmp("sendkey " + k)
            time.sleep(0.05)
    else:
        print(hmp(a), end="")
