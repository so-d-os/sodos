/* vmain.cpp - soma 3+5+7 com a classe Vec (vec.cpp). */
#include <stdio.h>
#include "vec.h"

int main(void) {
  Vec v;
  int i;
  int s;
  v.push(3);
  v.push(5);
  v.push(7);
  s = 0;
  for (i = 0; i < v.len(); i++) s = s + v.at(i);
  printf("%d itens, soma %d\n", v.len(), s);
  return s == 15 ? 0 : 1;
}
