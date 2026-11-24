/* prtscr.h - PrintScreen do WIN (ver prtscr.c). */
#pragma once

/* Grava a tela do WIN em Z:\SAVE\SCREENSHOT\printscreen-AAAAMMDD-HHMMSS.png.
 * Devolve o tamanho do PNG (>0) e o caminho em out_path; <0 se o WIN nao esta
 * na tela (-1), nao ha Z: (-2), faltou memoria (-3) ou a gravacao falhou (-4). */
int prtscr_save(char* out_path, int max);

/* O mesmo para a tela de texto visivel (80x25, Ctrl+F1..F12): 640x400 com a
 * fonte atual. Mesmos retornos, sem o -1. */
int prtscr_text_save(char* out_path, int max);

/* Copia que o ultimo print deixou em D:\TEMP (SET PRTSCRCOPY); "" se nao
 * houve copia. */
const char* prtscr_last_copy(void);
