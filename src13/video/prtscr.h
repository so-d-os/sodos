/* prtscr.h - PrintScreen do WIN (ver prtscr.c). */
#pragma once

/* Grava a tela do WIN em Z:\SAVE\SCREENSHOT\printscreen-AAAAMMDD-HHMMSS.png.
 * Devolve o tamanho do PNG (>0) e o caminho em out_path; <0 se o WIN nao esta
 * na tela (-1), nao ha Z: (-2), faltou memoria (-3) ou a gravacao falhou (-4). */
int prtscr_save(char* out_path, int max);
