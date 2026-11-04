#include <stdio.h>
extern int rust_add(int a, int b);
extern int rust_vec(int n);
int main(void) {
  int r = rust_add(20, 22);
  int s = rust_vec(5);
  printf("RUST64: add=%d vec=%d\n", r, s);
  return (r == 42 && s == 10) ? 0 : 1;
}
