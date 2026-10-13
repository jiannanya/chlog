#include <chlog/chlog.hpp>

#include <barrier>
#include <cstdlib>

// Focused, dependency-free benchmarks. Build this same source against both
// headers when comparing revisions; the timed work includes sink consumption.
namespace {
using clock_type = std::chrono::steady_clock;
std::vector<std::string_view> selected_cases;

bool selected(std::string_view name) {
    return selected_cases.empty() || std::find(selected_cases.begin(), selected_cases.end(), name) != selected_cases.end();
}

struct counting_sink final : chlog::view_sink {
    std::size_t bytes = 0, records = 0;
    void consume(const chlog::log_event_view& event) override {
        bytes += event.payload.size();
        ++records;
    }
};

template <class Work>
void measure(std::string_view name, std::size_t iterations, Work work) {
    const auto start = clock_type::now();
    const auto bytes = work();
    const auto seconds = std::chrono::duration<double>(clock_type::now() - start).count();
    std::cout << "HOTPATH case=" << name << " iterations=" << iterations
              << " seconds=" << seconds << " bytes=" << bytes << '\n';
}

template <class Write>
void logging(std::string_view name, std::size_t iterations, Write write) {
    if (!selected(name)) return;
    chlog::logger_config config;
    config.pattern = "{msg}";
    config.single_threaded = true;
    config.flush_on_level = chlog::level::off;
    chlog::logger logger(config);
    auto output = std::make_shared<counting_sink>();
    logger.add_sink(output);
    for (std::size_t i = 0; i < 1000; ++i) write(logger, i);
    output->bytes = output->records = 0;
    measure(name, iterations, [&] {
        for (std::size_t i = 0; i < iterations; ++i) write(logger, i);
        logger.shutdown();
        if (output->records != iterations || logger.stats().errors != 0)
            throw std::runtime_error("logging benchmark lost records");
        return output->bytes;
    });
}

void async_rendering(std::size_t iterations) {
    if (!selected("async_pattern_8")) return;
    struct rendered_sink final : chlog::view_sink {
        std::size_t bytes = 0, records = 0;
        void consume(const chlog::log_event_view& event) override {
            chlog::detail::text_buffer buffer;
            render_to(event, buffer);
            bytes += buffer.size();
            ++records;
        }
    };
    chlog::logger_config config;
    config.async.enabled = true;
    config.async.drop_when_full = false;
    config.async.weighted_queue = false;
    config.async.queue_capacity = 65536;
    config.async.flush_every = std::chrono::milliseconds(0);
    config.flush_on_level = chlog::level::off;
    chlog::logger logger(config);
    auto output = std::make_shared<rendered_sink>();
    logger.add_sink(output);
    std::barrier start(9);
    std::vector<std::thread> producers;
    for (std::size_t t = 0; t < 8; ++t) producers.emplace_back([&, t] {
        start.arrive_and_wait();
        for (std::size_t i = t; i < iterations; i += 8) logger.info("message {}", i);
    });
    measure("async_pattern_8", iterations, [&] {
        start.arrive_and_wait();
        for (auto& producer : producers) producer.join();
        logger.shutdown();
        if (output->records != iterations || logger.stats().errors || logger.stats().dropped)
            throw std::runtime_error("async benchmark lost records");
        return output->bytes;
    });
}
}

int main(int argc, char** argv) try {
    const auto iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
    if (!iterations) return 2;
    for (int i = 2; i < argc; ++i) selected_cases.emplace_back(argv[i]);
    logging("bare_integer", iterations, [](auto& logger, auto i) { logger.info("value {}", i); });
    logging("format_specs", iterations, [](auto& logger, auto i) {
        logger.info("hex {:08x} float {:.3f} padded {:>8}", i, 1.2345, "text");
    });
    const std::string payload(1024, 'x');
    logging("format_long", iterations, [&](auto& logger, auto) { logger.info("[{:>1028}]", payload); });
    logging("runtime_specs", iterations, [](auto& logger, auto i) {
        logger.info(std::string_view("hex {:08x} float {:.3f} padded {:>8}"), i, 1.2345, "text");
    });
    async_rendering(iterations);

    // Keep all threads alive until their IDs have been consumed: platforms may
    // recycle IDs immediately after join. No representation assumptions.
    std::array<std::thread::id, 32> ids;
    std::binary_semaphore done{0};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < ids.size(); ++i)
        threads.emplace_back([&] {
            done.acquire();
            done.release();
        });
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = threads[i].get_id();
    for (std::size_t count : {1u, 8u, 32u}) {
        if (!selected("thread_ids_" + std::to_string(count))) continue;
        measure("thread_ids_" + std::to_string(count), iterations, [&] {
            std::size_t bytes = 0;
            for (std::size_t i = 0; i < iterations; ++i)
                bytes += chlog::cached_thread_id(ids[i % count]).size();
            return bytes;
        });
    }
    done.release();
    for (auto& thread : threads) thread.join();

    for (bool escaped : {false, true}) {
        if (!selected(escaped ? "json_controls" : "json_ascii")) continue;
        std::string message(1024, 'x');
        if (escaped) for (std::size_t i = 0; i < message.size(); ++i)
            message[i] = static_cast<char>(i % 32);
        measure(escaped ? "json_controls" : "json_ascii", iterations, [&] {
            std::size_t bytes = 0;
            for (std::size_t i = 0; i < iterations; ++i) {
                chlog::detail::text_buffer buffer;
                chlog::detail::append_json_escaped(buffer, message);
                bytes += buffer.size();
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            return bytes;
        });
    }

    const auto dir = std::filesystem::temp_directory_path() /
        ("chlog-hotpaths-" + std::to_string(clock_type::now().time_since_epoch().count()));
    struct cleanup {
        std::filesystem::path path;
        ~cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{dir};
    for (std::size_t size : {128u, 4096u}) {
        if (!selected("daily_" + std::to_string(size))) continue;
        const auto output_dir = dir / std::to_string(size);
        chlog::daily_file_sink output(output_dir);
        output.set_thread_safe(false);
        output.set_pattern("{msg}");
        chlog::log_event event;
        event.ts = std::chrono::system_clock::now();
        event.payload.assign(size, 'x');
        // Bound disk traffic independently of the CPU microbench iteration count.
        const auto calls = (std::min)(iterations, 20000ULL);
        measure("daily_" + std::to_string(size), calls, [&] {
            for (std::size_t i = 0; i < calls; ++i) output.log(event);
            output.flush();
            return calls * (size + 1);
        });
        std::ifstream file(output_dir / (chlog::date_string(event.ts) + ".log"), std::ios::binary);
        std::string line;
        std::size_t records = 0;
        while (std::getline(file, line)) {
            if (line != event.payload) throw std::runtime_error("file benchmark corrupted record");
            ++records;
        }
        if (records != calls) throw std::runtime_error("file benchmark lost records");
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
