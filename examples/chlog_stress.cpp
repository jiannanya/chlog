#include <chlog/chlog.hpp>

int main() {
    using namespace chlog;

    auto console = std::make_shared<console_sink>(console_sink::style::plain);
    auto file    = std::make_shared<rotating_file_sink>("logs/stress.log", 32 * 1024 * 1024, 5);
    auto json     = std::make_shared<json_sink>("logs/stress.json");
    auto daily    = std::make_shared<daily_file_sink>("logs/daily");

    logger_config cfg;
    cfg.name = "stress";
    cfg.level = level::trace;
    cfg.pattern = "[{date} {time}.{ms}][{lvl}][tid={tid}][{name}] {msg}";
    cfg.flush_on_level = level::warn;

    cfg.async.enabled = true;
    cfg.async.queue_capacity = 1 << 16; // 65536
    cfg.async.batch_max = 256;
    cfg.async.flush_every = std::chrono::milliseconds(200);
    cfg.async.drop_when_full = true;
    cfg.async.weighted_queue = true;

    cfg.parallel_sinks = true;
    cfg.sink_pool_size = 0; // 0 = sinks.size()

    auto lg = std::make_shared<logger>(cfg);
    lg->add_sink(console);
    lg->add_sink(file);
    lg->add_sink(json);
    lg->add_sink(daily);

    constexpr int threads = 20;
    constexpr int messages = 15000;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> producers;
    producers.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < messages; ++i) {
                // Mixed levels to simulate realistic workloads.
                if ((i % 1000) == 0) {
                    lg->error("E thread={} i={}", t, i);
                } else if ((i % 200) == 0) {
                    lg->warn("W thread={} i={}", t, i);
                } else if ((i % 5) == 0) {
                    lg->info("I thread={} i={}", t, i);
                } else {
                    lg->debug("D thread={} i={}", t, i);
                }
            }
        });
    }

    for (auto& th : producers) th.join();

    lg->shutdown();

    auto end = std::chrono::steady_clock::now();
    const auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    const auto s = lg->stats();
    std::cout << "Total time: " << dur_ms << " ms\n";
    std::cout << "Enqueued:   " << s.enqueued << "\n";
    std::cout << "Dequeued:   " << s.dequeued << "\n";
    std::cout << "Dropped:    " << s.dropped << "\n";
    std::cout << "Flushed:    " << s.flushed << "\n";
    std::cout << "Queue size: " << s.queue_size << "\n";

    std::cout << "Throughput: " << (s.dequeued * 1000.0 / dur_ms) << " msgs/s\n";
}
