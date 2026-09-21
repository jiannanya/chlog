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
  #include <spdlog/sinks/sink.h>
  #include <spdlog/spdlog.h>
  #include <spdlog/version.h>
#endif

namespace {

using clock_t = std::chrono::steady_clock;

struct bench_config {
  std::uint64_t iters = 1'000'000;
  bool spdlog_first = false;
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
    if (a == "--spdlog-first") cfg.spdlog_first = true;
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

bool benchmark_failed = false;

void print_result(const run_result& r) {
  const auto expected = r.bench_case == "filtered_out" ? 0 : r.calls;
  if (r.processed != expected || r.dropped != 0) benchmark_failed = true;
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

// -------------------- chlog sinks --------------------

class chlog_counter_sink final : public chlog::view_sink {
public:
  explicit chlog_counter_sink(std::atomic<std::uint64_t>& processed) : processed_(&processed) {}

  void consume(const chlog::log_event_view&) override {
    processed_->fetch_add(1, std::memory_order_relaxed);
  }

  void flush() override {}

private:
  std::atomic<std::uint64_t>* processed_;
};

template <bool Plain = false, std::size_t PayloadBytes = 0>
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

  const std::string payload(PayloadBytes, 'x');
  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    if constexpr (PayloadBytes != 0) lg->info("{}", payload);
    else if constexpr (Plain) lg->info("message ready");
    else lg->info("v {}", i);
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "chlog";
  r.bench_case = single_threaded ? "sync_st" : "sync_mt";
  if constexpr (Plain) r.bench_case += "_literal";
  if constexpr (PayloadBytes != 0) r.bench_case += "_payload" + std::to_string(PayloadBytes);
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  lg->shutdown();
  return r;
}

// `parallel_sinks` defaults to true, so this is the out-of-the-box configuration
// of a synchronous logger and deserves its own cases. Each record is dispatched
// once per sink, hence the per-record normalization of the processed count.
// A single destination is written by the calling thread, so `sync_parallel1`
// measures the auto-mode fallback rather than the pool.
template <unsigned Sinks>
run_result bench_chlog_parallel(std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  chlog::logger_config cfg;
  cfg.name = "chlog_parallel";
  cfg.level = chlog::level::info;
  cfg.single_threaded = false;
  cfg.async.enabled = false;
  cfg.parallel_sinks = true;
  cfg.pattern = "{msg}";

  auto lg = std::make_shared<chlog::logger>(cfg);
  for (unsigned i = 0; i < Sinks; ++i) {
    lg->add_sink(std::make_shared<chlog_counter_sink>(processed));
  }

  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    lg->info("v {}", i);
  }
  // Include draining, matching the async cases' timing boundary.
  lg->shutdown();
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "chlog";
  r.bench_case = "sync_parallel" + std::to_string(Sinks);
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed) / Sinks;
  r.dropped = 0;
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

template <bool Plain = false, std::size_t PayloadBytes = 0>
run_result bench_chlog_async_mt(std::uint64_t iters, unsigned producers = 1) {
  std::atomic<std::uint64_t> processed{0};

  chlog::logger_config cfg;
  cfg.name = "chlog_async_mt";
  cfg.level = chlog::level::info;
  cfg.single_threaded = false;
  cfg.async.enabled = true;
  cfg.async.queue_capacity = 65536;
  cfg.async.weighted_queue = false; // Same FIFO capacity as spdlog.
  cfg.async.drop_when_full = false;
  cfg.async.batch_max = 256;
  cfg.async.flush_every = std::chrono::milliseconds(0);
  cfg.parallel_sinks = false;
  cfg.pattern = "{msg}";

  auto lg = std::make_shared<chlog::logger>(cfg);
  lg->add_sink(std::make_shared<chlog_counter_sink>(processed));

  const std::string payload(PayloadBytes, 'x');
  const auto t0 = clock_t::now();
  auto produce = [&](unsigned thread) {
    for (std::uint64_t i = thread; i < iters; i += producers) {
      if constexpr (PayloadBytes != 0) lg->info("{}", payload);
    else if constexpr (Plain) lg->info("message ready");
      else lg->info("v {}", i);
    }
  };
  if (producers == 1) produce(0);
  else {
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < producers; ++t) threads.emplace_back(produce, t);
    for (auto& t : threads) t.join();
  }
  // Include draining, without a busy loop or an iteration-sized allocation.
  lg->shutdown();
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "chlog";
  r.bench_case = producers == 1 ? "async_mt" : "async_4p";
  if constexpr (Plain) r.bench_case += "_literal";
  if constexpr (PayloadBytes != 0) r.bench_case += "_payload" + std::to_string(PayloadBytes);
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = lg->stats().dropped;

  return r;
}


#if defined(CHLOG_HAS_SPDLOG)
// -------------------- spdlog sinks --------------------

class spdlog_counter_sink final : public spdlog::sinks::sink {
public:
  explicit spdlog_counter_sink(std::atomic<std::uint64_t>& processed) : processed_(&processed) {}

  void log(const spdlog::details::log_msg&) override {
    processed_->fetch_add(1, std::memory_order_relaxed);
  }

  void flush() override {}
  void set_pattern(const std::string&) override {}
  void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

private:
  std::atomic<std::uint64_t>* processed_;
};

template <bool Plain = false, std::size_t PayloadBytes = 0>
run_result bench_spdlog_sync(bool single_threaded, std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  // Both libraries' sinks do exactly one atomic increment and no formatting.
  auto sink = std::make_shared<spdlog_counter_sink>(processed);
  auto lg = std::make_shared<spdlog::logger>(single_threaded ? "spdlog_sync_st" : "spdlog_sync_mt",
                                           spdlog::sinks_init_list{sink});

  lg->set_level(spdlog::level::info);
  lg->flush_on(spdlog::level::off);

  const std::string payload(PayloadBytes, 'x');
  const auto t0 = clock_t::now();
  for (std::uint64_t i = 0; i < iters; ++i) {
    if constexpr (PayloadBytes != 0) lg->info("{}", payload);
    else if constexpr (Plain) lg->info("message ready");
    else lg->info("v {}", i);
  }
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "spdlog";
  r.bench_case = single_threaded ? "sync_st" : "sync_mt";
  if constexpr (Plain) r.bench_case += "_literal";
  if constexpr (PayloadBytes != 0) r.bench_case += "_payload" + std::to_string(PayloadBytes);
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  return r;
}

run_result bench_spdlog_filtered_out(std::uint64_t iters) {
  std::atomic<std::uint64_t> processed{0};

  auto sink = std::make_shared<spdlog_counter_sink>(processed);
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

template <bool Plain = false, std::size_t PayloadBytes = 0>
run_result bench_spdlog_async_mt(std::uint64_t iters, unsigned producers = 1) {
  std::atomic<std::uint64_t> processed{0};

  // Fixed capacity matches chlog and keeps memory independent of iterations.
  auto pool = std::make_shared<spdlog::details::thread_pool>(65536, 1);

  auto sink = std::make_shared<spdlog_counter_sink>(processed);

  auto lg = std::make_shared<spdlog::async_logger>(
      "spdlog_async_mt",
      spdlog::sinks_init_list{sink},
      pool,
      spdlog::async_overflow_policy::block);

  lg->set_level(spdlog::level::info);
  lg->flush_on(spdlog::level::off);

  const std::string payload(PayloadBytes, 'x');
  const auto t0 = clock_t::now();
  auto produce = [&](unsigned thread) {
    for (std::uint64_t i = thread; i < iters; i += producers) {
      if constexpr (PayloadBytes != 0) lg->info("{}", payload);
    else if constexpr (Plain) lg->info("message ready");
      else lg->info("v {}", i);
    }
  };
  if (producers == 1) produce(0);
  else {
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < producers; ++t) threads.emplace_back(produce, t);
    for (auto& t : threads) t.join();
  }
  // Destruction posts a termination message after all records and joins the worker.
  // Include draining and worker shutdown, matching the chlog timing boundary.
  pool.reset();
  const auto t1 = clock_t::now();

  run_result r;
  r.runner = "spdlog";
  r.bench_case = producers == 1 ? "async_mt" : "async_4p";
  if constexpr (Plain) r.bench_case += "_literal";
  if constexpr (PayloadBytes != 0) r.bench_case += "_payload" + std::to_string(PayloadBytes);
  r.calls = iters;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.processed = processed.load(std::memory_order_relaxed);
  r.dropped = 0;

  return r;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const auto cfg = parse_args(argc, argv);

  std::cout << "META backend=";
#if defined(CHLOG_USE_FMT)
  std::cout << "fmt fmt=" << FMT_VERSION / 10000 << '.' << FMT_VERSION / 100 % 100 << '.' << FMT_VERSION % 100;
#else
  std::cout << "std";
#endif
#if defined(CHLOG_HAS_SPDLOG)
  std::cout << " spdlog=" << SPDLOG_VER_MAJOR << '.' << SPDLOG_VER_MINOR << '.' << SPDLOG_VER_PATCH;
#endif
#if defined(__clang__)
  std::cout << " compiler=clang-" << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(_MSC_VER)
  std::cout << " compiler=msvc-" << _MSC_VER;
#elif defined(__GNUC__)
  std::cout << " compiler=gcc-" << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#endif
  std::cout << " queue_capacity=65536 overflow=block sink=atomic_counter\n";

  auto run_chlog = [&] {
  print_result(bench_chlog_filtered_out(cfg.iters));
  print_result(bench_chlog_sync(true, cfg.iters));
  print_result(bench_chlog_sync(false, cfg.iters));
  print_result(bench_chlog_parallel<1>(cfg.iters));
  print_result(bench_chlog_parallel<2>(cfg.iters));
  print_result(bench_chlog_parallel<4>(cfg.iters));
  print_result(bench_chlog_async_mt(cfg.iters));
  print_result(bench_chlog_async_mt(cfg.iters, 4));
  print_result(bench_chlog_sync<true>(true, cfg.iters));
  print_result(bench_chlog_sync<true>(false, cfg.iters));
  print_result(bench_chlog_async_mt<true>(cfg.iters));
  print_result(bench_chlog_sync<false, 128>(true, cfg.iters));
  print_result(bench_chlog_sync<false, 128>(false, cfg.iters));
  print_result(bench_chlog_sync<false, 1024>(true, cfg.iters));
  print_result(bench_chlog_sync<false, 1024>(false, cfg.iters));
  print_result(bench_chlog_async_mt<false, 128>(cfg.iters));
  print_result(bench_chlog_async_mt<false, 1024>(cfg.iters));
  };

#if defined(CHLOG_HAS_SPDLOG)
  auto run_spdlog = [&] {
  print_result(bench_spdlog_filtered_out(cfg.iters));
  print_result(bench_spdlog_sync(true, cfg.iters));
  print_result(bench_spdlog_sync(false, cfg.iters));
  print_result(bench_spdlog_async_mt(cfg.iters));
  print_result(bench_spdlog_async_mt(cfg.iters, 4));
  print_result(bench_spdlog_sync<true>(true, cfg.iters));
  print_result(bench_spdlog_sync<true>(false, cfg.iters));
  print_result(bench_spdlog_async_mt<true>(cfg.iters));
  print_result(bench_spdlog_sync<false, 128>(true, cfg.iters));
  print_result(bench_spdlog_sync<false, 128>(false, cfg.iters));
  print_result(bench_spdlog_sync<false, 1024>(true, cfg.iters));
  print_result(bench_spdlog_sync<false, 1024>(false, cfg.iters));
  print_result(bench_spdlog_async_mt<false, 128>(cfg.iters));
  print_result(bench_spdlog_async_mt<false, 1024>(cfg.iters));
  };
  if (cfg.spdlog_first) { run_spdlog(); run_chlog(); }
  else { run_chlog(); run_spdlog(); }
#else
  run_chlog();
  std::cerr << "NOTE: spdlog not available (build without CHLOG_HAS_SPDLOG).\n";
#endif

  return benchmark_failed ? 1 : 0;
}
