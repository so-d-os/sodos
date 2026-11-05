/* img_big.c - o mesmo decodificador de img.c, com limite maior.
 *
 * O IMG.DLL normal para em 800 px de lado (bom para miniaturas e paginas).
 * Papel de parede precisa da resolucao da tela, entao o VIDEOCFG.EXE linka
 * esta variante: ate 1920 px de lado na saida.
 */
#define IMG_MAXS 1920
#define IMG_MAX_IN 8192
#include "img.c"
