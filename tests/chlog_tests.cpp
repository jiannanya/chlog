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
    std::string buffered(const chlog::log_event& e) const {
        chlog::detail::text_buffer output;
        render_to(e, output);
        return std::string(output.data(), output.size());
    }
    void append(const chlog::log_event_view& e, std::string& output) const { render_to(e, output); }
    void log(const chlog::log_event& e) override {
        // Exercise both output APIs during concurrent pattern replacement.
        auto line = e.seq & 1 ? buffered(e) : render(e);
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

template <class Base>
struct legacy_builtin_sink : Base {
    using Base::Base;
    std::size_t owning_calls = 0;
    void log(const chlog::log_event& e) override { ++owning_calls; Base::log(e); }
};

struct recording_view_sink : chlog::view_sink {
    std::mutex mutex;
    std::vector<chlog::log_event> events;
    unsigned required_metadata() const noexcept override { return all_metadata; }
    void consume(const chlog::log_event_view& e) override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e.own());
    }
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
    {
        std::ostringstream captured;
        struct restore_stream {
            std::streambuf* previous;
            ~restore_stream() { std::cout.rdbuf(previous); }
        } restore{std::cout.rdbuf(captured.rdbuf())};
        for (bool threaded : {false, true}) for (bool color : {false, true}) {
            chlog::console_sink console(color ? chlog::console_sink::style::color : chlog::console_sink::style::plain);
            console.set_pattern("{msg}");
            console.set_thread_safe(threaded);
            chlog::log_event event;
            event.lvl = chlog::level::info;
            for (auto size : {0u, 128u, 511u, 512u, 1024u}) {
                captured.str("");
                event.payload.assign(size, 'x');
                if (size) event.payload[size / 2] = '\0';
                console.log(event);
                console.flush();
                CHECK(captured.str() == (color ? "\x1b[32m" : "") + event.payload + (color ? "\x1b[0m\n" : "\n"));
            }
        }
    }
    // Every byte value at each possible alignment, including short tails. The
    // expected control escapes are the JSON specification's canonical forms.
    const std::array<std::string_view, 32> controls = {
        "\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005", "\\u0006", "\\u0007",
        "\\b", "\\t", "\\n", "\\u000B", "\\f", "\\r", "\\u000E", "\\u000F",
        "\\u0010", "\\u0011", "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
        "\\u0018", "\\u0019", "\\u001A", "\\u001B", "\\u001C", "\\u001D", "\\u001E", "\\u001F"};
    for (std::size_t offset = 0; offset < 24; ++offset) for (unsigned byte = 0; byte < 256; ++byte) {
        const std::string prefix(offset, 'a');
        const std::string raw(1, static_cast<char>(byte));
        const std::string escaped = byte < 32 ? std::string(controls[byte])
                                  : byte == '"' ? "\\\"" : byte == '\\' ? "\\\\" : raw;
        for (auto suffix : {"", "abcdefghijk"})
            CHECK(chlog::json_escape(prefix + raw + suffix) == prefix + escaped + suffix);
    }
    recording_sink buffer_test;
    chlog::log_event buffer_event;
    buffer_event.name = "buffer";
    buffer_event.lvl = chlog::level::warn;
    for (auto size : {0u, 1u, 127u, 500u, 511u, 512u, 513u, 1024u, 65536u}) {
        buffer_event.payload.assign(size, 'x');
        if (size) buffer_event.payload[size / 2] = '\0';
        for (auto pattern : {"{msg}", "{json}", "{date} {time}.{ms} [{name}] {msg} {msg} {unknown}"}) {
            buffer_test.set_pattern(pattern);
            const auto expected = buffer_test.format(buffer_event);
            CHECK(buffer_test.buffered(buffer_event) == expected);
            std::string appended = "prefix:";
            buffer_test.append({buffer_event.ts, buffer_event.lvl, buffer_event.tid, buffer_event.name,
                                buffer_event.payload, buffer_event.seq, buffer_event.loc}, appended);
            CHECK(appended == "prefix:" + expected);
        }
    }
    for (int mode = 0; mode < 4; ++mode) {
        auto view_cfg = config();
        view_cfg.single_threaded = mode == 0;
        view_cfg.async.enabled = mode == 2;
        view_cfg.parallel_sinks = mode == 3;
        chlog::logger viewed(view_cfg);
        auto captured = std::make_shared<recording_view_sink>(); viewed.add_sink(captured);
        std::string input(4096, 'x'); input[17] = '\0';
        const auto expected = input;
        CHLOG_INFO(viewed, "{}", input);
        input.assign("changed");
        viewed.info("custom {}", custom_value{19});
        viewed.info("escaped {{braces}}");
        viewed.info(std::string_view("broken {"), 1);
        viewed.log_raw(chlog::level::info, "{literal}");
        viewed.flush();
        CHECK(captured->events.size() == 5);
        CHECK(captured->events.at(0).payload == expected);
        CHECK(captured->events.at(0).name == view_cfg.name);
        CHECK(captured->events.at(0).loc.line() != 0);
        CHECK(captured->events.at(1).payload == "custom 19");
        CHECK(captured->events.at(2).payload == "escaped {braces}");
        CHECK(captured->events.at(3).payload == "broken {");
        CHECK(captured->events.at(4).payload == "{literal}");
        CHECK(viewed.stats().errors == 1 && viewed.stats().enqueued == 5 && viewed.stats().dequeued == 5);
        // Adding a legacy sink changes dispatch without changing either API.
        auto legacy = std::make_shared<recording_sink>(); viewed.add_sink(legacy);
        viewed.info("mixed sinks"); viewed.shutdown();
        CHECK(legacy->events.at(0).payload == "mixed sinks");
        CHECK(captured->events.at(5).payload == "mixed sinks");
    }
    // Standalone sinks must render the default before any set_pattern call.
    recording_sink standalone;
    chlog::log_event standalone_event;
    standalone_event.name = "standalone";
    standalone_event.payload = "default pattern";
    standalone_event.lvl = chlog::level::info;
    CHECK(standalone.format(standalone_event).find("[INFO][standalone] default pattern") != std::string::npos);
    standalone.set_thread_safe(false);
    CHECK(standalone.format(standalone_event).find("[INFO][standalone] default pattern") != std::string::npos);
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
    renderer.set_pattern("[{date} {time}.{ms}]|{date} {time}.{ms}|{unknown}");
    const auto calendar = chlog::make_timestamp(e.ts);
    CHECK(renderer.format(e) == "[" + calendar + "]|" + calendar + "|{unknown}");
    CHECK(renderer.buffered(e) == "[" + calendar + "]|" + calendar + "|{unknown}");
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
    for (bool views : {false, true}) for (bool async : {false, true}) {
        auto string_config = config(); string_config.async.enabled = async; string_config.async.drop_when_full = false;
        chlog::logger strings(string_config);
        auto owned = std::make_shared<recording_sink>();
        auto viewed = std::make_shared<recording_view_sink>();
        strings.add_sink(views ? std::static_pointer_cast<chlog::sink>(viewed)
                               : std::static_pointer_cast<chlog::sink>(owned));
        std::string argument = "preserved";
        strings.info("{}", std::move(argument));
        CHECK(argument == "preserved");
        strings.info(std::string("{}"), std::string_view(argument));
        strings.info("{:>9.3}", std::string("hello"));
        strings.info("{{{}}}", std::string("world"));
        strings.info("{}", std::string{});
        strings.shutdown();
        const auto& events = views ? viewed->events : owned->events;
        CHECK(events.size() == 5);
        CHECK(events[0].payload == "preserved" && events[1].payload == "preserved");
        CHECK(events[2].payload == "      hel" && events[3].payload == "{world}");
        CHECK(events[4].payload.empty() && strings.stats().errors == 0);
    }
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

    // Auto mode: one destination is written by the caller; the pool engages
    // when a second sink makes fan-out real.
    {
        auto auto_cfg = config();
        auto_cfg.parallel_sinks = true;
        auto_cfg.sink_pool_size = 0;
        chlog::logger auto_logger(auto_cfg);
        auto first = std::make_shared<counter_sink>();
        auto_logger.add_sink(first);
        for (int i = 0; i < 100; ++i) auto_logger.info("inline {}", i);
        CHECK(auto_logger.stats().queue_size == 0);
        auto second = std::make_shared<counter_sink>();
        auto_logger.add_sink(second);
        for (int i = 0; i < 100; ++i) auto_logger.info("pooled {}", i);
        auto_logger.flush();
        CHECK(first->count == 200 && second->count == 100);
        CHECK(auto_logger.stats().enqueued == 200 && auto_logger.stats().dequeued == 200);
        auto_logger.shutdown();
    }
    // An explicit pool size keeps even a single destination on the pool: the
    // caller returns while the sink is still inside its callback.
    {
        auto forced_cfg = config();
        forced_cfg.parallel_sinks = true;
        forced_cfg.sink_pool_size = 1;
        chlog::logger forced_logger(forced_cfg);
        auto gated = std::make_shared<blocked_sink>();
        forced_logger.add_sink(gated);
        auto producer = std::async(std::launch::async, [&] { forced_logger.info("first"); });
        gated->wait_entered();
        CHECK(producer.wait_for(200ms) == std::future_status::ready);
        gated->proceed.release();
        forced_logger.flush();
        CHECK(gated->count.load() == 1);
        forced_logger.shutdown();
    }
    // A flush must wait for a record that is still being written even when
    // newer records complete first. A barrier built only from a count of
    // completed records would pass as soon as enough later records finish.
    {
        auto barrier_cfg = config();
        barrier_cfg.parallel_sinks = true;
        barrier_cfg.sink_pool_size = 4;
        barrier_cfg.sink_queue_capacity = 32;
        chlog::logger blocked_logger(barrier_cfg);
        auto fast = std::make_shared<counter_sink>();
        auto gated = std::make_shared<blocked_sink>();
        blocked_logger.add_sink(gated);
        blocked_logger.add_sink(fast);
        for (int i = 0; i < 4; ++i) blocked_logger.info("early {}", i);
        gated->wait_entered();
        auto flushed = std::async(std::launch::async, [&] { blocked_logger.flush(); });
        // Give the flush thread time to enter the barrier, then let newer
        // records finish while the gated record is still in flight.
        std::this_thread::sleep_for(50ms);
        for (int i = 0; i < 16; ++i) blocked_logger.info("late {}", i);
        std::this_thread::sleep_for(50ms);
        CHECK(flushed.wait_for(0ms) == std::future_status::timeout);
        gated->proceed.release();
        CHECK(flushed.wait_for(5s) == std::future_status::ready);
        CHECK(gated->count.load() == 20 && fast->count.load() == 20);
        blocked_logger.shutdown();
    }

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
        std::string expected;
        const auto path = dir / "buffer-boundaries.log";
        {
            chlog::detail::file_writer output;
            output.open(path);
            for (auto count : {0u, 1u, 4094u, 1u, 1u, 4096u, 4097u, 0u, 131072u, 3u}) {
                std::string bytes(count, '\0');
                for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
                output.write(bytes.data(), bytes.size());
                expected += bytes;
            }
            // Destruction must drain the last partial block.
        }
        CHECK(read_file(path) == expected);
    }
    {
        auto verify_legacy = [&](auto output) {
            CHECK(!output->uses_views());
            chlog::log_event_view event;
            event.ts = std::chrono::system_clock::now();
            event.payload = "direct borrowed call";
            output->log_view(event);
            chlog::logger logger(config());
            logger.add_sink(output);
            logger.info("legacy callback");
            logger.shutdown();
            CHECK(output->owning_calls == 2);
        };
        verify_legacy(std::make_shared<legacy_builtin_sink<chlog::rotating_file_sink>>(dir / "derived.log", 4096, 1));
        verify_legacy(std::make_shared<legacy_builtin_sink<chlog::daily_file_sink>>(dir / "derived-daily"));
        verify_legacy(std::make_shared<legacy_builtin_sink<chlog::json_sink>>(dir / "derived-json.log"));
        std::ostringstream captured;
        struct restore_stream {
            std::streambuf* previous;
            ~restore_stream() { std::cout.rdbuf(previous); }
        } restore{std::cout.rdbuf(captured.rdbuf())};
        verify_legacy(std::make_shared<legacy_builtin_sink<chlog::console_sink>>());
        CHECK(captured.str().find("legacy callback") != std::string::npos);
    }
    for (int mode = 0; mode < 4; ++mode) {
        const auto path = dir / ("borrowed-" + std::to_string(mode) + ".log");
        auto cfg = config(); cfg.single_threaded = mode == 0;
        cfg.async.enabled = mode == 2; cfg.parallel_sinks = mode == 3;
        chlog::logger logger(cfg);
        auto output = std::make_shared<chlog::rotating_file_sink>(path, 1u << 20, 1);
#if defined(__cpp_rtti) || defined(_CPPRTTI)
        CHECK(output->uses_views());
#else
        CHECK(!output->uses_views());
#endif
        logger.add_sink(output);
        std::string message(8192, 'x'); message[91] = '\0';
        const auto expected = message + '\n';
        logger.info("{}", message);
        message.assign("changed input");
        logger.shutdown();
        CHECK(logger.stats().dequeued == 1 && logger.stats().errors == 0);
        CHECK(read_file(path) == expected);
    }
    {
        chlog::daily_file_sink daily(dir / "day-switch");
        daily.set_pattern("{msg}");
        chlog::log_event e;
        const auto first = std::chrono::system_clock::now();
        e.ts = first; e.payload = "first"; daily.log(e);
        e.ts = first + std::chrono::hours(48); e.payload = "second"; daily.log(e);
        e.ts = first; e.payload = "third"; daily.log(e);
        daily.flush();
        CHECK(read_file(dir / "day-switch" / (chlog::date_string(first) + ".log")) == "first\nthird\n");
        CHECK(read_file(dir / "day-switch" / (chlog::date_string(first + std::chrono::hours(48)) + ".log")) == "second\n");
    }
    {
        const auto path = dir / std::filesystem::path(u8"\u4e2d\u6587\u65e5\u5fd7.log");
        std::string expected = "existing\n";
        { std::ofstream initial(path, std::ios::binary); initial << expected; }
        chlog::rotating_file_sink output(path, 1u << 20, 1);
        output.set_pattern("{msg}");
        chlog::log_event event;
        for (auto size : {511u, 512u, 1024u}) {
            event.payload.assign(size, 'x');
            event.payload[size / 2] = '\0';
            output.log(event);
            expected += event.payload + '\n';
        }
        output.flush();
        CHECK(read_file(path) == expected); // Reading while the writer remains open.
    }
#ifndef _WIN32
    if (std::filesystem::exists("/dev/full")) {
        chlog::logger logger(config());
        logger.add_sink(std::make_shared<chlog::json_sink>("/dev/full"));
        logger.info("buffered write");
        logger.flush();
        CHECK(logger.stats().errors >= 1);
        logger.info("failed file stays failed");
        logger.shutdown();
        CHECK(logger.stats().errors >= 3);
    }
#endif
    {
        auto cfg = config();
        chlog::logger logger(cfg);
        logger.add_sink(std::make_shared<chlog::rotating_file_sink>(dir / "concurrent.log", 16u << 20, 1));
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t) writers.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i)
                logger.info("{}:{}:{}", t, i, std::string(i % 2 ? 511 : 4096, static_cast<char>('a' + t)));
        });
        for (auto& writer : writers) writer.join();
        logger.shutdown();
        CHECK(logger.stats().errors == 0 && logger.stats().dequeued == 400);
        std::istringstream contents(read_file(dir / "concurrent.log"));
        std::set<std::string> lines;
        for (std::string line; std::getline(contents, line);) CHECK(lines.insert(line).second);
        CHECK(lines.size() == 400);
        for (int t = 0; t < 4; ++t) for (int i = 0; i < 100; ++i)
            CHECK(lines.contains(std::to_string(t) + ":" + std::to_string(i) + ":" +
                                 std::string(i % 2 ? 511 : 4096, static_cast<char>('a' + t))));
    }
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
    {
        chlog::detail::compact_mutex lock;
        std::uint64_t total = 0, checksum = 0;
        std::vector<std::thread> writers;
        for (int t = 0; t < 8; ++t) writers.emplace_back([&] {
            for (int i = 0; i < 4000; ++i) {
                std::lock_guard<chlog::detail::compact_mutex> guard(lock);
                checksum += ++total;
                if (i % 31 == 0) std::this_thread::yield();
            }
        });
        for (auto& writer : writers) writer.join();
        CHECK(total == 32000 && checksum == total * (total + 1) / 2);
    }
    for (int mode = 0; mode < 3; ++mode) {
        for (int round = 0; round < 12; ++round) {
            auto cfg = config(); cfg.async.enabled = mode == 1; cfg.parallel_sinks = mode == 2;
            cfg.async.queue_capacity = 4;
            cfg.async.drop_when_full = false; cfg.async.batch_max = 1;
            chlog::logger logger(cfg);
            auto output = std::make_shared<counter_sink>(); logger.add_sink(output);
            // The parallel pool engages for fan-out, so give it a second sink.
            if (mode == 2) logger.add_sink(std::make_shared<counter_sink>());
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
    }
    for (const bool async : {false, true}) {
        auto cfg = config(); cfg.async.enabled = async; cfg.async.drop_when_full = false;
        chlog::logger logger(cfg);
        auto output = std::make_shared<recording_sink>(); logger.add_sink(output);
        std::vector<std::thread> producers;
        for (int t = 0; t < 4; ++t) producers.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) logger.info("sequence {}", i);
        });
        for (auto& producer : producers) producer.join();
        logger.shutdown();
        std::set<std::uint64_t> sequences;
        for (const auto& event : output->events) sequences.insert(event.seq);
        CHECK(sequences.size() == 4000);
        CHECK(*sequences.begin() == 0 && *sequences.rbegin() == 3999);
        CHECK(logger.stats().enqueued == 4000 && logger.stats().dequeued == 4000);
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
    {
        auto cfg = config(); cfg.single_threaded = true;
        chlog::logger logger(cfg);
        std::vector<std::shared_ptr<recording_sink>> outputs;
        for (int i = 0; i < 8; ++i) {
            outputs.push_back(std::make_shared<recording_sink>());
            logger.add_sink(outputs.back());
            logger.info("registration {}", i);
        }
        logger.set_pattern("[{lvl}] {msg}");
        logger.info("complete"); logger.shutdown();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            CHECK(outputs[i]->events.size() == 9 - i);
            CHECK(outputs[i]->lines.back() == "[INFO] complete");
        }
        CHECK(logger.stats().enqueued == 9 && logger.stats().dequeued == 9);
    }
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
