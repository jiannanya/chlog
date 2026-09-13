#include <chlog/chlog.hpp>

#include <cstdlib>
#include <new>

#if defined(__GNUC__) && !defined(__clang__)
// The replacement new/delete pairs below deliberately use malloc/free.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

// Counts requested heap bytes during construction, not process RSS.
namespace {
std::atomic<bool> track_allocations{false};
std::atomic<std::size_t> allocated_bytes{0};
std::atomic<std::size_t> allocation_count{0};
void account(std::size_t n) {
    if (track_allocations.load(std::memory_order_relaxed)) {
        allocated_bytes.fetch_add(n, std::memory_order_relaxed);
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}
}
void* operator new(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { account(n); return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t alignment) {
#ifdef _WIN32
    void* p = _aligned_malloc(n ? n : 1, static_cast<std::size_t>(alignment));
#else
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(alignment), n ? n : 1) != 0) p = nullptr;
#endif
    if (!p) throw std::bad_alloc();
    account(n);
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

class render_sink final : public chlog::sink {
public:
    void log(const chlog::log_event& e) override { bytes += render(e).size(); }
    void buffered(const chlog::log_event& e) {
        chlog::detail::text_buffer buffer;
        render_to(e, buffer);
        buffer.push_back('\n');
        bytes += buffer.size();
    }
    std::size_t bytes = 0;
};

class count_sink final : public chlog::sink {
public:
    void log(const chlog::log_event&) override { ++count; }
    std::atomic<std::size_t> count{0};
};

int main(int argc, char** argv) {
    const auto iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    for (const auto pattern : {"{msg}", "[{date} {time}.{ms}][{lvl}][{name}] {msg}", "{json}"}) {
        render_sink output;
        output.set_thread_safe(false);
        output.set_pattern(pattern);
        chlog::log_event e;
        e.ts = std::chrono::system_clock::now();
        e.tid = std::this_thread::get_id();
        e.name = "render-bench";
        e.payload = "message with a moderately long payload for rendering";
        for (int i = 0; i < 1000; ++i) output.log(e);
        allocated_bytes = 0;
        allocation_count = 0;
        track_allocations = true;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) { e.seq = i; output.log(e); }
        const auto end = std::chrono::steady_clock::now();
        track_allocations = false;
        std::cout << "RENDER pattern=" << pattern << " iterations=" << iterations
                  << " seconds=" << std::chrono::duration<double>(end - start).count()
                  << " allocations=" << allocation_count.load() << " bytes=" << output.bytes << '\n';
        for (auto payload : {128u, 1024u}) {
            e.payload.assign(payload, 'x');
            allocation_count = 0;
            allocated_bytes = 0;
            track_allocations = true;
            const auto buffered_start = std::chrono::steady_clock::now();
            for (std::uint64_t i = 0; i < iterations; ++i) { e.seq = i; output.buffered(e); }
            const auto buffered_end = std::chrono::steady_clock::now();
            track_allocations = false;
            const auto expected = payload == 128 ? 0 : iterations;
            if (allocation_count != expected) return 1;
            std::cout << "BUFFER pattern=" << pattern << " payload=" << payload << " iterations=" << iterations
                      << " seconds=" << std::chrono::duration<double>(buffered_end - buffered_start).count()
                      << " allocations=" << allocation_count.load() << " allocated_bytes=" << allocated_bytes.load()
                      << " bytes=" << output.bytes << '\n';
        }
    }
    chlog::logger_config cfg;
    cfg.async.enabled = true;
    cfg.async.queue_capacity = 65536;
    allocated_bytes = 0;
    allocation_count = 0;
    track_allocations = true;
    chlog::logger logger(cfg);
    // Include the worker's batch storage; shutdown synchronizes its initialization.
    logger.shutdown();
    track_allocations = false;
    std::cout << "MEMORY queue_capacity=65536 allocated_bytes=" << allocated_bytes.load()
              << " allocations=" << allocation_count.load() << '\n';

    cfg.name = std::string(128, 'n');
    cfg.pattern = "{name} {msg}";
    cfg.async.drop_when_full = false;
    chlog::logger named(cfg);
    auto output = std::make_shared<count_sink>();
    named.add_sink(output);
    named.info("warmup");
    while (output->count == 0) std::this_thread::yield();
    allocated_bytes = 0;
    allocation_count = 0;
    track_allocations = true;
    for (std::uint64_t i = 0; i < iterations; ++i) named.info("message");
    named.shutdown();
    track_allocations = false;
    std::cout << "ASYNC_NAME iterations=" << iterations << " allocated_bytes=" << allocated_bytes.load()
              << " allocations=" << allocation_count.load() << " processed=" << output->count.load() << '\n';

    for (std::size_t count : {16u, 256u, 1024u}) {
        chlog::logger_config registration;
        registration.async.enabled = false;
        registration.parallel_sinks = false;
        registration.pattern = "{msg}";
        chlog::logger registered(registration);
        std::vector<std::shared_ptr<count_sink>> outputs;
        outputs.reserve(count);
        allocated_bytes = 0;
        allocation_count = 0;
        track_allocations = true;
        for (std::size_t i = 0; i < count; ++i) {
            auto added = std::make_shared<count_sink>();
            registered.add_sink(added);
            outputs.push_back(std::move(added));
        }
        track_allocations = false;
        std::cout << "REGISTRATION sinks=" << count << " allocated_bytes=" << allocated_bytes.load()
                  << " allocations=" << allocation_count.load() << '\n';
        registered.info("all sinks");
        registered.shutdown();
        for (auto& added : outputs) if (added->count != 1) return 1;
    }

    for (bool async : {false, true}) {
        chlog::logger_config full;
        full.single_threaded = !async;
        full.async.enabled = async;
        full.async.queue_capacity = 65536;
        full.async.drop_when_full = false;
        full.parallel_sinks = false;
        full.flush_on_level = chlog::level::off;
        chlog::logger end_to_end(full);
        auto rendered = std::make_shared<render_sink>();
        end_to_end.add_sink(rendered);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) end_to_end.info("message with a moderately long payload {}", i);
        end_to_end.shutdown();
        const auto end = std::chrono::steady_clock::now();
        std::cout << "END_TO_END case=" << (async ? "async_pattern" : "sync_pattern")
                  << " iterations=" << iterations << " seconds=" << std::chrono::duration<double>(end - start).count()
                  << " bytes=" << rendered->bytes << '\n';
    }
}
