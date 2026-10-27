/* vec.cpp - vetor que cresce com new/delete. */
#include "vec.h"

Vec::Vec(void) {
  data = 0;
  n = 0;
  cap = 0;
}

Vec::~Vec(void) {
  if (data) delete[] data;
  data = 0;
}

void Vec::push(int v) {
  int* neu;
  int i;
  if (n == cap) {
    int ncap = cap == 0 ? 4 : cap * 2;
    neu = new int[ncap];
    for (i = 0; i < n; i++) neu[i] = data[i];
    if (data) delete[] data;
    data = neu;
    cap = ncap;
  }
  data[n] = v;
  n = n + 1;
}

int Vec::at(int i) {
  if (i < 0 || i >= n) return 0;
  return data[i];
}

int Vec::len(void) { return n; }
