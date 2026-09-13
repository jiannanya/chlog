#include <chlog/chlog.hpp>
#include <spdlog/async.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <semaphore>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

// C++ new diagnostics exclude malloc calls (including fmt's memory buffers).
// The default executable has no replacement allocator; use it for process memory.
// Run each library/case in a fresh process.
namespace allocation {
struct header { void* base; std::size_t bytes; bool tracked; };
std::atomic<bool> enabled{false};
std::atomic<std::size_t> live{0}, peak{0}, total{0}, count{0};
void* allocate(std::size_t bytes, std::size_t alignment) {
    alignment = (std::max)(alignment, alignof(header));
    if (bytes > (std::numeric_limits<std::size_t>::max)() - alignment - sizeof(header))
        throw std::bad_alloc();
    void* base = std::malloc(bytes + alignment + sizeof(header));
    if (!base) throw std::bad_alloc();
    auto address = (reinterpret_cast<std::uintptr_t>(base) + sizeof(header) + alignment - 1) & ~(alignment - 1);
    auto* info = reinterpret_cast<header*>(address) - 1;
    *info = {base, bytes, enabled.load(std::memory_order_relaxed)};
    if (info->tracked) {
        count.fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(bytes, std::memory_order_relaxed);
        const auto current = live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        auto previous = peak.load(std::memory_order_relaxed);
        while (previous < current && !peak.compare_exchange_weak(previous, current, std::memory_order_relaxed)) {}
    }
    return reinterpret_cast<void*>(address);
}
void release(void* ptr) noexcept {
    if (!ptr) return;
    auto* info = static_cast<header*>(ptr) - 1;
    if (info->tracked) live.fetch_sub(info->bytes, std::memory_order_relaxed);
    std::free(info->base);
}
}
#if defined(CHLOG_TRACK_CPP_ALLOCATIONS)
void* operator new(std::size_t n) { return allocation::allocate(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocation::allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { allocation::release(p); }
void operator delete[](void* p) noexcept { allocation::release(p); }
void operator delete(void* p, std::size_t) noexcept { allocation::release(p); }
void operator delete[](void* p, std::size_t) noexcept { allocation::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { allocation::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { allocation::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { allocation::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { allocation::release(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return ::operator new(n, t); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t& t) noexcept { return ::operator new(n, a, t); }
void operator delete(void* p, const std::nothrow_t&) noexcept { allocation::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { allocation::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation::release(p); }
#endif

namespace {
struct counter {
    std::atomic<std::size_t> processed{0};
    bool block = false;
    std::binary_semaphore entered{0}, resume{0};
    void log() {
        if (processed.fetch_add(1, std::memory_order_relaxed) == 0 && block) {
            entered.release();
            resume.acquire();
        }
    }
};
struct ch_sink final : chlog::view_sink {
    explicit ch_sink(counter& value) : value(value) {}
    void consume(const chlog::log_event_view&) override { value.log(); }
    counter& value;
};
struct sp_sink final : spdlog::sinks::sink {
    explicit sp_sink(counter& value) : value(value) {}
    void log(const spdlog::details::log_msg&) override { value.log(); }
    void flush() override {}
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}
    counter& value;
};
struct result {
    std::size_t resident = 0, peak = 0, allocations = 0, allocated_bytes = 0, retained = 0;
    std::size_t construction_peak = 0;
    std::size_t private_bytes = 0, peak_private_bytes = 0, peak_working_set_bytes = 0;
};
void process_memory(result& out) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    out.private_bytes = (std::max)(out.private_bytes, static_cast<std::size_t>(counters.PrivateUsage));
    out.peak_private_bytes = (std::max)(out.peak_private_bytes, static_cast<std::size_t>(counters.PeakPagefileUsage));
    out.peak_working_set_bytes = (std::max)(out.peak_working_set_bytes, static_cast<std::size_t>(counters.PeakWorkingSetSize));
#else
    (void)out;
#endif
}
template<class Log, class Drain>
result measure(Log log, Drain drain, counter& sink, std::size_t messages) {
    result out;
    out.resident = allocation::live.load();
    out.construction_peak = allocation::peak.load();
    process_memory(out);
    const auto start_count = allocation::count.load();
    const auto start_bytes = allocation::total.load();
    for (std::size_t i = 0; i < messages; ++i) {
        log();
        if (i == 0 && sink.block) sink.entered.acquire();
    }
    process_memory(out);
    if (sink.block) sink.resume.release();
    drain();
    process_memory(out);
    out.peak = allocation::peak.load();
    out.allocations = allocation::count.load() - start_count;
    out.allocated_bytes = allocation::total.load() - start_bytes;
    return out;
}
template<class Logger, class Factory, class Drain>
result measure_objects(std::size_t count, counter& sink, const std::string& payload, Factory create, Drain drain) {
    std::vector<std::shared_ptr<Logger>> loggers;
    loggers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) loggers.push_back(create());
    return measure([&] { for (auto& logger : loggers) logger->info("{}", payload); },
                   [&] { for (auto& logger : loggers) drain(*logger); }, sink, 1);
}

result measure_files(std::string_view runner, bool single_threaded, std::size_t count,
                     counter& sink, const std::string& payload) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("chlog-memory-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("benchmark directory exists");
    std::size_t index = 0;
    result out;
    if (runner == "chlog") {
        out = measure_objects<chlog::logger>(count, sink, payload, [&] {
            chlog::logger_config cfg;
            cfg.name = "memory"; cfg.pattern = "{msg}"; cfg.single_threaded = single_threaded;
            cfg.parallel_sinks = false; cfg.flush_on_level = chlog::level::off;
            auto logger = std::make_shared<chlog::logger>(cfg);
            logger->add_sink(std::make_shared<chlog::rotating_file_sink>(
                directory / (std::to_string(index++) + ".log"), 1u << 20, 1));
            return logger;
        }, [](chlog::logger& logger) {
            logger.shutdown();
            const auto stats = logger.stats();
            if (stats.enqueued != 1 || stats.dequeued != 1 || stats.errors)
                throw std::runtime_error("Incomplete file benchmark");
        });
    } else {
        out = measure_objects<spdlog::logger>(count, sink, payload, [&] {
            const auto path = (directory / (std::to_string(index++) + ".log")).string();
            std::shared_ptr<spdlog::sinks::sink> output;
            if (single_threaded) output = std::make_shared<spdlog::sinks::rotating_file_sink_st>(path, 1u << 20, 1);
            else output = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, 1u << 20, 1);
            output->set_formatter(std::make_unique<spdlog::pattern_formatter>("%v", spdlog::pattern_time_type::local, "\n"));
            return std::make_shared<spdlog::logger>("memory", output);
        }, [](spdlog::logger& logger) { logger.flush(); });
    }
    // Validation and cleanup follow the last memory snapshot. The logger vector
    // has been destroyed, so this also checks implicit file close/draining.
    for (std::size_t i = 0; i < count; ++i) {
        const auto path = directory / (std::to_string(i) + ".log");
        {
            std::ifstream input(path, std::ios::binary);
            const std::string content(std::istreambuf_iterator<char>{input}, {});
            if (!input || content != payload + '\n') throw std::runtime_error("Incorrect file benchmark output");
        }
        sink.processed.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::remove(path);
    }
    std::filesystem::remove(directory);
    return out;
}
}
int main(int argc, char** argv) {
    std::printf("SIZES chlog_logger=%zu chlog_sink=%zu chlog_config=%zu chlog_metrics=%zu spdlog_logger=%zu spdlog_sink=%zu\n",
        sizeof(chlog::logger), sizeof(chlog::sink), sizeof(chlog::logger_config), sizeof(chlog::metrics), sizeof(spdlog::logger), sizeof(spdlog::sinks::sink));
    if (argc != 4) {
        std::fprintf(stderr, "usage: chlog_bench_memory chlog|spdlog sync_st|sync_mt|async|backlog payload_bytes\n"
                            "       chlog_bench_memory chlog|spdlog objects_st|objects_mt object_count\n"
                            "       chlog_bench_memory chlog|spdlog file_st|file_mt file_count (1..128)\n");
        return 2;
    }
    const std::string_view runner = argv[1], mode = argv[2];
    const bool files = mode == "file_st" || mode == "file_mt";
    if ((runner != "chlog" && runner != "spdlog") ||
        (mode != "sync_st" && mode != "sync_mt" && mode != "async" && mode != "backlog" && mode != "objects_st" && mode != "objects_mt" && !files)) return 2;
    const bool multiple = files || mode == "objects_st" || mode == "objects_mt";
    const auto parameter = std::strtoull(argv[3], nullptr, 10);
    if (parameter == 0 || parameter > 65536 || (files && parameter > 128)) return 2;
    const std::size_t objects = multiple ? static_cast<std::size_t>(parameter) : 1;
    const auto bytes = multiple ? 13 : parameter;
    const bool async = mode == "async" || mode == "backlog";
    constexpr std::size_t capacity = 65536;
    const std::size_t messages = multiple ? objects : mode == "backlog" ? capacity + 1 : 10000;
    const std::string payload(static_cast<std::size_t>(bytes), 'x');
    counter sink;
    sink.block = mode == "backlog";
    result out;
    // Input storage and harness synchronization objects precede tracking.
    allocation::enabled.store(true);
    if (files) {
        out = measure_files(runner, mode == "file_st", objects, sink, payload);
    } else if (multiple && runner == "chlog") {
        out = measure_objects<chlog::logger>(objects, sink, payload, [&] {
            chlog::logger_config cfg;
            cfg.name = "memory"; cfg.pattern = "{msg}";
            cfg.single_threaded = mode == "objects_st"; cfg.parallel_sinks = false;
            auto logger = std::make_shared<chlog::logger>(std::move(cfg));
            logger->add_sink(std::make_shared<ch_sink>(sink));
            return logger;
        }, [](chlog::logger& logger) {
            logger.shutdown();
            const auto stats = logger.stats();
            if (stats.enqueued != 1 || stats.dequeued != 1 || stats.dropped != 0)
                throw std::runtime_error("Incomplete object benchmark");
        });
    } else if (multiple) {
        out = measure_objects<spdlog::logger>(objects, sink, payload, [&] {
            return std::make_shared<spdlog::logger>("memory", std::make_shared<sp_sink>(sink));
        }, [](spdlog::logger& logger) { logger.flush(); });
    } else if (runner == "chlog") {
        chlog::logger_config cfg;
        cfg.name = "memory";
        cfg.pattern = "{msg}";
        cfg.single_threaded = mode == "sync_st";
        cfg.parallel_sinks = false;
        cfg.async.enabled = async;
        cfg.async.weighted_queue = false;
        cfg.async.queue_capacity = capacity;
        cfg.async.batch_max = 256;
        cfg.async.drop_when_full = false;
        cfg.async.flush_every = std::chrono::milliseconds(0);
        auto lg = std::make_shared<chlog::logger>(std::move(cfg));
        lg->add_sink(std::make_shared<ch_sink>(sink));
        out = measure([&] { lg->info("{}", payload); }, [&] { lg->shutdown(); }, sink, messages);
        const auto stats = lg->stats();
        if (stats.enqueued != messages || stats.dequeued != messages || stats.dropped != 0) return 1;
    } else {
        std::shared_ptr<spdlog::details::thread_pool> pool;
        std::shared_ptr<spdlog::logger> lg;
        auto output = std::make_shared<sp_sink>(sink);
        if (async) {
            pool = std::make_shared<spdlog::details::thread_pool>(capacity, 1);
            lg = std::make_shared<spdlog::async_logger>("memory", output, pool, spdlog::async_overflow_policy::block);
        } else lg = std::make_shared<spdlog::logger>("memory", output);
        out = measure([&] { lg->info("{}", payload); }, [&] { if (pool) pool.reset(); else lg->flush(); }, sink, messages);
    }
    out.retained = allocation::live.load();
    allocation::enabled.store(false);
    std::printf("MEMORY runner=%s case=%s payload_bytes=%zu calls=%zu resident_bytes=%zu peak_bytes=%zu allocations=%zu allocated_bytes=%zu retained_bytes=%zu processed=%zu private_bytes=%zu peak_private_bytes=%zu peak_working_set_bytes=%zu construction_peak_bytes=%zu objects=%zu\n",
        argv[1], argv[2], payload.size(), messages, out.resident, out.peak, out.allocations,
        out.allocated_bytes, out.retained, sink.processed.load(), out.private_bytes, out.peak_private_bytes, out.peak_working_set_bytes,
        out.construction_peak, objects);
    return sink.processed.load() == messages ? 0 : 1;
}
