#include <cstdio>
#include "mozilla/Vector.h"

int main() {
  mozilla::Vector<int, 4> v;
  for (int i = 0; i < 5; i++) {
    if (!v.append(i * 2)) return 2;
  }
  printf("MFBT: len=%u first=%d last=%d\n", (unsigned)v.length(), v[0], v[4]);
  return v.length() == 5 && v[4] == 8 ? 0 : 1;
}
