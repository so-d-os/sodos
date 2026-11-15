/* img_big.c - o mesmo decodificador de img.c, com limite maior.
 *
 * O IMG.DLL normal para em 800 px de lado (bom para miniaturas e paginas).
 * Papel de parede precisa da resolucao da tela, entao o VIDEOCFG.EXE linka
 * esta variante: ate 3840 px de lado na saida (o IMAGEVW.EXE tambem usa).
 */
#define IMG_MAXS 3840
#define IMG_MAX_IN 8192
#include "img.c"
