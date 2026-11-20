/* cpptest - does Omi run real C++ with real threads?
 *
 * Zig's C++ front end, libc++ and musl, statically linked: no Omi extensions, no
 * hand-rolled anything. What this covers is what a browser engine's foundation
 * needs before any of it can be attempted - std::thread over pthreads,
 * std::atomic, a thread_local with a destructor (__cxa_thread_atexit_impl),
 * exceptions unwinding through frames (PT_GNU_EH_FRAME + AT_PHDR from the
 * loader), and libc++'s allocator and containers on top of mallocng.
 *
 * Proven from the ISO, not from host tests.
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <iostream>
#include <map>

static int checks, failures;

static void ok(const char *what) {
  checks++;
  std::printf("[cxx] ok   %s\n", what);
}

static void bad(const char *what, long got) {
  checks++;
  failures++;
  std::printf("[cxx] FAIL %s (got %ld)\n", what, got);
}

/* ---- 1. a std::thread runs and joins ---------------------------------- */

static void test_thread() {
  int ran = 0;
  std::thread t([&ran] { ran = 1; });
  t.join();
  if (ran == 1) ok("std::thread runs and joins");
  else bad("std::thread runs and joins", ran);
}

/* ---- 2. std::mutex serialises eight threads --------------------------- */

static void test_mutex() {
  std::mutex m;
  long counter = 0;
  std::vector<std::thread> pool;
  for (int i = 0; i < 8; i++)
    pool.emplace_back([&] {
      for (int j = 0; j < 2000; j++) {
        std::lock_guard<std::mutex> guard(m);
        counter++;
      }
    });
  for (auto &t : pool) t.join();
  if (counter == 16000) ok("std::mutex serialises eight threads (16000 increments)");
  else bad("std::mutex serialises eight threads", counter);
}

/* ---- 3. std::atomic across threads ------------------------------------ */

static void test_atomic() {
  std::atomic<long> n{0};
  std::vector<std::thread> pool;
  for (int i = 0; i < 4; i++)
    pool.emplace_back([&n] {
      for (int j = 0; j < 5000; j++) n.fetch_add(1, std::memory_order_relaxed);
    });
  for (auto &t : pool) t.join();
  if (n.load() == 20000) ok("std::atomic adds across four threads");
  else bad("std::atomic adds across four threads", n.load());
}

/* ---- 4. condition variable handshake --------------------------------- */

static void test_condvar() {
  std::mutex m;
  std::condition_variable cv;
  bool ready = false;
  int seen = 0;
  std::thread waiter([&] {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&ready] { return ready; });
    seen = 1;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  {
    std::lock_guard<std::mutex> lock(m);
    ready = true;
  }
  cv.notify_one();
  waiter.join();
  if (seen == 1) ok("std::condition_variable wakes a waiter");
  else bad("std::condition_variable wakes a waiter", seen);
}

/* ---- 5. thread_local with a destructor ------------------------------- */

static std::atomic<int> dtors{0};

struct Watch {
  int value;
  explicit Watch(int v) : value(v) {}
  ~Watch() { dtors.fetch_add(1); }
};

static thread_local Watch watch{7};

static void test_thread_local() {
  dtors = 0;
  std::thread t([] {
    if (watch.value != 7) return;
    watch.value = 8;
  });
  t.join();
  if (dtors.load() == 1) ok("a thread_local object is destructed at thread exit");
  else bad("a thread_local object is destructed at thread exit", dtors.load());
}

/* ---- 6. exceptions unwind through frames ----------------------------- */

static int thrower(int depth) {
  if (depth == 0) throw std::runtime_error("deep");
  return thrower(depth - 1);
}

static void test_exceptions() {
  int caught = 0;
  try {
    thrower(6);
  } catch (const std::runtime_error &e) {
    caught = std::string(e.what()) == "deep" ? 1 : 0;
  }
  if (caught == 1) ok("an exception unwinds six frames with its message intact");
  else bad("an exception unwinds six frames", caught);

  /* the same thing on a thread that was created for it */
  int in_thread = 0;
  std::thread t([&in_thread] {
    try {
      throw std::out_of_range("in a thread");
    } catch (const std::exception &e) {
      in_thread = std::string(e.what()) == "in a thread" ? 1 : 0;
    }
  });
  t.join();
  if (in_thread == 1) ok("an exception is caught on a spawned thread");
  else bad("an exception is caught on a spawned thread", in_thread);
}

/* ---- 7. containers, allocator, algorithms ---------------------------- */

static void test_containers() {
  std::vector<int> v;
  for (int i = 0; i < 5000; i++) v.push_back((i * 37) % 5000);
  std::sort(v.begin(), v.end());
  bool sorted = true;
  for (size_t i = 1; i < v.size(); i++)
    if (v[i - 1] > v[i]) sorted = false;
  if (sorted && v.size() == 5000) ok("std::vector and std::sort over 5000 ints");
  else bad("std::vector and std::sort over 5000 ints", (long)v.size());

  std::string s;
  for (int i = 0; i < 200; i++) s += "ab";
  std::map<std::string, int> m;
  m[s] = 1;
  if (s.size() == 400 && m.count(s) == 1) ok("std::string and std::map");
  else bad("std::string and std::map", (long)s.size());
}

/* ---- 8. clocks and streams ------------------------------------------- */

static void test_misc() {
  auto a = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - a)
                .count();
  if (us >= 15000) ok("std::chrono and sleep_for measure 20 ms");
  else bad("std::chrono and sleep_for measure 20 ms", (long)us);

  std::cout << "[cxx]      iostream is writing this line" << std::endl;
  ok("iostream writes");

  static std::once_flag once;
  int calls = 0;
  for (int i = 0; i < 8; i++)
    std::thread([&] { std::call_once(once, [&] { calls++; }); }).join();
  if (calls == 1) ok("std::call_once runs exactly once over eight threads");
  else bad("std::call_once runs exactly once over eight threads", calls);
}

int main() {
  std::printf("[cxx] C++ with threads, exceptions and libc++ on musl\n");
  test_thread();
  test_mutex();
  test_atomic();
  test_condvar();
  test_thread_local();
  test_exceptions();
  test_containers();
  test_misc();
  std::printf("[cxx] %d/%d\n", checks - failures, checks);
  return failures ? 1 : 0;
}