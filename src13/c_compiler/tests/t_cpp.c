/* C++ enxuto: class, metodo, heranca, new/delete, referencia, namespace. */
#include <stdio.h>

static int fails;
static int count;
static int alive;

#define CHECK(expect, e) check_ll((long long)(expect), (long long)(e), #e, __LINE__)

static void check_ll(long long want, long long got, const char* what, int line) {
  count++;
  if (want != got) {
    printf("FALHOU linha %d: %s => %lld (esperava %lld)\n", line, what, got, want);
    fails++;
  }
}

class Life {
public:
  int v;
  Life(int x) {
    v = x;
    alive = alive + 1;
  }
  ~Life() { alive = alive - 1; }
  int get(void) { return v; }
};

class Base {
protected:
  int n;
public:
  Base(int v) { n = v; }
  int get(void) { return n; }
};

class Step : public Base {
public:
  int s;
  Step(int v, int st) : Base(v) { s = st; }
  int get(void) { return n + s; }
  void bump(void) { n = n + s; }
};

static void inc(int& x) { x = x + 1; }

static int grab(void) {
  Life b(4);
  return b.v;
}

namespace mathx {
  int dbl(int x) { return x + x; }
  int quad(int x) { return dbl(dbl(x)); }
}

int main(void) {
  Life* p;
  Step* s;
  int a;
  int* arr;
  bool flag;
  int* np;

  CHECK(0, alive);
  {
    Life local(1);
    CHECK(1, alive);
    CHECK(1, local.get());
  }
  CHECK(0, alive);

  CHECK(4, grab());
  CHECK(0, alive);

  p = new Life(8);
  CHECK(1, alive);
  CHECK(8, p->get());
  delete p;
  CHECK(0, alive);

  s = new Step(1, 4);
  CHECK(5, s->get());
  s->bump();
  CHECK(9, s->get());
  delete s;

  a = 3;
  inc(a);
  CHECK(4, a);

  arr = new int[4];
  arr[0] = 7;
  arr[3] = 9;
  CHECK(7, arr[0]);
  CHECK(9, arr[3]);
  delete[] arr;

  CHECK(8, mathx::quad(2));

  flag = 1;
  CHECK(1, flag);
  flag = 0;
  CHECK(0, flag);

  np = nullptr;
  CHECK(1, np == 0);

  if (fails) {
    printf("%d falhas em %d\n", fails, count);
    return 1;
  }
  printf("ok %d\n", count);
  return 0;
}
