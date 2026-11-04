/* t_stl_thread.cpp - threads/mutex/condvar/atomic da libc++ no SODOS (M2b).
 *
 * Usa os pthreads cooperativos da libsodos por baixo do std::thread.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
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
  printf("THRTEST - std::thread/mutex/condvar no SODOS\n");
  flush_out();

  {
    std::mutex m;
    int counter = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
      threads.emplace_back([&]() {
        for (int k = 0; k < 200; k++) {
          std::lock_guard<std::mutex> lock(m);
          counter++;
        }
      });
    }
    for (auto& t : threads) t.join();
    CHECK(counter == 800, "thread: 4 threads + mutex + join");
  }

  {
    std::mutex m;
    std::condition_variable cv;
    int produced = 0, consumed = 0;
    std::thread producer([&]() {
      for (int i = 0; i < 10; i++) {
        std::lock_guard<std::mutex> lock(m);
        produced++;
        cv.notify_one();
      }
    });
    std::thread consumer([&]() {
      std::unique_lock<std::mutex> lock(m);
      while (consumed < 10) {
        cv.wait(lock, [&]() { return produced > consumed; });
        consumed++;
      }
    });
    producer.join();
    consumer.join();
    CHECK(produced == 10 && consumed == 10, "thread: condition_variable");
  }

  {
    std::atomic<int> a(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
      threads.emplace_back([&]() {
        for (int k = 0; k < 250; k++) a.fetch_add(1);
      });
    }
    for (auto& t : threads) t.join();
    CHECK(a.load() == 1000, "thread: std::atomic");
  }

  {
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    CHECK(ms >= 10, "thread: sleep_for");
  }

  printf("\nTHRTEST: %d ok, %d falhas\n", passed, failed);
  flush_out();
  return failed ? 1 : 0;
}
