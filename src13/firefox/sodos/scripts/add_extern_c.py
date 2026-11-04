#!/usr/bin/env python3
"""add_extern_c.py - poe extern "C" nos headers C do SODOS usados pela libc++.

Nao mexe em headers que ja tem a guarda. Roda uma vez; idempotente.
"""
import re
import sys
from pathlib import Path

HEADERS = [
    "stdio.h", "stdlib.h", "string.h", "math.h", "time.h", "ctype.h",
    "locale.h", "errno.h", "inttypes.h", "fcntl.h", "unistd.h", "signal.h",
    "dirent.h", "sys/types.h", "sys/stat.h", "sys/time.h", "sys/select.h",
    "sys/socket.h", "netinet/in.h", "netdb.h", "arpa/inet.h",
]


def add_guards(path: Path) -> bool:
    text = path.read_text(encoding="utf-8")
    if 'extern "C"' in text:
        return False
    lines = text.splitlines()
    guard_i = None
    for i, line in enumerate(lines):
        if re.match(r"^#define\s+_[A-Z0-9_]+_H\b", line):
            guard_i = i
            break
    if guard_i is None:
        print("sem guarda:", path, file=sys.stderr)
        return False
    # ultimo #endif (fecha a guarda)
    end_i = None
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == "#endif":
            end_i = i
            break
    if end_i is None or end_i <= guard_i:
        print("sem #endif:", path, file=sys.stderr)
        return False
    lines[guard_i + 1:guard_i + 1] = [
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
    ]
    end_i += 4
    lines[end_i:end_i] = [
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return True


def main() -> int:
    root = Path(sys.argv[1])
    n = 0
    for rel in HEADERS:
        p = root / rel
        if not p.exists():
            print("falta:", p, file=sys.stderr)
            continue
        if add_guards(p):
            n += 1
    print("headers ajustados:", n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
