/* ztest.c - zlib no SODOS: comprime e descomprime um buffer e confere. */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void)
{
  static unsigned char src[64 * 1024], z[80 * 1024], back[64 * 1024];
  uLongf zl = sizeof z, bl = sizeof back;
  int i, fails = 0;
  for (i = 0; i < (int)sizeof src; i++) src[i] = (unsigned char)("SODOS QEMU zlib "[i % 16] ^ (i >> 10));
  if (compress2(z, &zl, src, sizeof src, 9) != Z_OK) { printf("FAIL compress2\n"); fails++; }
  else printf("PASS compress2: %u -> %lu bytes\n", (unsigned)sizeof src, (unsigned long)zl);
  if (uncompress(back, &bl, z, zl) != Z_OK || bl != sizeof src || memcmp(src, back, bl)) {
    printf("FAIL uncompress\n"); fails++;
  } else printf("PASS uncompress + memcmp\n");
  printf("PASS crc32 = %08lx\n", (unsigned long)crc32(0, src, sizeof src));
  printf("ZTEST: %s (zlib %s)\n", fails ? "FALHOU" : "ok", zlibVersion());
  return fails;
}
