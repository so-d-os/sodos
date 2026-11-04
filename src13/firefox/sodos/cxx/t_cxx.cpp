/* t_cxx.cpp - teste do runtime C++ minimo do SODOS (M2a).
 *
 * Prova o que o compilador gera sem libstdc++: construtor/destrutor global
 * (.init_array/.fini_array via __cxa_atexit), classes com heranca virtual,
 * templates, new/delete e static local com guard.
 */
#include <stddef.h>
#include "sodos_cxx.h"

extern "C" int printf(const char* fmt, ...);
struct __so_file;
extern "C" __so_file* __so_stdio(int which);
extern "C" int fflush(__so_file* f);

static void flush_out(void) { fflush(__so_stdio(1)); }

static int passed;
static int failed;

static void ok(const char* what) {
  printf("PASS %s\n", what);
  flush_out();
  passed++;
}

static void fail(const char* what, int line) {
  printf("FAIL %s (linha %d)\n", what, line);
  flush_out();
  failed++;
}

#define CHECK(cond, what)      \
  do {                         \
    if (cond) ok(what);        \
    else fail(what, __LINE__); \
  } while (0)

static int g_ctor_state;
static int g_dtor_state;

struct Global {
  int value;
  Global() : value(7) { g_ctor_state = value; }
  ~Global() { g_dtor_state = 1; }
};

static Global g_global;

class Shape {
 public:
  virtual ~Shape() {}
  virtual int area() const = 0;
  virtual const char* name() const { return "shape"; }
};

class Square : public Shape {
  int side_;

 public:
  explicit Square(int s) : side_(s) {}
  int area() const override { return side_ * side_; }
  const char* name() const override { return "square"; }
};

template <typename T>
static T twice(T v) {
  return v + v;
}

static int local_static_calls(void) {
  static int calls = 0; /* g++ emite __cxa_guard_* */
  return ++calls;
}

class Counter {
  int n_;

 public:
  Counter() : n_(0) {}
  Counter& operator++() {
    ++n_;
    return *this;
  }
  int value() const { return n_; }
};

int main() {
  printf("CXXTEST - runtime C++ minimo do SODOS\n");
  flush_out();

  CHECK(g_ctor_state == 7, "cxx: construtor global roda antes de main");

  Shape* s = new Square(5);
  CHECK(s != nullptr && s->area() == 25, "cxx: new + heranca virtual");
  CHECK(s->name()[0] == 's', "cxx: override de metodo virtual");

  int* arr = new int[16];
  for (int i = 0; i < 16; i++) arr[i] = i * i;
  CHECK(arr[15] == 225, "cxx: new[]");
  delete[] arr;
  delete s;

  CHECK(twice(21) == 42 && twice(1.5) == 3.0, "cxx: template");

  CHECK(local_static_calls() == 1 && local_static_calls() == 2,
        "cxx: static local com guard");

  Counter c;
  ++(++c);
  CHECK(c.value() == 2, "cxx: operador sobrecarregado");

  printf("\nCXXTEST: %d ok, %d falhas\n", passed, failed);
  flush_out();
  return failed ? 1 : 0;
}
