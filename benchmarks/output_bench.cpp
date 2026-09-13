#include <chlog/chlog.hpp>

#include <cstdlib>
#include <iomanip>

#if defined(CHLOG_HAS_SPDLOG)
#include <spdlog/async.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/version.h>
#endif

namespace {
using bench_clock = std::chrono::steady_clock;

void verify(const std::filesystem::path& path, std::string_view payload, std::uint64_t calls,
            bool pattern) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read benchmark output");
    std::string line;
    std::uint64_t count = 0;
    while (std::getline(input, line)) {
        if (pattern ? line.size() != payload.size() + 34 || !line.ends_with(payload)
                    : line != payload)
            throw std::runtime_error("incorrect benchmark output");
        ++count;
    }
    if (!input.eof() || count != calls) throw std::runtime_error("missing benchmark records");
}

void run(std::string_view library, int mode, bool pattern, std::size_t payload_bytes,
         std::uint64_t calls, const std::filesystem::path& directory, bool report = true) {
    const auto path = directory / "output.log";
    const std::string payload(payload_bytes, 'x');
    double seconds = 0;
    const auto max_bytes = (std::numeric_limits<std::size_t>::max)() / 2;
    if (library == "chlog") {
        chlog::logger_config cfg;
        cfg.name = "output";
        cfg.pattern = pattern ? "[{date} {time}.{ms}][{name}] {msg}" : "{msg}";
        cfg.single_threaded = mode == 0;
        cfg.parallel_sinks = false;
        cfg.flush_on_level = chlog::level::off;
        cfg.async.enabled = mode == 2;
        cfg.async.queue_capacity = 65536;
        cfg.async.weighted_queue = false;
        cfg.async.drop_when_full = false;
        cfg.async.flush_every = std::chrono::milliseconds(0);
        chlog::logger logger(cfg);
        logger.add_sink(std::make_shared<chlog::rotating_file_sink>(path, max_bytes, 1));
        const auto begin = bench_clock::now();
        for (std::uint64_t i = 0; i < calls; ++i) logger.info("{}", payload);
        logger.shutdown();
        seconds = std::chrono::duration<double>(bench_clock::now() - begin).count();
        const auto stats = logger.stats();
        if (stats.errors || stats.dropped || stats.dequeued != calls)
            throw std::runtime_error("chlog benchmark delivery failed");
    }
#if defined(CHLOG_HAS_SPDLOG)
    else {
        std::shared_ptr<spdlog::sinks::sink> output;
        if (mode == 0) output = std::make_shared<spdlog::sinks::rotating_file_sink_st>(path.string(), max_bytes, 1);
        else output = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path.string(), max_bytes, 1);
        output->set_formatter(std::make_unique<spdlog::pattern_formatter>(
            pattern ? "[%Y-%m-%d %H:%M:%S.%e][%n] %v" : "%v",
            spdlog::pattern_time_type::local, "\n"));
        std::shared_ptr<spdlog::details::thread_pool> pool;
        std::shared_ptr<spdlog::logger> logger;
        if (mode == 2) {
            pool = std::make_shared<spdlog::details::thread_pool>(65536, 1);
            logger = std::make_shared<spdlog::async_logger>("output", output, pool,
                spdlog::async_overflow_policy::block);
        } else logger = std::make_shared<spdlog::logger>("output", output);
        logger->flush_on(spdlog::level::off);
        logger->set_error_handler([](const std::string& message) { throw std::runtime_error(message); });
        const auto begin = bench_clock::now();
        for (std::uint64_t i = 0; i < calls; ++i) logger->info("{}", payload);
        logger->flush();
        pool.reset(); // Drain and join the async worker before stopping the clock.
        seconds = std::chrono::duration<double>(bench_clock::now() - begin).count();
    }
#endif
    verify(path, payload, calls, pattern);
    const auto bytes = std::filesystem::file_size(path);
    std::filesystem::remove(path);
    if (!report) return;
    std::cout << std::setprecision(10) << "OUTPUT runner=" << library
              << " case=" << (mode == 0 ? "sync_st" : mode == 1 ? "sync_mt" : "async_mt")
              << (pattern ? "_pattern" : "_message") << "_" << payload_bytes
              << " calls=" << calls << " seconds=" << seconds << " cps=" << calls / seconds
              << " bytes=" << bytes << " processed=" << calls << '\n';
}
} // namespace

int main(int argc, char** argv) try {
    const auto calls = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    if (!calls) throw std::invalid_argument("iterations must be positive");
    bool spdlog_first = false;
    auto short_calls = calls;
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--spdlog-first") spdlog_first = true;
        else if (std::string_view(argv[i]) == "--short-iters" && i + 1 < argc) {
            short_calls = std::strtoull(argv[++i], nullptr, 10);
            if (!short_calls) throw std::invalid_argument("short iterations must be positive");
        } else throw std::invalid_argument("unknown benchmark argument");
    }
    const auto directory = std::filesystem::temp_directory_path() /
        ("chlog-output-bench-" + std::to_string(bench_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("output directory exists");
    std::cout << "META queue_capacity=65536 flush=drained_file_buffers newline=LF rotation=disabled warmup_calls=10000";
    std::cout << " chlog_file_sink_bytes=" << sizeof(chlog::rotating_file_sink);
#if defined(CHLOG_USE_FMT)
    std::cout << " backend=fmt fmt_version=" << FMT_VERSION;
#else
    std::cout << " backend=std_format";
#endif
#if defined(CHLOG_HAS_SPDLOG)
    std::cout << " spdlog_version=" << SPDLOG_VER_MAJOR << '.' << SPDLOG_VER_MINOR << '.' << SPDLOG_VER_PATCH;
    std::cout << " spdlog_file_sink_st_bytes=" << sizeof(spdlog::sinks::rotating_file_sink_st)
              << " spdlog_file_sink_mt_bytes=" << sizeof(spdlog::sinks::rotating_file_sink_mt);
#endif
    std::cout << '\n';
    for (auto size : {128u, 1024u}) for (bool pattern : {false, true}) for (int mode = 0; mode < 3; ++mode) {
#if defined(CHLOG_HAS_SPDLOG)
        for (auto library : spdlog_first ? std::array{"spdlog", "chlog"} : std::array{"chlog", "spdlog"}) {
            run(library, mode, pattern, size, 10000, directory, false);
            run(library, mode, pattern, size, size == 128 ? short_calls : calls, directory);
        }
#else
        (void)spdlog_first;
        run("chlog", mode, pattern, size, 10000, directory, false);
        run("chlog", mode, pattern, size, size == 128 ? short_calls : calls, directory);
#endif
    }
    std::filesystem::remove(directory);
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
