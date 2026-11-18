/* msgbox.c - USER32 (W0: a caixa sai no console). */
#include <windows.h>

int main(void) {
  MessageBoxA(NULL, "MessageBoxA no SODOS", "WINE", MB_OK);
  MessageBoxW(NULL, L"MessageBoxW com acentuação", L"WINE", MB_OK);
  return 0;
}
