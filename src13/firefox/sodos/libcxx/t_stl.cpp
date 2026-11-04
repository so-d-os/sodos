/* t_stl.cpp - teste da libc++ sobre o SODOS (M2b).
 *
 * Cobre containers/algoritmos/utilitarios da biblioteca padrao; o link
 * mostra o que ainda falta do runtime compilado da libc++.
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

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

int main() {
  printf("STLTEST - libc++ sobre o SODOS\n");
  flush_out();

  std::vector<int> v;
  v.push_back(3);
  v.push_back(1);
  v.push_back(2);
  std::sort(v.begin(), v.end());
  CHECK(v.size() == 3 && v[0] == 1 && v[2] == 3, "stl: vector + sort");

  std::string s = "ola";
  s += " mundo";
  CHECK(s.size() == 9 && s == "ola mundo", "stl: string + operador +=");
  CHECK(s.find("mundo") == 4 && s.substr(0, 3) == "ola", "stl: string find/substr");
  CHECK(std::to_string(42) == "42", "stl: to_string");

  std::map<std::string, int> m;
  m["um"] = 1;
  m["dois"] = 2;
  CHECK(m.size() == 2 && m["dois"] == 2, "stl: map");
  CHECK(m.find("um") != m.end(), "stl: map find");

  std::unordered_map<std::string, int> um;
  um["a"] = 10;
  um["b"] = 20;
  CHECK(um.size() == 2 && um["b"] == 20, "stl: unordered_map + hash");

  std::set<int> st;
  st.insert(4);
  st.insert(2);
  st.insert(4);
  CHECK(st.size() == 2 && *st.begin() == 2, "stl: set");

  std::list<int> li;
  li.push_back(1);
  li.push_back(2);
  std::deque<int> dq;
  dq.push_front(1);
  dq.push_back(2);
  CHECK(li.size() == 2 && dq.size() == 2 && dq.front() == 1, "stl: list + deque");

  std::array<int, 4> ar = {{4, 3, 2, 1}};
  std::sort(ar.begin(), ar.end());
  CHECK(ar[0] == 1 && ar[3] == 4, "stl: array + sort");

  std::tuple<int, std::string> tp(7, "sete");
  CHECK(std::get<0>(tp) == 7 && std::get<1>(tp) == "sete", "stl: tuple");

  std::unique_ptr<int> p(new int(5));
  CHECK(p && *p == 5, "stl: unique_ptr");
  std::unique_ptr<int> q = std::move(p);
  CHECK(!p && q && *q == 5, "stl: unique_ptr move");

  std::shared_ptr<int> sp = std::make_shared<int>(9);
  std::shared_ptr<int> sp2 = sp;
  CHECK(sp.use_count() == 2 && *sp2 == 9, "stl: shared_ptr");

  std::function<int(int)> f = [](int x) { return x * 2; };
  CHECK(f(21) == 42, "stl: std::function + lambda");

  std::vector<std::string> words;
  words.push_back("a");
  words.push_back("b");
  std::string joined;
  for (const auto& w : words) joined += w;
  CHECK(joined == "ab", "stl: range-for + vector<string>");

  std::vector<int> nums = {1, 2, 3, 4};
  CHECK(std::accumulate(nums.begin(), nums.end(), 0) == 10, "stl: numeric accumulate");

  std::string_view sv("texto");
  CHECK(sv.size() == 5 && sv.substr(0, 3) == "tex", "stl: string_view");

  std::optional<int> opt = 11;
  CHECK(opt && *opt == 11, "stl: optional");

  std::variant<int, std::string> var = std::string("v");
  CHECK(std::holds_alternative<std::string>(var) &&
            std::get<std::string>(var) == "v",
        "stl: variant");

  {
    auto t0 = std::chrono::steady_clock::now();
    volatile unsigned spin = 0;
    for (unsigned i = 0; i < 200000; i++) spin += i;
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    CHECK(us >= 0, "stl: chrono steady_clock");
  }

  CHECK(std::stoi("123") == 123, "stl: stoi");
  CHECK(std::stof("1.5") == 1.5f, "stl: stof");

  printf("\nSTLTEST: %d ok, %d falhas\n", passed, failed);
  flush_out();
  return failed ? 1 : 0;
}
