#include <chlog/chlog.hpp>

#include <future>
#include <set>

using namespace std::chrono_literals;

struct custom_value { int value; };
#if defined(CHLOG_USE_FMT)
template <> struct fmt::formatter<custom_value> : fmt::formatter<int> {
#else
template <> struct std::formatter<custom_value> : std::formatter<int> {
#endif
    template <class Context> auto format(const custom_value& value, Context& context) const {
        return formatter<int>::format(value.value, context);
    }
};

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " check failed: " #expr); } while (false)

template <class F> void expect_throw(F f) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

struct recording_sink : chlog::sink {
    std::mutex mutex;
    std::vector<chlog::log_event> events;
    std::vector<std::string> lines;
    std::atomic<std::size_t> flushes{0};
    std::string format(const chlog::log_event& e) const { return render(e); }
    void log(const chlog::log_event& e) override {
        auto line = render(e);
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
        lines.push_back(std::move(line));
    }
    void flush() override { ++flushes; }
};

struct counter_sink : chlog::sink {
    std::atomic<std::size_t> count{0}, flushes{0};
    void log(const chlog::log_event&) override { ++count; }
    void flush() override { ++flushes; }
};

struct blocked_sink : counter_sink {
    std::binary_semaphore entered{0}, proceed{0};
    void log(const chlog::log_event&) override {
        if (count.fetch_add(1) == 0) { entered.release(); proceed.acquire(); }
    }
    void wait_entered() { CHECK(entered.try_acquire_for(5s)); }
};

chlog::logger_config config() {
    chlog::logger_config cfg;
    cfg.pattern = "{msg}";
    cfg.level = chlog::level::trace;
    cfg.parallel_sinks = false;
    cfg.flush_on_level = chlog::level::off;
    return cfg;
}

void formatting() {
    auto cfg = config();
    cfg.single_threaded = true;
    chlog::logger logger(cfg);
    auto output = std::make_shared<recording_sink>();
    logger.add_sink(output);
    logger.info("value {} {:04}", 42, 7);
    std::string runtime = "runtime {} {}";
    logger.info(runtime, std::string("temporary"), 9);
    logger.info(std::string_view("invalid {"), 1);
    logger.log_raw(chlog::level::info, "{file} {line} {func} {msg}");
    CHECK(output->lines.at(0) == "value 42 0007");
    CHECK(output->lines.at(1) == "runtime temporary 9");
    CHECK(output->lines.at(2) == "invalid {");
    CHECK(output->lines.at(3) == "{file} {line} {func} {msg}");
    CHECK(logger.stats().errors == 1);

    logger.set_pattern("[{name}] {msg}|{file}:{line}|{unknown}");
    const auto line = std::source_location::current().line() + 1;
    CHLOG_INFO(logger, "literal {{line}} {{func}}");
    CHECK(output->events.back().loc.line() == line);
    CHECK(output->events.back().name == cfg.name);
    CHECK(output->lines.back().find("literal {line} {func}|") != std::string::npos);
    CHECK(output->lines.back().ends_with("|{unknown}"));
    logger.set_pattern("{msg}");
    logger.info("metadata disabled");
    CHECK(output->events.back().name.empty());
    CHECK(output->events.back().ts == std::chrono::system_clock::time_point{});

    logger.set_level(chlog::level::off);
    logger.log(chlog::level::off, "off");
    logger.log_at_no_loc(chlog::level::critical, "filtered");
    CHECK(logger.stats().enqueued == 6);
    logger.set_level(chlog::level::trace);
    output->set_level(chlog::level::off);
    logger.critical("sink filtered");
    CHECK(output->events.size() == 6);
    CHECK(logger.stats().enqueued == 7);
    logger.shutdown();
    const auto flushed = logger.stats().flushed;
    logger.shutdown();
    logger.info("after stop");
    CHECK(logger.stats().enqueued == 7);
    CHECK(logger.stats().flushed == flushed);

    recording_sink renderer;
    chlog::log_event e;
    e.name = "quoted\"\\\n\t";
    e.payload = std::string("\0\x01\x1f", 3) + "\b\f\n\r\t\\\"";
    renderer.set_pattern("{json}");
    auto json = renderer.format(e);
    CHECK(json.find("\"name\":\"quoted\\\"\\\\\\n\\t\"") != std::string::npos);
    CHECK(json.find("\\u0000\\u0001\\u001F\\b\\f\\n\\r\\t\\\\\\\"") != std::string::npos);
    CHECK(json.find('\n') == std::string::npos);
    e.ts = std::chrono::system_clock::time_point{} - 1ms;
    CHECK(chlog::make_timestamp(e.ts).ends_with(".999"));
    CHECK(chlog::make_timestamp((std::chrono::system_clock::time_point::max)()).size() == 23);
    CHECK(chlog::make_timestamp((std::chrono::system_clock::time_point::min)()).size() == 23);
    renderer.set_pattern("{ms}|{time}|{date}|{ts}|{lvl}|{tid}");
    CHECK(renderer.format(e).starts_with("999|"));
    renderer.set_pattern("");
    CHECK(renderer.format(e).empty());

    chlog::logger custom(config());
    auto custom_output = std::make_shared<recording_sink>(); custom.add_sink(custom_output);
    custom.info("custom {}", custom_value{123});
    custom.info(std::string("runtime custom {}"), custom_value{456});
    const char* runtime_pointer = "pointer {}";
    custom.info(runtime_pointer, 789);
    CHECK(custom_output->lines.at(0) == "custom 123");
    CHECK(custom_output->lines.at(1) == "runtime custom 456");
    CHECK(custom_output->lines.at(2) == "pointer 789");
    custom.info("plain literal");
    custom.info("{{}} {{escaped}}");
    custom.info(std::string("plain runtime"));
    custom.info(std::string("{{runtime}}"));
    custom.info(std::string("lone }"));
    CHECK(custom_output->lines.at(3) == "plain literal");
    CHECK(custom_output->lines.at(4) == "{} {escaped}");
    CHECK(custom_output->lines.at(5) == "plain runtime");
    CHECK(custom_output->lines.at(6) == "{runtime}");
    CHECK(custom_output->lines.at(7) == "lone }");
    CHECK(custom.stats().errors == 1);
}

void queue_tests() {
    for (std::size_t cap : {0u, 1u, 2u, 3u, 4u, 6u, 7u, 10u, 12u, 16u, 24u, 25u, 48u, 63u, 96u, 128u}) {
        chlog::detail::queue_wait wait;
        chlog::detail::mpsc_ring<std::string> ring(cap, &wait);
        CHECK(ring.capacity() == std::max<std::size_t>(2, cap));
        std::vector<std::string> batch;
        batch.reserve(ring.capacity());
        for (int round = 0; round < 200; ++round) {
            for (std::size_t i = 0; i < ring.capacity(); ++i) CHECK(ring.try_push(std::to_string(i)));
            std::string untouched = "full";
            CHECK(!ring.try_push(std::move(untouched)));
            CHECK(untouched == "full");
            CHECK(ring.size_relaxed() == ring.capacity());
            batch.clear();
            CHECK(ring.pop_batch(batch, ring.capacity()) == ring.capacity());
            for (std::size_t i = 0; i < batch.size(); ++i) CHECK(batch[i] == std::to_string(i));
            CHECK(ring.size_relaxed() == 0);
        }
        for (std::size_t i = 0; i < ring.capacity(); ++i) CHECK(ring.try_push(std::string("x")));
        auto blocked = std::async(std::launch::async, [&] { return ring.push_blocking(std::string("cancelled")); });
        CHECK(blocked.wait_for(10ms) == std::future_status::timeout);
        ring.signal_stop();
        ring.signal_stop();
        CHECK(blocked.wait_for(5s) == std::future_status::ready);
        CHECK(!blocked.get());
    }
    expect_throw([] { chlog::dual_queue<std::string> q((std::numeric_limits<std::size_t>::max)()); });
    expect_throw([] { (void)chlog::detail::round_up_pow2((std::numeric_limits<std::size_t>::max)()); });
    chlog::dual_queue<std::string> q(13);
    CHECK(q.capacity() == 13);
    CHECK(q.try_push(std::string("low"), 1));
    for (int i = 0; i < 3; ++i) CHECK(q.try_push(std::string("high"), 3));
    std::vector<std::string> batch;
    q.pop_batch(batch, 1);
    q.pop_batch(batch, 1);
    CHECK(batch.at(0) == "high");
    CHECK(batch.at(1) == "low");
    chlog::dual_queue<std::string> fifo(3, false);
    CHECK(fifo.capacity() == 3);
    CHECK(fifo.try_push(std::string("low"), 1));
    CHECK(fifo.try_push(std::string("high"), 3));
    batch.clear();
    fifo.pop_batch(batch, 3);
    CHECK(batch.at(0) == "low" && batch.at(1) == "high");

    // Repeatedly cross the empty/park/publication boundary without polling the queue.
    chlog::dual_queue<std::string> wake(4);
    std::binary_semaphore consumed{0};
    std::thread consumer([&] {
        std::vector<std::string> values;
        for (int i = 0; i < 3000; ++i) {
            while (!wake.pop_batch(values, 1)) wake.wait_for_data(1s);
            CHECK(values.back() == std::to_string(i));
            values.clear();
            consumed.release();
        }
    });
    for (int i = 0; i < 3000; ++i) {
        CHECK(wake.push_blocking(std::to_string(i), 1));
        CHECK(consumed.try_acquire_for(2s));
    }
    consumer.join();
    wake.signal_stop();
}

void async_tests() {
    for (const auto weighted : {false, true}) {
        auto cfg = config();
        cfg.async.enabled = true;
        cfg.async.queue_capacity = 7;
        cfg.async.batch_max = 0;
        cfg.async.drop_when_full = false;
        cfg.async.weighted_queue = weighted;
        cfg.async.flush_every = 0ms;
        chlog::logger logger(cfg);
        auto output = std::make_shared<recording_sink>();
        logger.add_sink(output);
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) producers.emplace_back([&, t] {
            for (int i = 0; i < 1000; ++i)
                logger.log(i % 3 ? chlog::level::info : chlog::level::warn, "{}:{}", t, i);
        });
        for (auto& producer : producers) producer.join();
        logger.flush();
        CHECK(output->events.size() == 4000);
        std::set<std::string> distinct(output->lines.begin(), output->lines.end());
        CHECK(distinct.size() == 4000);
        CHECK(logger.stats().dequeued == 4000);
        CHECK(logger.stats().enqueued == 4000);
        CHECK(logger.stats().queue_size == 0);
        CHECK(logger.stats().dropped == 0);
        CHECK(output->flushes == 1);
        // Exercise owned heap payloads through repeated small-queue wraparound,
        // including embedded NUL and caller mutation after submission.
        std::string payload(8192, 'x'); payload[100] = '\0';
        for (int i = 0; i < 32; ++i) logger.info("{}:{}", payload, i);
        payload.assign("caller reused its buffer");
        logger.info(std::string("invalid {"));
        logger.flush();
        const std::string expected = std::string(100, 'x') + '\0' + std::string(8091, 'x');
        for (int i = 0; i < 32; ++i)
            CHECK(output->events.at(4000 + i).payload == expected + ':' + std::to_string(i));
        CHECK(output->events.at(4032).payload == "invalid {");
        CHECK(logger.stats().errors == 1);
        CHECK(logger.stats().dequeued == 4033);
        logger.shutdown();
        logger.info("after shutdown");
        CHECK(logger.stats().enqueued == 4033);
    }

    auto cfg = config();
    cfg.async.enabled = true;
    cfg.async.queue_capacity = 4;
    cfg.async.flush_every = 0ms;
    chlog::logger logger(cfg);
    auto slow = std::make_shared<blocked_sink>();
    logger.add_sink(slow);
    logger.info("first");
    slow->wait_entered();
    logger.info("second");
    logger.info("third");
    logger.info("dropped");
    logger.warn("reserved capacity");
    CHECK(logger.stats().dropped == 1);
    auto flush1 = std::async(std::launch::async, [&] { logger.flush(); });
    auto flush2 = std::async(std::launch::async, [&] { logger.flush(); });
    CHECK(flush1.wait_for(20ms) == std::future_status::timeout);
    CHECK(flush2.wait_for(20ms) == std::future_status::timeout);
    slow->proceed.release();
    CHECK(flush1.wait_for(5s) == std::future_status::ready);
    CHECK(flush2.wait_for(5s) == std::future_status::ready);
    flush1.get(); flush2.get();
    CHECK(slow->count == 4);
    CHECK(logger.stats().dequeued == 4);
    logger.shutdown();

    cfg.async.flush_every = 5ms;
    chlog::logger periodic(cfg);
    auto output = std::make_shared<counter_sink>();
    periodic.add_sink(output);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (output->flushes == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(output->flushes > 0);

    cfg.name = std::string(256, 'n'); cfg.pattern = "{name}|{msg}";
    chlog::logger named(cfg);
    auto recorded = std::make_shared<recording_sink>(); named.add_sink(recorded);
    CHLOG_INFO(named, "captured");
    named.flush();
    CHECK(recorded->events.at(0).name == cfg.name);
    CHECK(recorded->lines.at(0) == cfg.name + "|captured");
    named.set_pattern("{msg}");
    named.info("unnamed"); named.flush();
    CHECK(recorded->events.at(1).name.empty());
    named.set_pattern("{name}|{msg}");
    named.info("restored"); named.shutdown();
    CHECK(recorded->events.at(2).name == cfg.name);
}

void parallel_tests() {
    auto cfg = config();
    cfg.parallel_sinks = true;
    cfg.sink_pool_size = 2;
    cfg.sink_queue_capacity = 3;
    chlog::logger logger(cfg);
    auto one = std::make_shared<recording_sink>();
    auto two = std::make_shared<recording_sink>();
    logger.add_sink(one); logger.add_sink(two);
    for (int i = 0; i < 500; ++i) logger.info("{} {}", std::string(1000, 'x'), i);
    logger.flush();
    CHECK(one->events.size() == 500 && two->events.size() == 500);
    CHECK(logger.stats().enqueued == 500 && logger.stats().dequeued == 500);
    CHECK(logger.stats().queue_size == 0);
    logger.set_flush_on(chlog::level::error);
    logger.error("flush on error");
    logger.flush();
    CHECK(one->flushes >= 3 && two->flushes >= 3);
    logger.shutdown();

    chlog::thread_pool pool(1, 1);
    std::binary_semaphore entered{0}, proceed{0};
    CHECK(pool.enqueue([&] { entered.release(); proceed.acquire(); }));
    CHECK(entered.try_acquire_for(5s));
    CHECK(pool.enqueue([] { throw std::runtime_error("task failure"); }));
    auto blocked = std::async(std::launch::async, [&] { return pool.enqueue([] {}); });
    CHECK(blocked.wait_for(20ms) == std::future_status::timeout);
    CHECK(pool.queue_size() == 1);
    proceed.release();
    CHECK(blocked.wait_for(5s) == std::future_status::ready);
    CHECK(blocked.get());
    pool.wait_idle();
    pool.shutdown();
    CHECK(!pool.enqueue([] {}));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

void file_tests() {
    const auto dir = std::filesystem::current_path() /
        ("chlog-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    struct cleanup { std::filesystem::path dir; ~cleanup() { std::error_code ec; std::filesystem::remove_all(dir, ec); } } clean{dir};
    {
        chlog::rotating_file_sink file(dir / "rotate.log", 8, 2);
        file.set_pattern("{msg}");
        chlog::log_event e;
        for (auto text : {"aaa", "bbb", "ccc", "ddd", "eee", "fff", "ggg"}) { e.payload = text; file.log(e); }
        file.flush();
        CHECK(read_file(dir / "rotate.log") == "ggg\n");
        CHECK(read_file(dir / "rotate.log.1") == "eee\nfff\n");
        CHECK(read_file(dir / "rotate.log.2") == "ccc\nddd\n");
        CHECK(!std::filesystem::exists(dir / "rotate.log.3"));
    }
    {
        chlog::rotating_file_sink file(dir / "rotate.log", 8, 1);
        file.set_pattern("{msg}");
        chlog::log_event e; e.payload = "a record larger than the limit";
        file.log(e); e.payload = "z"; file.log(e); file.flush();
        CHECK(read_file(dir / "rotate.log") == "z\n");
        CHECK(read_file(dir / "rotate.log.1") == "a record larger than the limit\n");
    }
    expect_throw([&] { chlog::rotating_file_sink file(dir / "bad.log", 0, 1); });
    expect_throw([&] { chlog::json_sink file(dir); });
    {
        auto cfg = config(); chlog::logger logger(cfg);
        logger.add_sink(std::make_shared<chlog::rotating_file_sink>(dir / "failure.log", 4, 1));
        logger.info("abc");
        std::filesystem::create_directory(dir / "failure.log.1");
        std::ofstream(dir / "failure.log.1" / "occupied") << "keep";
        logger.info("def"); // rotation fails; the original record must survive
        CHECK(logger.stats().errors == 1);
        CHECK(read_file(dir / "failure.log") == "abc\n");
    }
    auto cfg = config(); cfg.name = "name\"\\\n";
    {
        chlog::logger logger(cfg);
        logger.add_sink(std::make_shared<chlog::daily_file_sink>(dir / "daily"));
        logger.add_sink(std::make_shared<chlog::json_sink>(dir / "json.log"));
        const auto line = std::source_location::current().line() + 1;
        CHLOG_INFO(logger, "{}", "hello\nworld");
        logger.flush();
        const auto today = chlog::date_string(std::chrono::system_clock::now());
        CHECK(read_file(dir / "daily" / (today + ".log")) == "hello\nworld\n");
        auto json = read_file(dir / "json.log");
        CHECK(json.find("\"name\":\"name\\\"\\\\\\n\"") != std::string::npos);
        CHECK(json.find("\"line\":" + std::to_string(line)) != std::string::npos);
        CHECK(json.find("hello\\nworld") != std::string::npos);
    }
}

void concurrent_tests() {
    for (int round = 0; round < 12; ++round) {
        auto cfg = config(); cfg.async.enabled = true; cfg.async.queue_capacity = 4;
        cfg.async.drop_when_full = false; cfg.async.batch_max = 1;
        chlog::logger logger(cfg);
        auto output = std::make_shared<counter_sink>(); logger.add_sink(output);
        std::atomic<int> started{0};
        std::vector<std::thread> producers;
        for (int t = 0; t < 6; ++t) producers.emplace_back([&] {
            ++started;
            for (int i = 0; i < 1000; ++i) logger.warn("{}", i);
        });
        while (started != 6) std::this_thread::yield();
        auto first = std::async(std::launch::async, [&] { logger.shutdown(); });
        auto second = std::async(std::launch::async, [&] { logger.shutdown(); });
        for (auto& producer : producers) producer.join();
        CHECK(first.wait_for(5s) == std::future_status::ready);
        CHECK(second.wait_for(5s) == std::future_status::ready);
        first.get(); second.get();
        CHECK(logger.stats().enqueued == output->count);
        CHECK(logger.stats().dequeued == output->count);
        CHECK(logger.stats().queue_size == 0);
    }
    auto cfg = config();
    chlog::logger logger(cfg);
    auto output = std::make_shared<recording_sink>(); logger.add_sink(output);
    std::thread configure([&] {
        for (int i = 0; i < 1000; ++i) {
            logger.set_pattern(i % 2 ? "{msg}" : "[{lvl}] {msg}");
            logger.set_level(i % 3 ? chlog::level::trace : chlog::level::warn);
            logger.set_flush_on(i % 5 ? chlog::level::off : chlog::level::critical);
            output->set_level(i % 7 ? chlog::level::trace : chlog::level::off);
        }
    });
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) producers.emplace_back([&] { for (int j = 0; j < 2000; ++j) logger.warn("value"); });
    for (auto& producer : producers) producer.join();
    configure.join();
    logger.shutdown();
    for (const auto& line : output->lines) CHECK(line == "value" || line == "[WARN] value");
    CHECK(logger.stats().enqueued == 8000 && logger.stats().dequeued == 8000);

    // Shared sinks can receive pattern updates from different logger locks.
    chlog::logger first(cfg), second(cfg);
    auto shared = std::make_shared<recording_sink>();
    first.add_sink(shared); second.add_sink(shared);
    std::thread a([&] { for (int i = 0; i < 1000; ++i) first.set_pattern("A{msg}"); });
    std::thread b([&] { for (int i = 0; i < 1000; ++i) second.set_pattern("B{msg}"); });
    for (int i = 0; i < 1000; ++i) first.info("shared");
    a.join(); b.join();
    first.shutdown(); second.shutdown();
    for (const auto& line : shared->lines) CHECK(line == "shared" || line == "Ashared" || line == "Bshared");
}

void configuration_tests() {
    struct throwing_sink : chlog::sink {
        void log(const chlog::log_event&) override { throw std::runtime_error("write"); }
        void flush() override { throw std::runtime_error("flush"); }
    };
    for (const auto mode : {0, 1, 2, 3}) {
        auto cfg = config(); cfg.single_threaded = mode == 0; cfg.async.enabled = mode == 2;
        cfg.parallel_sinks = mode == 3;
        chlog::logger logger(cfg);
        expect_throw([&] { logger.add_sink(nullptr); });
        logger.add_sink(std::make_shared<throwing_sink>());
        auto output = std::make_shared<counter_sink>(); logger.add_sink(output);
        logger.info("still delivered");
        logger.flush();
        CHECK(output->count == 1);
        CHECK(logger.stats().errors == 2);
        CHECK(logger.stats().enqueued == 1 && logger.stats().dequeued == 1);
    }
    for (std::size_t cap : {0u, 1u, 3u, 5u, 65536u}) {
        auto cfg = config(); cfg.async.enabled = true; cfg.async.queue_capacity = cap;
        cfg.async.batch_max = (std::numeric_limits<std::size_t>::max)();
        chlog::logger logger(cfg);
        CHECK(logger.queue_capacity() == std::max<std::size_t>(4, cap));
        logger.warn("normalized config"); logger.flush();
        CHECK(logger.stats().dequeued == 1);
    }
    auto cfg = config(); cfg.single_threaded = true; cfg.async.enabled = true; cfg.parallel_sinks = true;
    chlog::logger logger(cfg);
    CHECK(logger.queue_capacity() == 0);
    logger.log_raw(chlog::level::warn, "raw {braces}");
    CHECK(logger.stats().enqueued == 1 && logger.stats().dequeued == 1);

    struct metadata_sink : recording_sink {
        unsigned required_metadata() const noexcept override { return all_metadata; }
    };
    cfg.capture_logger_name = false; cfg.capture_source_location = false;
    chlog::logger explicit_flags(cfg);
    auto output = std::make_shared<metadata_sink>(); explicit_flags.add_sink(output);
    CHLOG_INFO(explicit_flags, "honor capture flags");
    CHECK(output->events.at(0).name.empty());
    CHECK(output->events.at(0).loc.line() == 0);
    CHECK(output->events.at(0).ts != std::chrono::system_clock::time_point{});
    for (const auto interval : {(std::chrono::milliseconds::min)(), (std::chrono::milliseconds::max)()}) {
        auto extreme = config(); extreme.async.enabled = true; extreme.async.flush_every = interval;
        chlog::logger timed(extreme);
        timed.info("extreme interval"); timed.flush();
        CHECK(timed.stats().enqueued == 1 && timed.stats().dequeued == 1);
        CHECK(timed.stats().flushed == 1);
    }
}

void registration_tests() {
    std::weak_ptr<counter_sink> lifetime;
    {
        chlog::logger logger(config());
        auto gate = std::make_shared<blocked_sink>(); logger.add_sink(gate);
        auto first = std::async(std::launch::async, [&] { logger.info("before registration"); });
        gate->wait_entered();
        auto added = std::make_shared<counter_sink>(); logger.add_sink(added);
        lifetime = added;
        gate->proceed.release();
        CHECK(first.wait_for(5s) == std::future_status::ready); first.get();
        CHECK(added->count == 0); // the first event retains its captured end of list
        logger.info("after registration"); logger.flush();
        CHECK(added->count == 1 && gate->count == 2);
        added.reset();
        CHECK(!lifetime.expired());
    }
    CHECK(lifetime.expired());

    for (int mode : {0, 1, 2}) {
        auto cfg = config(); cfg.async.enabled = mode == 1; cfg.parallel_sinks = mode == 2;
        cfg.async.drop_when_full = false; cfg.async.queue_capacity = 48;
        cfg.sink_pool_size = 2; cfg.sink_queue_capacity = 32;
        chlog::logger logger(cfg);
        auto initial = std::make_shared<counter_sink>(); logger.add_sink(initial);
        std::vector<std::shared_ptr<counter_sink>> added;
        std::atomic<bool> start{false};
        std::thread registration([&] {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 16; ++i) {
                auto output = std::make_shared<counter_sink>();
                logger.add_sink(output); added.push_back(std::move(output));
            }
        });
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) producers.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 1000; ++i) logger.info("concurrent append {}", i);
        });
        start = true;
        registration.join();
        for (auto& producer : producers) producer.join();
        logger.flush();
        CHECK(initial->count == 4000);
        CHECK(logger.stats().enqueued == 4000 && logger.stats().dequeued == 4000);
        std::vector<std::size_t> counts;
        for (auto& output : added) counts.push_back(output->count.load());
        logger.info("final marker"); logger.shutdown();
        for (std::size_t i = 0; i < added.size(); ++i) CHECK(added[i]->count == counts[i] + 1);
        CHECK(initial->count == 4001);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::string_view suite = argv[1];
        if (suite == "formatting") formatting();
        else if (suite == "queue") queue_tests();
        else if (suite == "async") async_tests();
        else if (suite == "parallel") parallel_tests();
        else if (suite == "files") file_tests();
        else if (suite == "concurrent") concurrent_tests();
        else if (suite == "configuration") configuration_tests();
        else if (suite == "registration") registration_tests();
        else return 2;
        std::cout << "PASS " << suite << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
