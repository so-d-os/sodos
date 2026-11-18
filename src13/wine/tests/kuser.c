/* kuser.c - le o relogio da KUSER_SHARED_DATA (0x7FFE0000) como o runtime do
 * Go faz: SystemTime (+0x14) e InterruptTime (+0x08), antes e depois de
 * Sleep(1500). Saida esperada: data de hoje e ~1500 ms de diferenca. */
#include <windows.h>
#include <stdio.h>

static unsigned long long ksys(int off) {
  volatile unsigned int* k = (volatile unsigned int*)(0x7FFE0000ull + off);
  unsigned int lo, hi, hi2;
  do {
    hi = k[1];
    lo = k[0];
    hi2 = k[2];
  } while (hi != hi2);
  return ((unsigned long long)hi << 32) | lo;
}

int main(void) {
  unsigned long long s0 = ksys(0x14), i0 = ksys(0x08), s1, i1;
  FILETIME ft;
  SYSTEMTIME st;
  Sleep(1500);
  s1 = ksys(0x14);
  i1 = ksys(0x08);
  ft.dwLowDateTime = (DWORD)s1;
  ft.dwHighDateTime = (DWORD)(s1 >> 32);
  FileTimeToSystemTime(&ft, &st);
  printf("KUSER data %04d-%02d-%02d %02d:%02d:%02d UTC\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
         st.wSecond);
  printf("KUSER andou %llu ms (SystemTime) / %llu ms (InterruptTime)\n", (s1 - s0) / 10000, (i1 - i0) / 10000);
  printf("%s\n", (s1 - s0) / 10000 >= 1000 && st.wYear >= 2025 && st.wYear < 2040 ? "KUSER OK" : "KUSER ERRADO");
  return 0;
}
