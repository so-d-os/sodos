#!/usr/bin/env python3
# genres.py - embute os recursos do NetSurf num .c (o FAT do SODOS e 8.3).
# Uso: genres.py <dir res do framebuffer> <saida.c>
import gzip, os, sys

res, out = sys.argv[1], sys.argv[2]
names = ["adblock.css", "default.css", "internal.css", "quirks.css",
         "credits.html", "licence.html", "welcome.html", "Messages"]
with open(out, "w") as f:
    f.write("/* gerado por sodos/genres.py - nao editar */\n")
    f.write("#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n\n")
    for i, n in enumerate(names):
        data = open(os.path.join(res, n), "rb").read()
        if data[:2] == b"\x1f\x8b":
            data = gzip.decompress(data)
        f.write("static const uint8_t r%d[%d] = {\n" % (i, len(data) + 1))
        for k in range(0, len(data), 16):
            f.write("".join("%d," % b for b in data[k:k + 16]) + "\n")
        f.write("0};\n")
    f.write("\nstatic const struct { const char* name; const uint8_t* data; size_t len; } "
            "res_tab[] = {\n")
    for i, n in enumerate(names):
        f.write('  { "%s", r%d, sizeof(r%d) - 1 },\n' % (n, i, i))
    f.write("};\n\n")
    f.write("""int sodos_resource(const char* name, const uint8_t** data, size_t* len) {
  size_t i;
  for (i = 0; i < sizeof(res_tab) / sizeof(res_tab[0]); i++) {
    if (strcmp(res_tab[i].name, name) == 0) {
      *data = res_tab[i].data;
      *len = res_tab[i].len;
      return 0;
    }
  }
  return -1;
}
""")
