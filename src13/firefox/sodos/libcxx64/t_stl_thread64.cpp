#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

int main() {
  std::mutex m;
  int counter = 0;
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; i++) {
    ts.emplace_back([&]() {
      for (int k = 0; k < 200; k++) {
        std::lock_guard<std::mutex> lock(m);
        counter++;
      }
    });
  }
  for (auto& t : ts) t.join();

  std::atomic<int> a(0);
  std::vector<std::thread> ts2;
  for (int i = 0; i < 2; i++) {
    ts2.emplace_back([&]() {
      for (int k = 0; k < 100; k++) a.fetch_add(1);
    });
  }
  for (auto& t : ts2) t.join();

  std::mutex cm;
  std::condition_variable cv;
  bool ready = false;
  int val = 0;
  std::thread prod([&]() {
    std::lock_guard<std::mutex> lock(cm);
    val = 42;
    ready = true;
    cv.notify_one();
  });
  std::thread cons([&]() {
    std::unique_lock<std::mutex> lock(cm);
    cv.wait(lock, [&]() { return ready; });
  });
  prod.join();
  cons.join();

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  printf("THRT64: counter=%d atomic=%d val=%d\n", counter, a.load(), val);
  return (counter == 800 && a.load() == 200 && val == 42) ? 0 : 1;
}
