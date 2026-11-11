/* t_cxx.cpp - CXXDSO.EXE: C++ dinamico sobre libc++.so.1 e libc.so.6. */
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <mutex>

static int ok, fail;
static void check(const char* what, bool cond) {
  std::printf("%s %s\n", cond ? "PASS" : "FALHA", what);
  if (cond) ok++; else fail++;
}

struct Global {
  int v;
  Global() : v(42) { std::printf("construtor global\n"); }
  ~Global() { std::printf("destrutor global\n"); }
};
static Global g;
thread_local int tl = 3;

struct Base { virtual ~Base() {} virtual int f() { return 1; } };
struct Derived : Base { int f() override { return 2; } };

int main() {
  std::printf("CXXDSO\n");
  check("construtor global rodou antes de main", g.v == 42);
  std::string s = "sodos";
  s += " + libc++.so.1";
  check("std::string", s.size() == 19 && s.find("libc++") == 8);
  std::vector<int> v{5, 3, 9, 1};
  std::sort(v.begin(), v.end());
  check("std::vector + sort", v[0] == 1 && v[3] == 9);
  std::map<std::string, int> m;
  m["a"] = 1;
  m["b"] = 2;
  check("std::map", m.size() == 2 && m["b"] == 2);
  Base* b = new Derived;
  check("new + metodo virtual", b->f() == 2);
  delete b;
  int seen = 0;
  std::mutex mu;
  std::thread t([&] {
    std::lock_guard<std::mutex> l(mu);
    seen = tl;  /* copia da thread: valor inicial */
    tl = 99;
  });
  t.join();
  check("std::thread + thread_local", seen == 3 && tl == 3);
  std::printf("CXXDSO: %d ok, %d falhas\n", ok, fail);
  return fail ? 1 : 0;
}
