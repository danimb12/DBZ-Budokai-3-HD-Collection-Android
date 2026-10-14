/**
 * @file        rex/core/android_profiler.cpp
 * @brief       Tiny sampling CPU profiler for the Android port (diagnostics).
 *
 * Enabled with the environment variable REX_PROFILE=1 (set by the Android
 * launcher). SIGPROF fires every ~4 ms of process CPU time; the handler stores
 * the interrupted thread id and program counter in a lock-free ring. A
 * background thread aggregates every 10 s and logs the hottest functions per
 * thread ("prof" lines), resolved with dladdr (exported symbols: the recompiled
 * guest functions are exported from libmain.so) or as module+offset.
 */
#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

namespace rex::profiler {
namespace {

struct Sample {
  uint32_t tid;
  uintptr_t pc;
};

constexpr size_t kRing = 1 << 16;
Sample g_ring[kRing];
std::atomic<uint64_t> g_head{0};

void OnSigprof(int, siginfo_t*, void* ctx) {
  auto* uc = static_cast<ucontext_t*>(ctx);
  const uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
  Sample& s = g_ring[i & (kRing - 1)];
  s.tid = uint32_t(syscall(SYS_gettid));
#if defined(__aarch64__)
  s.pc = uintptr_t(uc->uc_mcontext.pc);
#else
  s.pc = 0;
#endif
}

std::string ThreadName(uint32_t tid) {
  static std::unordered_map<uint32_t, std::string> cache;
  auto it = cache.find(tid);
  if (it != cache.end()) return it->second;
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
  std::string name = "?";
  if (FILE* f = fopen(path, "r")) {
    char buf[64] = {};
    if (fgets(buf, sizeof(buf), f)) {
      name = buf;
      while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
    }
    fclose(f);
  }
  cache[tid] = name;
  return name;
}

std::string Symbolize(uintptr_t pc) {
  Dl_info info = {};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* mod = strrchr(info.dli_fname, '/');
    mod = mod ? mod + 1 : info.dli_fname;
    char buf[256];
    if (info.dli_sname) {
      snprintf(buf, sizeof(buf), "%s!%s+0x%zx", mod, info.dli_sname,
               size_t(pc - uintptr_t(info.dli_saddr)));
    } else {
      snprintf(buf, sizeof(buf), "%s+0x%zx", mod, size_t(pc - uintptr_t(info.dli_fbase)));
    }
    return buf;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%zx", size_t(pc));
  return buf;
}

// Group samples by (thread, function start). dladdr's dli_saddr gives the
// function for exported symbols; otherwise bucket by 256-byte block.
uintptr_t FunctionKey(uintptr_t pc) {
  Dl_info info = {};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_saddr) {
    return uintptr_t(info.dli_saddr);
  }
  return pc & ~uintptr_t(0xFF);
}

void Aggregator() {
  uint64_t tail = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const uint64_t head = g_head.load(std::memory_order_acquire);
    if (head - tail > kRing) tail = head - kRing;
    const uint64_t n = head - tail;
    if (!n) continue;
    std::map<std::pair<uint32_t, uintptr_t>, uint32_t> counts;  // (tid, func) -> samples
    std::map<uint32_t, uint32_t> per_thread;
    std::unordered_map<uintptr_t, uintptr_t> key_cache;
    std::unordered_map<uintptr_t, uintptr_t> sample_pc;  // key -> a representative pc
    for (uint64_t i = tail; i < head; ++i) {
      const Sample s = g_ring[i & (kRing - 1)];
      auto kc = key_cache.find(s.pc);
      uintptr_t key;
      if (kc == key_cache.end()) {
        key = FunctionKey(s.pc);
        key_cache.emplace(s.pc, key);
      } else {
        key = kc->second;
      }
      sample_pc.emplace(key, s.pc);
      ++counts[{s.tid, key}];
      ++per_thread[s.tid];
    }
    tail = head;
    std::vector<std::pair<uint32_t, std::pair<uint32_t, uintptr_t>>> sorted;
    for (auto& [k, c] : counts) sorted.push_back({c, k});
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
    REXLOG_INFO("prof: {} samples in 10 s", n);
    std::vector<std::pair<uint32_t, uint32_t>> threads;
    for (auto& [tid, c] : per_thread) threads.push_back({c, tid});
    std::sort(threads.begin(), threads.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < threads.size() && i < 6; ++i) {
      REXLOG_INFO("prof: thread {} ({}) {:.1f}%", ThreadName(threads[i].second),
                  threads[i].second, 100.0 * threads[i].first / double(n));
    }
    for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
      const auto& [c, k] = sorted[i];
      REXLOG_INFO("prof: {:5.1f}% [{}] {}", 100.0 * c / double(n), ThreadName(k.first),
                  Symbolize(sample_pc[k.second]));
    }
  }
}

}  // namespace

void StartIfRequested() {
  const char* env = std::getenv("REX_PROFILE");
  if (!env || env[0] != '1') return;
  struct sigaction sa = {};
  sa.sa_sigaction = OnSigprof;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, nullptr) != 0) {
    REXLOG_WARN("prof: sigaction failed");
    return;
  }
  itimerval tv = {};
  tv.it_interval.tv_usec = 4000;
  tv.it_value.tv_usec = 4000;
  if (setitimer(ITIMER_PROF, &tv, nullptr) != 0) {
    REXLOG_WARN("prof: setitimer failed");
    return;
  }
  std::thread(Aggregator).detach();
  REXLOG_INFO("prof: sampling profiler enabled (SIGPROF every 4 ms of CPU)");
}

}  // namespace rex::profiler

#endif  // REX_PLATFORM_ANDROID
