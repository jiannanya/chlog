#include <chlog/chlog.hpp>

#include <cstdint>
#include <cstdlib>

namespace {

class null_sink final : public chlog::sink {
public:
    void log(const chlog::log_event&) override {
        // Intentionally do nothing (benchmark logger overhead).
    }
    void flush() override {}
};

std::uint64_t parse_u64(const char* s, std::uint64_t fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s) return fallback;
    return static_cast<std::uint64_t>(v);
}

} // namespace

int main(int argc, char** argv) {
    using namespace chlog;

    // Usage: chlog_single_thread_bench [iterations]
    const std::uint64_t iterations = (argc >= 2) ? parse_u64(argv[1], 5'000'000ull) : 5'000'000ull;

    logger_config cfg;
    cfg.name = "st_bench";
    cfg.level = level::info;
    cfg.pattern = "{msg}";
    cfg.flush_on_level = level::critical;

    cfg.single_threaded = true;

    // These are ignored/forced off by single_threaded mode, but kept here to show intent.
    cfg.async.enabled = true;
    cfg.parallel_sinks = true;

    auto lg = std::make_shared<logger>(cfg);
    lg->add_sink(std::make_shared<null_sink>());

    // Warmup to stabilize codegen/caches.
    for (std::uint64_t i = 0; i < 1000; ++i) {
        lg->info("warmup {}", i);
    }

    const auto start = std::chrono::steady_clock::now();

    for (std::uint64_t i = 0; i < iterations; ++i) {
        // Tight loop: single-threaded fastest path.
        lg->info("v {}", i);
    }

    const auto end = std::chrono::steady_clock::now();
    lg->shutdown();

    const auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double seconds = static_cast<double>(dur_ns) / 1e9;

    const auto s = lg->stats();
    const double msgs_per_sec = seconds > 0.0 ? (static_cast<double>(s.dequeued) / seconds) : 0.0;

    std::cout << "Iterations:  " << iterations << "\n";
    std::cout << "Seconds:     " << seconds << "\n";
    std::cout << "Dequeued:    " << s.dequeued << "\n";
    std::cout << "Throughput:  " << msgs_per_sec << " msgs/s\n";
}
