/* vec.h - vetor de int. Metodos definidos em vec.cpp. */
#ifndef VEC_H
#define VEC_H

class Vec {
public:
  int* data;
  int n;
  int cap;
  Vec(void);
  ~Vec(void);
  void push(int v);
  int at(int i);
  int len(void);
};

#endif
