#include <chlog/chlog.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(CHLOG_HAS_SPDLOG)
  #include <spdlog/async.h>
  #include <spdlog/details/null_mutex.h>
  #include <spdlog/sinks/base_sink.h>
  #include <spdlog/spdlog.h>
#endif

namespace {

using clock_t = std::chrono::steady_clock;

struct bench_config {
  std::uint64_t iters = 1'000'000;
};

std::optional<std::uint64_t> getenv_u64(const char* name) {
#if defined(_WIN32)
  std::size_t required = 0;
  if (::getenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
    return std::nullopt;
  }
  std::string buf(required, '\0');
  if (::getenv_s(&required, buf.data(), buf.size(), name) != 0) {
    return std::nullopt;
  }
  // getenv_s includes the null terminator in required.
  if (!buf.empty() && buf.back() == '\0') {
    buf.pop_back();
  }
  char* end = nullptr;
  const auto v = std::strtoull(buf.c_str(), &end, 10);
  if (end == buf.c_str()) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(v);
#else
  const char* s = std::getenv(name);
  if (!s || !*s) {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto v = std::strtoull(s, &end, 10);
  if (end == s) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(v);
#endif
}

bench_config parse_args(int argc, char** argv) {
  bench_config cfg;

  if (auto it = getenv_u64("CHLOG_BENCH_ITERS")) {
    cfg.iters = *it;
  }

  // CLI: --iters N (wins over env)
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--iters" && (i + 1) < argc) {
      cfg.iters = static_cast<std::uint64_t>(std::strtoull(argv[i + 1], nullptr, 10));
      ++i;
    }
  }

  if (cfg.iters == 0) {
    cfg.iters = 1;
  }

  return cfg;
}

struct run_result {
  std::string runner;
  std::string bench_case;
  std::uint64_t calls = 0;
  double seconds = 0.0;
  std::uint64_t processed = 0;
  std::uint64_t dropped = 0;

  double cps() const {
    if (seconds <= 0.0) {
      return 0.0;
    }
    return static_cast<double>(calls) / seconds;
  }
};

void print_result(const run_result& r) {
  std::cout << "RESULT"
            << " runner=" << r.runner
            << " case=" << r.bench_case
            << " calls=" << r.calls
            << " seconds=" << r.seconds
            << " cps=" << r.cps()
            << " processed=" << r.processed
            << " dropped=" << r.dropped
            << "\n";
}

std::uint64_t next_pow2_u64(std::uint64_t v) {
  if (v <= 1) {
    return 1;
  }
  --v;
  v |= (v >> 1);
  v |= (v >> 2);
  v |= (v >> 4);
  v |= (v >> 8);
  v |= (v >> 16);
  v |= (v >> 32);
  return v + 1;
}

// -------------------- chlog sinks --------------------

class chlog_counter_sink final : public chlog::sink {
public:
  explicit chlog_counter_sink(std::atomic<std::uint64_t>& processed) : processed_(&processed) {}

  void log(const chlog::log_event&) override {
    processed_->fetch_add(1, std::memory_order_relaxed);
  }

  void flush() override {}

private:
  std::atomic<std::uint64_t>* processed_;
};

run_result bench_chlog_sync(bool single_threaded, std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  chlog::logger_config cfg;
  cfg.name = single_threaded ? "chlog_sync_st" : "chlog_sync_mt";
  cfg.level = chlog::level::info;
  cfg.single_threaded = single_threaded;
  cfg.async.enabled = false;
  cfg.parallel_sinks = false;
  cfg.pattern = "{msg}";

  auto lg = std::make_shared<chlog::logger>(cfg);
  lg->add_sink(std::make_shared<chlog_counter_sink>(processed));

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "chlog";
  r.bench_case = single_threaded ? "sync_st" : "sync_mt";
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  lg->shutdown();
  return r;
}

run_result bench_chlog_filtered_out(std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  chlog::logger_config cfg;
  cfg.name = "chlog_filtered_out";
  cfg.level = chlog::level::warn;  // info is filtered out
  cfg.single_threaded = true;
  cfg.async.enabled = false;
  cfg.parallel_sinks = false;
  cfg.pattern = "{msg}";

  auto lg = std::make_shared<chlog::logger>(cfg);
  lg->add_sink(std::make_shared<chlog_counter_sink>(processed));

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "chlog";
  r.bench_case = "filtered_out";
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  lg->shutdown();
  return r;
}

run_result bench_chlog_async_mt(std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  chlog::logger_config cfg;
  cfg.name = "chlog_async_mt";
  cfg.level = chlog::level::info;
  cfg.single_threaded = false;
  cfg.async.enabled = true;
  {
    auto cap = next_pow2_u64(iters);
    if (cap > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
      cap = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    }
    cfg.async.queue_capacity = static_cast<std::uint32_t>(cap);
  }
  cfg.async.batch_max = 256;
  cfg.async.flush_every = std::chrono::milliseconds(0);
  cfg.parallel_sinks = false;
  cfg.pattern = "{msg}";

  auto lg = std::make_shared<chlog::logger>(cfg);
  lg->add_sink(std::make_shared<chlog_counter_sink>(processed));

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }

  // Wait until the async worker has processed everything.
  const auto deadline = t0 + std::chrono::seconds(30);
  while (processed.load(std::memory_order_relaxed) < iters && clock_t::now() < deadline) {
    std::this_thread::yield();
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "chlog";
  r.bench_case = "async_mt";
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  lg->shutdown();
  return r;
}


#if defined(CHLOG_HAS_SPDLOG)
// -------------------- spdlog sinks --------------------

template <typename Mutex>
class spdlog_counter_sink final : public spdlog::sinks::base_sink<Mutex> {
public:
  explicit spdlog_counter_sink(std::atomic<std::uint64_t>& processed) : processed_(&processed) {}

protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    processed_->fetch_add(1, std::memory_order_relaxed);
  }

  void flush_() override {}

private:
  std::atomic<std::uint64_t>* processed_;
};

run_result bench_spdlog_sync(bool single_threaded, std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  std::shared_ptr<spdlog::logger> lg;
  if (single_threaded) {
    auto sink = std::make_shared<spdlog_counter_sink<spdlog::details::null_mutex>>(processed);
    lg = std::make_shared<spdlog::logger>("spdlog_sync_st", spdlog::sinks_init_list{sink});
  } else {
    auto sink = std::make_shared<spdlog_counter_sink<std::mutex>>(processed);
    lg = std::make_shared<spdlog::logger>("spdlog_sync_mt", spdlog::sinks_init_list{sink});
  }

  lg->set_level(spdlog::level::info);
  lg->flush_on(spdlog::level::off);

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "spdlog";
  r.bench_case = single_threaded ? "sync_st" : "sync_mt";
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  return r;
}

run_result bench_spdlog_filtered_out(std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  auto sink = std::make_shared<spdlog_counter_sink<spdlog::details::null_mutex>>(processed);
  auto lg = std::make_shared<spdlog::logger>("spdlog_filtered_out", spdlog::sinks_init_list{sink});

  lg->set_level(spdlog::level::warn);  // info is filtered out

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "spdlog";
  r.bench_case = "filtered_out";
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  return r;
}

run_result bench_spdlog_async_mt(std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  // Create a dedicated thread pool for this benchmark.
  // Queue size chosen to keep overhead modest; the counter sink is fast, so it typically doesn't overflow.
  {
    auto q = next_pow2_u64(iters);
    if (q > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      q = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    }
    spdlog::init_thread_pool(static_cast<std::size_t>(q), 1);
  }

  auto sink = std::make_shared<spdlog_counter_sink<std::mutex>>(processed);

  auto lg = std::make_shared<spdlog::async_logger>(
      "spdlog_async_mt",
      spdlog::sinks_init_list{sink},
      spdlog::thread_pool(),
      spdlog::async_overflow_policy::block);

  lg->set_level(spdlog::level::info);

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }

  const auto deadline = t0 + std::chrono::seconds(30);
  while (processed.load(std::memory_order_relaxed) < iters && clock_t::now() < deadline) {
    std::this_thread::yield();
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "spdlog";
  r.bench_case = "async_mt";
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  spdlog::shutdown();
  return r;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const auto cfg = parse_args(argc, argv);

  // chlog
  print_result(bench_chlog_filtered_out(cfg.iters));
  print_result(bench_chlog_sync(true, cfg.iters));
  print_result(bench_chlog_sync(false, cfg.iters));
  print_result(bench_chlog_async_mt(cfg.iters));

#if defined(CHLOG_HAS_SPDLOG)
  // spdlog
  print_result(bench_spdlog_filtered_out(cfg.iters));
  print_result(bench_spdlog_sync(true, cfg.iters));
  print_result(bench_spdlog_sync(false, cfg.iters));
  print_result(bench_spdlog_async_mt(cfg.iters));
#else
  std::cerr << "NOTE: spdlog not available (build without CHLOG_HAS_SPDLOG).\n";
#endif

  return 0;
}
