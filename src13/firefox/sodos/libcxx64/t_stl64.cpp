/* t_stl64.cpp - containers/algoritmos da libc++ na ABI LP64 (SOX2). */
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern "C" void k_dbg(const char* s, int n);

static void mark(const char* s, int n) { k_dbg(s, n); }

int main() {
  char b[4] = {'M', 'A', 'I', '\n'};
  k_dbg(b, 4);
  std::vector<int> v{3, 1, 2};
  mark("STL64: vector\n", 14);
  std::sort(v.begin(), v.end());
  std::string s = "libc++ LP64";
  std::map<std::string, int> m;
  m["x"] = 7;
  mark("STL64: map\n", 11);
  printf("STL64: %s sort=%d map=%d\n", s.c_str(), v[0], m["x"]);
  return (v[0] == 1 && m["x"] == 7) ? 0 : 1;
}
