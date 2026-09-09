#pragma once

#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable : 4324) // Deliberate cache-line padding for contended atomics.
#endif

// chlog: header-only logging library
// C++20 required (std::format, std::source_location)

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <limits>
#include <new>
#include <queue>
#include <semaphore>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    #include <immintrin.h>
#endif

#if defined(CHLOG_USE_FMT)
    #include <fmt/format.h>
#else
    #include <format>
#endif

namespace chlog {

namespace detail {

// Configuration locks are cold and do not need a platform mutex per object.
// Atomic waiting parks contending threads without retaining a spinning waiter.
class compact_mutex {
public:
    void lock() noexcept {
        while (locked_.exchange(true, std::memory_order_acquire))
            locked_.wait(true, std::memory_order_relaxed);
    }
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
        locked_.notify_one();
    }
private:
    // Some standard libraries retain a four-byte atomic_flag for ABI reasons.
    std::atomic<bool> locked_{false};
};

#if defined(CHLOG_USE_FMT)
template <class... Args> using format_string = fmt::format_string<Args...>;
#else
template <class... Args> using format_string = std::format_string<Args...>;
#endif

template <class... Args>
inline std::string_view format_view(format_string<Args...> f) {
#if defined(CHLOG_USE_FMT)
    const auto view = fmt::string_view(f);
    return {view.data(), view.size()};
#else
    return f.get();
#endif
}


template <class... Args>
inline std::string format_payload(format_string<Args...> fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        const auto text = format_view<>(fmt);
        if (text.find_first_of("{}") == std::string_view::npos) return std::string(text);
    }
#if defined(CHLOG_USE_FMT)
    return fmt::format(fmt, std::forward<Args>(args)...);
#else
    return std::format(fmt, std::forward<Args>(args)...);
#endif
}

template <class... Args>
inline std::string vformat_payload(std::string_view fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        if (fmt.find_first_of("{}") == std::string_view::npos) return std::string(fmt);
    }
#if defined(CHLOG_USE_FMT)
    return fmt::vformat(fmt::string_view(fmt.data(), fmt.size()), fmt::make_format_args(args...));
#else
    return std::vformat(fmt, std::make_format_args(args...));
#endif
}


}  // namespace detail

// =========================== Levels & Config ===========================

enum class level : int { trace, debug, info, warn, error, critical, off };

inline constexpr std::string_view level_name(level lv) noexcept {
    switch (lv) {
        case level::trace: return "TRACE";
        case level::debug: return "DEBUG";
        case level::info: return "INFO";
        case level::warn: return "WARN";
        case level::error: return "ERROR";
        case level::critical: return "CRITICAL";
        case level::off: return "OFF";
    }
    return "UNKNOWN";
}

inline constexpr int level_weight(level lv) noexcept {
    switch (lv) {
        case level::trace: return 1;
        case level::debug: return 1;
        case level::info: return 2;
        case level::warn: return 3;
        case level::error: return 4;
        case level::critical: return 5;
        case level::off: return 0;
    }
    return 0;
}

struct logger_config {
    std::string name = "default";
    chlog::level level = chlog::level::info;

    // Single-threaded mode:
    // - Optimized for the case where ALL logging calls happen from a single thread.
    // - Not thread-safe by design (do not call logger/sinks from multiple threads).
    // - Forces async + parallel_sinks off to avoid background threads and cross-thread sink writes.
    bool single_threaded = false;

    // Pattern tokens: {ts} {date} {time} {ms} {lvl} {tid} {name} {msg} {file} {line} {func}
    // Special pattern: {json} outputs a structured JSON line.
    std::string pattern = "[{date} {time}.{ms}][{lvl}][tid={tid}][{name}] {msg}";

    // Metadata capture controls.
    // These are performance-critical in tight loops. If your pattern is "{msg}" and your sinks
    // don't rely on metadata fields, disabling these avoids per-call work.
    bool capture_timestamp = true;
    bool capture_thread_id = true;
    bool capture_logger_name = true;
    bool capture_source_location = true;

    chlog::level flush_on_level = chlog::level::error;

    struct async_cfg {
        bool enabled = false;
        std::size_t queue_capacity = 1u << 14; // 16384
        std::size_t batch_max = 256;
        std::chrono::milliseconds flush_every{500};

        // When full:
        // - true: drop low priority first (trace/debug/info), keep warn+
        // - false: block producers
        bool drop_when_full = true;

        // true: reserve a separate queue for warn+; false: one FIFO queue.
        bool weighted_queue = true;
    } async;

    bool parallel_sinks = true;
    std::size_t sink_pool_size = 0; // 0 => = sinks.size()
    std::size_t sink_queue_capacity = 1024; // bounded parallel tasks; producers block when full
};

// =========================== Events & Metrics ===========================

struct log_event {
    std::chrono::system_clock::time_point ts;
    level lvl{};
    std::thread::id tid{};
    std::string name;
    std::string payload;
    std::uint64_t seq{};

    std::source_location loc{};
};

// Views are valid only during the sink callback. Copy to log_event to retain a
// record beyond that callback; asynchronous and parallel dispatch still own data.
struct log_event_view {
    std::chrono::system_clock::time_point ts{};
    level lvl{};
    std::thread::id tid{};
    std::string_view name, payload;
    std::uint64_t seq{};
    std::source_location loc{};
    log_event own() const { return {ts, lvl, tid, std::string(name), std::string(payload), seq, loc}; }
};

namespace detail {
// The logger name is immutable and owned by logger. Queue only its capture flag;
// materialize the public owning log_event on the consumer thread.
struct queued_event {
    std::chrono::system_clock::time_point ts{};
    std::thread::id tid{};
    std::string payload;
    std::uint64_t seq{};
    std::source_location loc{};
    level lvl{};
    bool capture_name = false;
    queued_event() = default;
    queued_event(log_event&& e, bool named) noexcept
        : ts(e.ts), tid(e.tid), payload(std::move(e.payload)), seq(e.seq), loc(e.loc), lvl(e.lvl), capture_name(named) {}
};
} // namespace detail

struct metrics_snapshot {
    std::size_t dropped{};
    std::size_t enqueued{};
    std::size_t dequeued{};
    std::size_t flushed{};
    std::size_t queue_size{};
    std::size_t errors{}; // formatting and sink failures
};

struct metrics {
    std::atomic<std::size_t> dropped{0};
    std::atomic<std::size_t> enqueued{0};
    alignas(64) std::atomic<std::size_t> dequeued{0};
    std::atomic<std::size_t> flushed{0};
    std::atomic<std::size_t> queue_size{0};
    std::atomic<std::size_t> errors{0};
};

// =========================== Time / Formatting Utils ===========================

inline std::tm localtime_safe(std::time_t t) noexcept {
    std::tm tm{};
#ifdef _WIN32
    const bool valid = t >= 0 && ::localtime_s(&tm, &t) == 0;
#else
    const bool valid = ::localtime_r(&t, &tm) != nullptr;
#endif
    if (!valid) {
        // Windows CRT rejects pre-epoch timestamps. Fall back to a valid UTC
        // calendar instead of passing an invalid tm to strftime's CRT handler.
        const std::chrono::sys_seconds tp{std::chrono::seconds{t}};
        const auto day = std::chrono::floor<std::chrono::days>(tp);
        const std::chrono::year_month_day date{day};
        const std::chrono::hh_mm_ss time{tp - day};
        tm = {};
        tm.tm_year = static_cast<int>(date.year()) - 1900;
        tm.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
        tm.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
        tm.tm_hour = static_cast<int>(time.hours().count());
        tm.tm_min = static_cast<int>(time.minutes().count());
        tm.tm_sec = static_cast<int>(time.seconds().count());
    }
    return tm;
}

namespace detail {

struct time_parts {
    std::chrono::sys_seconds second{};
    bool valid = false;
    std::array<char, 20> timestamp{};
};

inline const time_parts& cached_time(std::chrono::system_clock::time_point tp) {
    thread_local time_parts cache;
    const auto second = std::chrono::floor<std::chrono::seconds>(tp);
    if (!cache.valid || cache.second != second) {
        const auto tm = localtime_safe(static_cast<std::time_t>(second.time_since_epoch().count()));
        if (tm.tm_year < -1900 || tm.tm_year > 8099 ||
            std::strftime(cache.timestamp.data(), cache.timestamp.size(), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
            constexpr char unavailable[] = "0000-00-00 00:00:00";
            std::copy(std::begin(unavailable), std::end(unavailable), cache.timestamp.begin());
        }
        cache.second = second;
        cache.valid = true;
    }
    return cache;
}

inline void append_milliseconds(std::string& out, std::chrono::system_clock::time_point tp) {
    auto ms = std::chrono::floor<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    if (ms < 0) ms += 1000;
    out.push_back(static_cast<char>('0' + ms / 100));
    out.push_back(static_cast<char>('0' + ms / 10 % 10));
    out.push_back(static_cast<char>('0' + ms % 10));
}

template <class T>
inline void append_number(std::string& out, T value) {
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, result.ptr);
}

inline void append_json_escaped(std::string& out, std::string_view s) {
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else out.push_back(static_cast<char>(c));
        }
    }
}
} // namespace detail

inline std::string make_timestamp(std::chrono::system_clock::time_point tp) {
    std::string out;
    out.reserve(23);
    out.append(detail::cached_time(tp).timestamp.data(), 19);
    out.push_back('.');
    detail::append_milliseconds(out, tp);
    return out;
}

inline std::string date_string(std::chrono::system_clock::time_point tp) {
    return {detail::cached_time(tp).timestamp.data(), 10};
}

inline std::string time_string(std::chrono::system_clock::time_point tp) {
    return {detail::cached_time(tp).timestamp.data() + 11, 8};
}

inline std::string thread_id_string(std::thread::id tid) {
    std::ostringstream oss;
    oss << tid;
    return oss.str();
}

inline const std::string& cached_thread_id(std::thread::id tid) {
    thread_local std::thread::id last;
    thread_local std::string text;
    thread_local bool valid = false;
    if (!valid || last != tid) {
        text = thread_id_string(tid);
        last = tid;
        valid = true;
    }
    return text;
}

inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    detail::append_json_escaped(out, s);
    return out;
}

namespace detail {
inline std::string render_json(const log_event& e) {
    std::string out;
    out.reserve(160 + e.name.size() + e.payload.size() +
                std::char_traits<char>::length(e.loc.file_name()) +
                std::char_traits<char>::length(e.loc.function_name()));
    out += "{\"ts\":\"";
    out.append(cached_time(e.ts).timestamp.data(), 19);
    out.push_back('.');
    append_milliseconds(out, e.ts);
    out += "\",\"lvl\":\"";
    out += level_name(e.lvl);
    out += "\",\"tid\":\"";
    append_json_escaped(out, cached_thread_id(e.tid));
    out += "\",\"name\":\"";
    append_json_escaped(out, e.name);
    out += "\",\"seq\":";
    append_number(out, e.seq);
    out += ",\"file\":\"";
    append_json_escaped(out, e.loc.file_name());
    out += "\",\"line\":";
    append_number(out, e.loc.line());
    out += ",\"func\":\"";
    append_json_escaped(out, e.loc.function_name());
    out += "\",\"msg\":\"";
    append_json_escaped(out, e.payload);
    out += "\"}";
    return out;
}

class compiled_pattern {
    enum class token { literal, ts, date, time, ms, lvl, tid, name, msg, file, line, func };
    struct part { token kind; std::size_t begin; std::size_t size; };
    std::string pattern_;
    std::vector<part> parts_;
public:
    explicit compiled_pattern(std::string pattern) : pattern_(std::move(pattern)) {
        if (pattern_ == "{msg}" || pattern_ == "{json}") return;
        constexpr std::string_view names[] = {"", "ts", "date", "time", "ms", "lvl", "tid", "name", "msg", "file", "line", "func"};
        std::size_t literal = 0;
        for (std::size_t pos = 0; pos < pattern_.size(); ++pos) {
            if (pattern_[pos] != '{') continue;
            const auto end = pattern_.find('}', pos + 1);
            if (end == std::string::npos) break;
            const auto key = std::string_view(pattern_).substr(pos + 1, end - pos - 1);
            for (std::size_t i = 1; i < std::size(names); ++i) {
                if (key != names[i]) continue;
                if (pos > literal) parts_.push_back({token::literal, literal, pos - literal});
                parts_.push_back({static_cast<token>(i), 0, 0});
                pos = end;
                literal = end + 1;
                break;
            }
        }
        if (literal < pattern_.size()) parts_.push_back({token::literal, literal, pattern_.size() - literal});
    }

    std::string render(const log_event& e) const {
        if (pattern_ == "{msg}") return e.payload;
        if (pattern_ == "{json}") return render_json(e);
        std::string out;
        out.reserve(pattern_.size() + e.payload.size() + e.name.size() + 64);
        for (const auto& p : parts_) {
            switch (p.kind) {
                case token::literal: out.append(pattern_, p.begin, p.size); break;
                case token::ts:
                    out.append(cached_time(e.ts).timestamp.data(), 19);
                    out.push_back('.');
                    append_milliseconds(out, e.ts);
                    break;
                case token::date: out.append(cached_time(e.ts).timestamp.data(), 10); break;
                case token::time: out.append(cached_time(e.ts).timestamp.data() + 11, 8); break;
                case token::ms: append_milliseconds(out, e.ts); break;
                case token::lvl: out += level_name(e.lvl); break;
                case token::tid: out += cached_thread_id(e.tid); break;
                case token::name: out += e.name; break;
                case token::msg: out += e.payload; break;
                case token::file: out += e.loc.file_name(); break;
                case token::line: append_number(out, e.loc.line()); break;
                case token::func: out += e.loc.function_name(); break;
            }
        }
        return out;
    }
};
} // namespace detail

// =========================== Sink Interface ===========================

class sink {
    enum class pattern_kind : unsigned char { default_pattern, message, json, compiled };
public:
    enum metadata : unsigned { timestamp = 1, thread_id = 2, logger_name = 4, source_location = 8, all_metadata = 15 };
    sink() = default;
    explicit sink(std::string pattern) : pattern_() { set_pattern(std::move(pattern)); }
    virtual ~sink() = default;
    virtual void set_pattern(std::string pat) {
        std::lock_guard<detail::compact_mutex> lock(pattern_mutex_);
        const auto kind = pat == "{msg}" ? pattern_kind::message
                        : pat == "{json}" ? pattern_kind::json : pattern_kind::compiled;
        auto compiled = kind == pattern_kind::compiled
            ? std::make_shared<const detail::compiled_pattern>(pat) : nullptr;
        // pattern_ remains available to derived sinks. Custom sinks reading it directly
        // must synchronize those reads with set_pattern themselves.
        pattern_.swap(pat); // Release the previous string's unused capacity.
        compiled_ = std::move(compiled);
        pattern_kind_.store(kind, std::memory_order_release);
    }
    virtual void set_level(level lv) { level_.store(lv, std::memory_order_relaxed); }
    // Configure before publishing the sink to logging threads.
    virtual void set_thread_safe(bool enabled) noexcept { thread_safe_ = enabled; }
    virtual level level_threshold() const { return level_.load(std::memory_order_relaxed); }
    // Override when a sink needs metadata even with a message-only pattern.
    virtual unsigned required_metadata() const noexcept { return 0; }
    virtual void log(const log_event& e) = 0;
    // Existing sinks retain their owning-event callback without modification.
    virtual void log_view(const log_event_view& e) { log(e.own()); }
    virtual bool uses_views() const noexcept { return false; }
    virtual void flush() {}

protected:
    std::string pattern_ = "[{date} {time}.{ms}][{lvl}][{name}] {msg}";
    std::atomic<level> level_{level::trace};
    bool thread_safe_ = true;

    bool accepts(const log_event& e) const {
        const auto threshold = level_threshold();
        return e.lvl != level::off && threshold != level::off && e.lvl >= threshold;
    }
    std::string render(const log_event& e) const {
        for (;;) {
            switch (pattern_kind_.load(std::memory_order_acquire)) {
                case pattern_kind::message: return e.payload;
                case pattern_kind::json: return detail::render_json(e);
                case pattern_kind::default_pattern: return default_pattern()->render(e);
                case pattern_kind::compiled: {
                    if (!thread_safe_) return compiled_->render(e);
                    std::shared_ptr<const detail::compiled_pattern> compiled;
                    {
                        std::lock_guard<detail::compact_mutex> lock(pattern_mutex_);
                        compiled = compiled_;
                    }
                    if (compiled) return compiled->render(e);
                    // A simple pattern replaced the snapshot before the lock.
                    break;
                }
            }
        }
    }
private:
    std::shared_ptr<const detail::compiled_pattern> default_pattern() const {
        std::lock_guard<detail::compact_mutex> lock(pattern_mutex_);
        if (!compiled_)
            compiled_ = std::make_shared<const detail::compiled_pattern>("[{date} {time}.{ms}][{lvl}][{name}] {msg}");
        return compiled_;
    }
    mutable detail::compact_mutex pattern_mutex_;
    std::atomic<pattern_kind> pattern_kind_{pattern_kind::default_pattern};
    // Most sinks receive their logger's pattern before first use. Compile the
    // standalone default only when rendering actually needs it.
    mutable std::shared_ptr<const detail::compiled_pattern> compiled_;
};

// Implement consume() for synchronous inspection without an owning string copy.
// The same callback also works with owned async and parallel events.
class view_sink : public sink {
public:
    view_sink() : sink("{msg}") {}
    bool uses_views() const noexcept override { return true; }
    void log(const log_event& e) override { consume({e.ts, e.lvl, e.tid, e.name, e.payload, e.seq, e.loc}); }
    void log_view(const log_event_view& e) override { consume(e); }
    virtual void consume(const log_event_view& e) = 0;
};

class console_sink : public sink {
public:
    enum class style { plain, color };
    explicit console_sink(style s = style::plain) : style_(s) {}

    void log(const log_event& e) override {
        if (!accepts(e)) return;
        const auto line = render(e);
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (style_ == style::color) {
                std::cout << color_of(e.lvl) << line << "\x1b[0m\n";
            } else {
                std::cout << line << '\n';
            }
        } else {
            if (style_ == style::color) {
                std::cout << color_of(e.lvl) << line << "\x1b[0m\n";
            } else {
                std::cout << line << '\n';
            }
        }
    }

    void flush() override {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            std::cout << std::flush;
        } else {
            std::cout << std::flush;
        }
    }

private:
    static const char* color_of(level lv) noexcept {
        switch (lv) {
            case level::trace: return "\x1b[37m";
            case level::debug: return "\x1b[36m";
            case level::info: return "\x1b[32m";
            case level::warn: return "\x1b[33m";
            case level::error: return "\x1b[31m";
            case level::critical: return "\x1b[1;31m";
            case level::off: return "\x1b[0m";
        }
        return "\x1b[0m";
    }

    style style_;
    std::mutex m_;
};

class rotating_file_sink : public sink {
public:
    rotating_file_sink(std::filesystem::path path, std::size_t max_bytes, std::size_t max_files)
        : path_(std::move(path)), max_bytes_(max_bytes), max_files_(max_files ? max_files : 1) {
        if (max_bytes_ == 0) throw std::invalid_argument("chlog: max_bytes must be positive");
        if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
        file_.exceptions(std::ios::badbit | std::ios::failbit);
        open();
    }

    void log(const log_event& e) override {
        if (!accepts(e)) return;
        const auto line = render(e);
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (!file_.is_open()) open();
        // Rotate before a record, keeping records intact. A single oversized record
        // occupies its own file and is rotated on the next write.
        if (bytes_ > 0 && (bytes_ >= max_bytes_ || line.size() >= max_bytes_ - bytes_)) rotate();
        file_ << line << '\n';
        bytes_ += line.size() + 1;
    }

    void flush() override {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (file_.is_open()) file_.flush();
    }

private:
    std::filesystem::path numbered(std::size_t n) const {
        auto result = path_;
        result += "." + std::to_string(n);
        return result;
    }
    void open() {
        file_.clear();
        file_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
        bytes_ = static_cast<std::size_t>(std::filesystem::file_size(path_));
    }
    void rotate() {
        file_.flush();
        file_.close();
        std::filesystem::remove(numbered(max_files_));
        for (std::size_t i = max_files_ - 1; i > 0; --i) {
            const auto src = numbered(i);
            if (std::filesystem::exists(src)) std::filesystem::rename(src, numbered(i + 1));
        }
        std::filesystem::rename(path_, numbered(1));
        open();
    }

    std::filesystem::path path_;
    std::size_t max_bytes_{};
    std::size_t max_files_{};
    std::ofstream file_;
    std::size_t bytes_ = 0;
    std::mutex m_;
};

class daily_file_sink : public sink {
public:
    explicit daily_file_sink(std::filesystem::path dir) : dir_(std::move(dir)) {
        if (!dir_.empty()) std::filesystem::create_directories(dir_);
        file_.exceptions(std::ios::badbit | std::ios::failbit);
        open(date_string(std::chrono::system_clock::now()));
    }
    unsigned required_metadata() const noexcept override { return timestamp; }

    void log(const log_event& e) override {
        if (!accepts(e)) return;
        const auto day = date_string(e.ts);
        const auto line = render(e);
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (day != current_day_ || !file_.is_open()) {
            if (file_.is_open()) file_.close();
            open(day);
        }
        file_ << line << '\n';
    }

    void flush() override {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (file_.is_open()) file_.flush();
    }

private:
    void open(const std::string& day) {
        file_.clear();
        file_.open(dir_ / (day + ".log"), std::ios::out | std::ios::app | std::ios::binary);
        current_day_ = day;
    }
    std::filesystem::path dir_;
    std::string current_day_;
    std::ofstream file_;
    std::mutex m_;
};

class json_sink : public sink {
public:
    explicit json_sink(std::filesystem::path path) {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        file_.exceptions(std::ios::badbit | std::ios::failbit);
        file_.open(path, std::ios::out | std::ios::app | std::ios::binary);
    }
    unsigned required_metadata() const noexcept override { return all_metadata; }

    void log(const log_event& e) override {
        if (!accepts(e)) return;
        const auto line = detail::render_json(e);
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        file_ << line << '\n';
    }

    void flush() override {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        file_.flush();
    }
private:
    std::ofstream file_;
    std::mutex m_;
};

// =========================== Bounded MPSC Queues ===========================
namespace detail {
inline constexpr bool is_pow2(std::size_t x) noexcept { return x != 0 && ((x & (x - 1)) == 0); }
inline constexpr std::size_t round_up_pow2(std::size_t x) {
    if (x <= 1) return 1;
    constexpr auto largest = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (x > largest) throw std::length_error("chlog: queue capacity overflow");
    --x;
    for (std::size_t i = 1; i < sizeof(std::size_t) * 8; i <<= 1) x |= x >> i;
    return x + 1;
}

struct queue_wait {
    std::atomic<bool> sleeping{false};
    std::binary_semaphore sem_not_empty{0};
    std::atomic<bool> stop{false};
    alignas(64) std::atomic<std::size_t> space_epoch{0};

    void notify_data() noexcept {
        // Always exchange: the consumer's acquire exchange synchronizes with
        // publications which happened just before it armed the wait.
        if (sleeping.exchange(false, std::memory_order_acq_rel)) sem_not_empty.release();
    }
    void notify_space() noexcept {
        space_epoch.fetch_add(1, std::memory_order_release);
        space_epoch.notify_all();
    }
    template <class Ready>
    void wait_for_data(std::chrono::milliseconds duration, Ready ready) {
        // Briefly bridge gaps between active producers before parking the
        // consumer. The bounded pause ends quickly when the logger is idle;
        // the semaphore handshake below still handles every sleep transition.
        for (unsigned retry = 0; retry < 32 && duration.count() > 0; ++retry) {
            if (ready() || stop.load(std::memory_order_acquire)) return;
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            _mm_pause();
#else
            std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
        }
        sleeping.exchange(true, std::memory_order_acq_rel);
        if (ready() || stop.load(std::memory_order_acquire)) {
            // A producer may already own a pending release. Consume it before
            // arming another wait, so the binary semaphore never overflows.
            if (!sleeping.exchange(false, std::memory_order_acq_rel)) sem_not_empty.acquire();
            return;
        }
        if (!sem_not_empty.try_acquire_for(duration)) {
            if (!sleeping.exchange(false, std::memory_order_acq_rel)) sem_not_empty.acquire();
        }
    }
    void signal_stop() noexcept {
        stop.store(true, std::memory_order_release);
        notify_data();
        notify_space();
    }
};

template <class T>
class mpsc_ring {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
public:
    mpsc_ring(std::size_t cap, queue_wait* wait)
        : cap_(std::max<std::size_t>(2, cap)), index_shift_(std::countr_zero(cap_)),
          index_mask_((std::size_t{1} << index_shift_) - 1),
          index_factor_(cap_ >> index_shift_), wait_(wait) {
        if (cap_ > static_cast<std::size_t>((std::numeric_limits<std::intptr_t>::max)()) / sizeof(cell))
            throw std::length_error("chlog: queue capacity too large");
        buffer_.reset(new cell[cap_]);
        for (std::size_t i = 0; i < cap_; ++i) buffer_[i].seq.store(i, std::memory_order_relaxed);
    }
    ~mpsc_ring() {
        T item;
        while (try_pop(item)) {}
    }
    mpsc_ring(const mpsc_ring&) = delete;
    mpsc_ring& operator=(const mpsc_ring&) = delete;

    bool try_push(T&& value) {
        if (wait_ && wait_->stop.load(std::memory_order_acquire)) return false;
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        cell* c;
        for (;;) {
            c = &buffer_[index(pos)];
            const auto seq = c->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq - pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) return false;
            else pos = tail_.load(std::memory_order_relaxed);
        }
        std::construct_at(reinterpret_cast<T*>(c->storage), std::move(value));
        c->seq.store(pos + 1, std::memory_order_release);
        if (wait_) wait_->notify_data();
        return true;
    }

    bool push_blocking(T&& value) {
        for (;;) {
            const auto epoch = wait_ ? wait_->space_epoch.load(std::memory_order_acquire) : 0;
            if (try_push(std::move(value))) return true;
            if (!wait_) { std::this_thread::yield(); continue; }
            if (wait_->stop.load(std::memory_order_acquire)) return false;
            wait_->space_epoch.wait(epoch, std::memory_order_acquire);
        }
    }

    std::size_t pop_batch(std::vector<T>& out, std::size_t max_batch) {
        const auto limit = (std::min)(max_batch, cap_);
        if (limit > out.max_size() - out.size()) throw std::length_error("chlog: batch too large");
        // Reserve before releasing any slots, so allocation failure leaves the
        // queue intact. The logger preallocates this storage once.
        if (out.capacity() - out.size() < limit) out.reserve(out.size() + limit);
        auto pos = head_.load(std::memory_order_relaxed);
        auto offset = index(pos);
        std::size_t n = 0;
        while (n < limit) {
            auto& c = buffer_[offset];
            if (c.seq.load(std::memory_order_acquire) != pos + 1) break;
            auto* obj = std::launder(reinterpret_cast<T*>(c.storage));
            out.emplace_back(std::move(*obj));
            std::destroy_at(obj);
            c.seq.store(pos + cap_, std::memory_order_release);
            ++pos;
            ++n;
            if (++offset == cap_) offset = 0;
        }
        if (n) {
            head_.store(pos, std::memory_order_release);
            if (wait_) wait_->notify_space();
        }
        return n;
    }
    void wait_for_data(std::chrono::milliseconds duration) {
        if (wait_) wait_->wait_for_data(duration, [&] { return ready(); });
        else std::this_thread::sleep_for(duration);
    }
    void signal_stop() { if (wait_) wait_->signal_stop(); }
    std::size_t size_relaxed() const noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        return (std::min)(cap_, tail_.load(std::memory_order_relaxed) - head);
    }
    std::size_t capacity() const noexcept { return cap_; }
    std::size_t tail() const noexcept { return tail_.load(std::memory_order_acquire); }
    std::size_t head() const noexcept { return head_.load(std::memory_order_acquire); }
    bool ready() const noexcept {
        const auto pos = head_.load(std::memory_order_relaxed);
        return buffer_[index(pos)].seq.load(std::memory_order_acquire) == pos + 1;
    }
private:
    struct cell {
        std::atomic<std::size_t> seq{0};
        alignas(T) unsigned char storage[sizeof(T)];
    };
    std::size_t index(std::size_t pos) const noexcept {
        if (index_factor_ == 1) return pos & index_mask_;
        // Default low-priority rings have 3 * 2^k slots. A constant remainder
        // by 3 avoids a hardware divide while keeping the exact capacity.
        if (index_factor_ == 3)
            return (((pos >> index_shift_) % 3) << index_shift_) | (pos & index_mask_);
        return pos % cap_;
    }
    bool try_pop(T& out) {
        // One consumer owns head; no CAS is needed here.
        const auto pos = head_.load(std::memory_order_relaxed);
        auto& c = buffer_[index(pos)];
        if (c.seq.load(std::memory_order_acquire) != pos + 1) return false;
        auto* obj = std::launder(reinterpret_cast<T*>(c.storage));
        out = std::move(*obj);
        std::destroy_at(obj);
        c.seq.store(pos + cap_, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_release);
        return true;
    }
    std::size_t cap_;
    int index_shift_;
    std::size_t index_mask_;
    std::size_t index_factor_;
    std::unique_ptr<cell[]> buffer_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    queue_wait* wait_;
};
} // namespace detail

template <class T>
class dual_queue {
public:
    struct positions { std::size_t hi, lo; };
    explicit dual_queue(std::size_t total_cap, bool weighted = true)
        : hi_(weighted ? std::make_unique<detail::mpsc_ring<T>>(high_capacity(total_cap), &wait_) : nullptr),
          lo_(weighted ? std::max<std::size_t>(4, total_cap) - high_capacity(total_cap)
                       : std::max<std::size_t>(2, total_cap), &wait_) {}

    bool try_push(T&& value, int weight) { return ring(weight).try_push(std::move(value)); }
    bool push_blocking(T&& value, int weight) { return ring(weight).push_blocking(std::move(value)); }
    std::size_t pop_batch(std::vector<T>& out, std::size_t max_batch) {
        if (!hi_) return lo_.pop_batch(out, max_batch);
        // Give both tiers service, including batch_max == 1.
        std::size_t n = 0;
        if (max_batch == 1 && low_first_) n += lo_.pop_batch(out, 1);
        low_first_ = !low_first_;
        if (n < max_batch) n += hi_->pop_batch(out, std::max<std::size_t>(1, max_batch - max_batch / 4) - n);
        if (n < max_batch) n += lo_.pop_batch(out, max_batch - n);
        if (n < max_batch) n += hi_->pop_batch(out, max_batch - n);
        return n;
    }
    void wait_for_data(std::chrono::milliseconds duration) {
        wait_.wait_for_data(duration, [&] { return lo_.ready() || (hi_ && hi_->ready()); });
    }
    void signal_stop() { wait_.signal_stop(); }
    std::size_t size_relaxed() const noexcept { return lo_.size_relaxed() + (hi_ ? hi_->size_relaxed() : 0); }
    std::size_t capacity() const noexcept { return lo_.capacity() + (hi_ ? hi_->capacity() : 0); }
    positions tails() const noexcept { return {hi_ ? hi_->tail() : 0, lo_.tail()}; }
    positions heads() const noexcept { return {hi_ ? hi_->head() : 0, lo_.head()}; }
private:
    static std::size_t high_capacity(std::size_t total) { return std::max<std::size_t>(2, total / 4); }
    detail::mpsc_ring<T>& ring(int weight) { return hi_ && weight >= 3 ? *hi_ : lo_; }
    detail::queue_wait wait_;
    std::unique_ptr<detail::mpsc_ring<T>> hi_;
    detail::mpsc_ring<T> lo_;
    bool low_first_ = false;
};

// =========================== Thread Pool ===========================

class thread_pool {
public:
    explicit thread_pool(std::size_t n, std::size_t capacity = 1024)
        : capacity_(std::max<std::size_t>(1, capacity)) {
        n = std::max<std::size_t>(1, n);
        workers_.reserve(n);
        try {
            for (std::size_t i = 0; i < n; ++i) workers_.emplace_back([this] { run(); });
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~thread_pool() { shutdown(); }

    bool enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lk(m_);
            space_.wait(lk, [&] { return stop_ || (!draining_ && tasks_.size() < capacity_); });
            if (stop_) return false;
            tasks_.push(std::move(task));
        }
        work_.notify_one();
        return true;
    }
    void wait_idle() {
        std::unique_lock<std::mutex> barrier(barrier_mu_);
        std::unique_lock<std::mutex> lk(m_);
        draining_ = true;
        idle_.wait(lk, [&] { return tasks_.empty() && active_ == 0; });
        draining_ = false;
        lk.unlock();
        space_.notify_all();
    }
    void shutdown() {
        std::lock_guard<std::mutex> join(join_mu_);
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        work_.notify_all();
        space_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        workers_.clear();
    }
    std::size_t queue_size() const {
        std::lock_guard<std::mutex> lk(m_);
        return tasks_.size();
    }
private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                work_.wait(lk, [&] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                ++active_;
            }
            space_.notify_one();
            try { task(); } catch (...) { /* User tasks must not terminate the worker. */ }
            task = {};
            {
                std::lock_guard<std::mutex> lk(m_);
                --active_;
                if (tasks_.empty() && active_ == 0) idle_.notify_all();
            }
        }
    }
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    const std::size_t capacity_;
    mutable std::mutex m_;
    std::mutex join_mu_;
    std::mutex barrier_mu_;
    std::condition_variable work_, space_, idle_;
    std::size_t active_ = 0;
    bool stop_ = false;
    bool draining_ = false;
};

// =========================== Logger ===========================

class logger {
    template <class Fmt>
    static constexpr bool runtime_format = std::is_convertible_v<Fmt, std::string_view> &&
        !(std::is_array_v<std::remove_reference_t<Fmt>> &&
          std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>);
public:
    explicit logger(logger_config cfg)
        : name_(std::move(cfg.name)), pattern_(std::move(cfg.pattern)),
          level_(cfg.level), flush_level_(cfg.flush_on_level),
          allowed_metadata_(static_cast<unsigned char>((cfg.capture_timestamp ? sink::timestamp : 0u) |
                            (cfg.capture_thread_id ? sink::thread_id : 0u) |
                            (cfg.capture_logger_name ? sink::logger_name : 0u) |
                            (cfg.capture_source_location ? sink::source_location : 0u))),
          single_threaded_(cfg.single_threaded) {
        pattern_.shrink_to_fit();
        capture_mask_.store(static_cast<unsigned char>(configured_metadata()), std::memory_order_relaxed);
        if (!single_threaded_ && cfg.async.enabled) {
            async_ = std::make_unique<async_state>(cfg.async);
            queue_ = async_->queue.get();
            async_->worker = std::thread([this] { worker_loop(); });
        } else if (!single_threaded_ && cfg.parallel_sinks) {
            parallel_ = std::make_unique<parallel_state>(cfg.sink_pool_size, cfg.sink_queue_capacity);
        }
    }
    ~logger() {
        shutdown();
        auto* node = first_sink_.next.load(std::memory_order_relaxed);
        while (node) {
            auto* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    void add_sink(std::shared_ptr<sink> s) {
        if (!s) throw std::invalid_argument("chlog: null sink");
        call_guard active(*this);
        if (!active) return;
        std::lock_guard<detail::compact_mutex> lk(sinks_mu_);
        s->set_pattern(pattern_);
        s->set_thread_safe(!single_threaded_);
        if (!s->uses_views()) views_only_.store(false, std::memory_order_release);
        required_metadata_ |= static_cast<unsigned char>(s->required_metadata());
        update_metadata();
        auto* previous = sinks_tail_.load(std::memory_order_relaxed);
        auto node = previous ? std::make_unique<sink_node>(std::move(s), previous->count + 1) : nullptr;
        auto* added = node ? node.get() : &first_sink_;
        if (parallel_ && !parallel_->pool) {
            parallel_->pool = std::make_unique<thread_pool>(parallel_->pool_size ? parallel_->pool_size : added->count,
                                                          parallel_->capacity);
            parallel_->published.store(parallel_->pool.get(), std::memory_order_release);
        }
        if (previous) {
            previous->next.store(node.release(), std::memory_order_release);
        } else first_sink_.output = std::move(s);
        sinks_tail_.store(added, std::memory_order_release);
    }
    void set_level(level lv) noexcept {
        level_.store(lv, std::memory_order_relaxed);
    }
    bool should_log(level lv) const noexcept {
        if (single_threaded_) return lv >= level_.load(std::memory_order_relaxed) && lv != level::off && !shutdown_done_;
        const auto threshold = level_.load(std::memory_order_relaxed);
        return lv >= threshold && lv != level::off &&
               !(counters().entries.load(std::memory_order_relaxed) & closed_bit);
    }
    void set_pattern(std::string pat) {
        call_guard active(*this);
        if (!active) return;
        std::lock_guard<detail::compact_mutex> lk(sinks_mu_);
        pattern_.swap(pat);
        // Enable newly required fields before publishing sink patterns.
        update_metadata();
        if (auto current = current_sinks()) {
            for (auto& s : current) s->set_pattern(pattern_);
        }
    }
    void set_flush_on(level lv) noexcept {
        flush_level_.store(lv, std::memory_order_relaxed);
    }

    template <class... Args>
    void log(level lv, detail::format_string<Args...> fmt, Args&&... args) {
        if (!should_log(lv)) return;
        submit_format(lv, std::source_location::current(), fmt, std::forward<Args>(args)...);
    }
    template <class Fmt, class... Args>
    void log(level lv, Fmt&& fmt, Args&&... args)
        requires(runtime_format<Fmt>) {
        if (!should_log(lv)) return;
        submit_runtime(lv, std::source_location::current(), std::string_view(fmt), std::forward<Args>(args)...);
    }
    template <class... Args>
    void log_at(level lv, const std::source_location& loc, detail::format_string<Args...> fmt, Args&&... args) {
        if (!should_log(lv)) return;
        submit_format(lv, loc, fmt, std::forward<Args>(args)...);
    }
    template <class Fmt, class... Args>
    void log_at(level lv, const std::source_location& loc, Fmt&& fmt, Args&&... args)
        requires(runtime_format<Fmt>) {
        if (!should_log(lv)) return;
        submit_runtime(lv, loc, std::string_view(fmt), std::forward<Args>(args)...);
    }
    template <class... Args>
    void log_at_no_loc(level lv, detail::format_string<Args...> fmt, Args&&... args) {
        log_at(lv, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template <class Fmt, class... Args>
    void log_at_no_loc(level lv, Fmt&& fmt, Args&&... args)
        requires(runtime_format<Fmt>) {
        log_at(lv, std::source_location{}, std::forward<Fmt>(fmt), std::forward<Args>(args)...);
    }
    // Already formatted text; braces are copied literally.
    void log_raw(level lv, std::string_view message,
                 const std::source_location& loc = std::source_location::current()) {
        if (!should_log(lv)) return;
        if (inline_dispatch()) { submit_view(lv, loc, message); return; }
        submit(lv, loc, [&] { return std::string(message); }, message);
    }

    template <class... Args>
    void trace(detail::format_string<Args...> f, Args&&... a) { log(level::trace, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void trace(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::trace, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void debug(detail::format_string<Args...> f, Args&&... a) { log(level::debug, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void debug(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::debug, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void info(detail::format_string<Args...> f, Args&&... a) { log(level::info, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void info(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::info, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void warn(detail::format_string<Args...> f, Args&&... a) { log(level::warn, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void warn(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::warn, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void error(detail::format_string<Args...> f, Args&&... a) { log(level::error, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void error(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::error, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void critical(detail::format_string<Args...> f, Args&&... a) { log(level::critical, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void critical(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::critical, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }


    void flush() {
        call_guard active(*this);
        if (!active) return;
        if (queue_) {
            // Per-tier publication positions include slots reserved by producers.
            // A count alone is insufficient because high-priority records can overtake lows.
            const auto target = queue_->tails();
            for (;;) {
                const auto epoch = async_->progress.load(std::memory_order_acquire);
                if (reached(async_->completed_hi.load(std::memory_order_acquire), target.hi) &&
                    reached(async_->completed_lo.load(std::memory_order_acquire), target.lo)) break;
                async_->progress.wait(epoch, std::memory_order_acquire);
            }
        }
        if (auto* pool = current_pool()) pool->wait_idle();
        flush_sinks();
    }
    void shutdown() {
        if (single_threaded_) {
            if (!shutdown_done_) { shutdown_done_ = true; flush_sinks(); }
            return;
        }
        std::lock_guard<detail::compact_mutex> join(shutdown_mu_);
        if (shutdown_done_) return;
        calls_.fetch_or(closed_bit, std::memory_order_acq_rel);
        const auto submitted = counters().entries.fetch_or(closed_bit, std::memory_order_seq_cst);
        // Cancel blocked queue submissions before waiting for their guards.
        if (queue_) queue_->signal_stop();
        for (;;) {
            const auto active = calls_.load(std::memory_order_acquire);
            if (active == closed_bit) break;
            calls_.wait(active, std::memory_order_acquire);
        }
        for (;;) {
            const auto completed = counters().returns.load(std::memory_order_seq_cst);
            if (completed == submitted) break;
            counters().returns.wait(completed, std::memory_order_acquire);
        }
        if (async_) {
            async_->stop.store(true, std::memory_order_release);
            queue_->signal_stop();
            if (async_->worker.joinable()) async_->worker.join();
        }
        if (parallel_ && parallel_->pool) parallel_->pool->shutdown();
        flush_sinks();
        shutdown_done_ = true;
    }
    metrics_snapshot stats() const {
        metrics_snapshot result;
        result.dropped = async_ ? async_->dropped.load(std::memory_order_relaxed) : 0;
        if (queue_) {
            const auto accepted = queue_->tails();
            result.enqueued = accepted.hi + accepted.lo;
        } else result.enqueued = static_cast<std::size_t>(counters().entries.load(std::memory_order_acquire) & ~closed_bit);
        result.dequeued = async_ ? async_->dequeued.load(std::memory_order_acquire)
            : parallel_ ? parallel_->dequeued.load(std::memory_order_acquire)
            : static_cast<std::size_t>(counters().returns.load(std::memory_order_acquire));
        result.flushed = static_cast<std::size_t>(flushed_.load(std::memory_order_relaxed));
        result.errors = static_cast<std::size_t>(errors_.load(std::memory_order_relaxed));
        if (queue_) result.queue_size = queue_->size_relaxed();
        else if (auto* pool = current_pool()) result.queue_size = pool->queue_size();
        return result;
    }
    std::size_t queue_capacity() const noexcept { return queue_ ? queue_->capacity() : 0; }

private:
    // The sink API only appends: nodes stay alive until shutdown has joined all
    // users. A captured tail bounds each traversal without refcount operations.
    struct sink_node {
        sink_node(std::shared_ptr<sink> s, std::size_t n) : output(std::move(s)), count(n) {}
        std::shared_ptr<sink> output;
        const std::size_t count;
        std::atomic<sink_node*> next{nullptr};
    };
    struct sink_snapshot {
        const sink_node* first = nullptr;
        const sink_node* last = nullptr;
        struct iterator {
            const sink_node* node;
            const sink_node* last;
            const std::shared_ptr<sink>& operator*() const noexcept { return node->output; }
            iterator& operator++() noexcept {
                node = node == last ? nullptr : node->next.load(std::memory_order_acquire);
                return *this;
            }
            bool operator!=(const iterator& other) const noexcept { return node != other.node; }
        };
        iterator begin() const noexcept { return {first, last}; }
        iterator end() const noexcept { return {nullptr, last}; }
        bool empty() const noexcept { return last == nullptr; }
        explicit operator bool() const noexcept { return !empty(); }
        std::size_t size() const noexcept { return last ? last->count : 0; }
    };
    sink_snapshot current_sinks() const noexcept {
        auto* last = sinks_tail_.load(std::memory_order_acquire);
        // The tail's release publishes head and every link through this tail.
        return last ? sink_snapshot{&first_sink_, last} : sink_snapshot{};
    }
    struct log_counters {
        std::atomic<std::uint64_t> entries{0}, returns{0};
    };
    static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63;
    class log_guard {
    public:
        explicit log_guard(logger& owner) {
            if (owner.single_threaded_) { entered_ = !owner.shutdown_done_; return; }
            counters_ = &owner.counters();
            sequence = counters_->entries.fetch_add(1, std::memory_order_acquire);
            if (sequence & closed_bit) counters_->entries.fetch_sub(1, std::memory_order_relaxed);
            else entered_ = true;
        }
        ~log_guard() {
            if (!entered_ || !counters_) return;
            // SC order prevents a missed shutdown notification: either shutdown
            // observes this return, or this return observes the closed gate.
            counters_->returns.fetch_add(1, std::memory_order_seq_cst);
            if (counters_->entries.load(std::memory_order_seq_cst) & closed_bit)
                counters_->returns.notify_all();
        }
        explicit operator bool() const noexcept { return entered_; }
        log_guard(const log_guard&) = delete;
        log_guard& operator=(const log_guard&) = delete;
        std::uint64_t sequence = 0;
    private:
        log_counters* counters_ = nullptr;
        bool entered_ = false;
    };
    class call_guard {
    public:
        explicit call_guard(logger& owner) : owner_(&owner) {
            if (owner.single_threaded_) { entered_ = !owner.shutdown_done_; return; }
            const auto value = owner.calls_.fetch_add(1, std::memory_order_acquire);
            if (value & closed_bit) {
                owner.calls_.fetch_sub(1, std::memory_order_release);
                owner.calls_.notify_all();
            } else {
                entered_ = true;
            }
        }
        ~call_guard() {
            if (entered_ && !owner_->single_threaded_) {
                const auto old = owner_->calls_.fetch_sub(1, std::memory_order_release);
                if (old & closed_bit) owner_->calls_.notify_all();
            }
        }
        explicit operator bool() const noexcept { return entered_; }
        call_guard(const call_guard&) = delete;
        call_guard& operator=(const call_guard&) = delete;
    private:
        logger* owner_;
        bool entered_ = false;
    };
    unsigned configured_metadata() const noexcept {
        return allowed_metadata() & (pattern_ == "{msg}" ? static_cast<unsigned>(required_metadata_)
                                                        : static_cast<unsigned>(sink::all_metadata));
    }
    unsigned allowed_metadata() const noexcept { return allowed_metadata_; }
    void update_metadata() noexcept {
        capture_mask_.store(static_cast<unsigned char>(configured_metadata()), std::memory_order_release);
    }
    bool should_flush(level lv) const noexcept {
        const auto threshold = flush_level_.load(std::memory_order_relaxed);
        return threshold != level::off && lv >= threshold;
    }
    void error() noexcept {
        if (single_threaded_) increment_local(errors_);
        else errors_.fetch_add(1, std::memory_order_relaxed);
    }
    template <class... Args>
    void submit_format(level lv, const std::source_location& loc, detail::format_string<Args...> fmt, Args&&... args) {
        if (inline_dispatch()) {
            const auto text = detail::format_view<Args...>(fmt);
            if constexpr (sizeof...(Args) == 0) {
                if (text.find_first_of("{}") == std::string_view::npos) {
                    submit_view(lv, loc, text);
                    return;
                }
            }
            submit_formatted_view(lv, loc, text, [&](auto& buffer) {
#if defined(CHLOG_USE_FMT)
                fmt::format_to(std::back_inserter(buffer), fmt, std::forward<Args>(args)...);
#else
                std::format_to(std::back_inserter(buffer), fmt, std::forward<Args>(args)...);
#endif
            });
            return;
        }
        submit(lv, loc, [&] { return detail::format_payload(fmt, std::forward<Args>(args)...); },
               detail::format_view<Args...>(fmt));
    }
    template <class... Args>
    void submit_runtime(level lv, const std::source_location& loc, std::string_view fmt, Args&&... args) {
        if (inline_dispatch()) {
            if constexpr (sizeof...(Args) == 0) {
                if (fmt.find_first_of("{}") == std::string_view::npos) { submit_view(lv, loc, fmt); return; }
            }
            submit_formatted_view(lv, loc, fmt, [&](auto& buffer) {
#if defined(CHLOG_USE_FMT)
                fmt::vformat_to(std::back_inserter(buffer), fmt::string_view(fmt.data(), fmt.size()), fmt::make_format_args(args...));
#else
                std::vformat_to(std::back_inserter(buffer), fmt, std::make_format_args(args...));
#endif
            });
            return;
        }
        submit(lv, loc, [&] { return detail::vformat_payload(fmt, std::forward<Args>(args)...); }, fmt);
    }
    bool inline_dispatch() const noexcept {
        return !queue_ && !parallel_ && views_only_.load(std::memory_order_acquire);
    }
    template <class Format>
    void submit_formatted_view(level lv, const std::source_location& loc, std::string_view fallback, Format format) {
        log_guard active(*this);
        if (!active) return;
#if defined(CHLOG_USE_FMT)
        fmt::basic_memory_buffer<char, 250> buffer;
#else
        std::string buffer;
#endif
        std::string_view message = fallback;
        try { format(buffer); message = {buffer.data(), buffer.size()}; }
        catch (...) { error(); }
        dispatch_view(lv, loc, message, active.sequence);
    }
    void submit_view(level lv, const std::source_location& loc, std::string_view message) {
        log_guard active(*this);
        if (active) dispatch_view(lv, loc, message, active.sequence);
    }
    void dispatch_view(level lv, const std::source_location& loc, std::string_view message, std::uint64_t sequence) {
        log_event_view e;
        const auto capture = capture_mask_.load(std::memory_order_acquire);
        if (capture & sink::timestamp) e.ts = std::chrono::system_clock::now();
        if (capture & sink::thread_id) e.tid = std::this_thread::get_id();
        if (capture & sink::logger_name) e.name = name_;
        if (capture & sink::source_location) e.loc = loc;
        e.lvl = lv;
        e.payload = message;
        e.seq = single_threaded_ ? increment_local(counters().entries) : sequence;
        if (single_threaded_) { write_to(current_sinks(), e); increment_local(counters().returns); }
        else if (auto current = current_sinks()) write_to(current, e);
        if (should_flush(lv)) flush_sinks();
    }
    template <class Format>
    void submit(level lv, const std::source_location& loc, Format format, std::string_view fallback) {
        log_guard active(*this);
        if (!active) return;
        log_event e;
        const auto capture = capture_mask_.load(std::memory_order_acquire);
        if (capture & sink::timestamp) e.ts = std::chrono::system_clock::now();
        if (capture & sink::thread_id) e.tid = std::this_thread::get_id();
        if ((capture & sink::logger_name) && !queue_) e.name = name_;
        if (capture & sink::source_location) e.loc = loc;
        e.lvl = lv;
        try { e.payload = format(); }
        catch (...) { error(); e.payload = fallback; }
        if (single_threaded_) {
            e.seq = increment_local(counters().entries);
            write_to(current_sinks(), e);
            increment_local(counters().returns);
            if (should_flush(lv)) flush_sinks();
            return;
        }
        e.seq = active.sequence;
        if (queue_) {
            const int weight = level_weight(lv);
            detail::queued_event queued(std::move(e), (capture & sink::logger_name) != 0);
            bool accepted = queue_->try_push(std::move(queued), weight);
            if (!accepted && (!async_->config.drop_when_full || lv >= level::warn))
                accepted = queue_->push_blocking(std::move(queued), weight);
            if (!accepted) async_->dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto current = current_sinks();
        auto* pool = current_pool();
        if (pool && current && !current.empty()) {
            // Each event owns its strings once; all per-sink tasks share it.
            const bool flush_event = should_flush(lv);
            auto job = std::make_shared<parallel_event>(std::move(e), current.size(), flush_event);
            for (auto& output : current) {
                try {
                    if (pool->enqueue([this, output, job, flush_event] {
                        write_one(*output, job->event);
                        if (flush_event) flush_one(*output);
                        finish_parallel(*job);
                    })) continue;
                } catch (...) { error(); }
                finish_parallel(*job);
            }
        } else {
            if (current) write_to(current, e);
            if (parallel_) parallel_->dequeued.fetch_add(1, std::memory_order_release);
            if (should_flush(lv)) flush_sinks();
        }
    }
    struct parallel_event {
        parallel_event(log_event&& e, std::size_t n, bool flush) : event(std::move(e)), remaining(n), flush_event(flush) {}
        log_event event;
        std::atomic<std::size_t> remaining;
        bool flush_event;
    };
    void finish_parallel(parallel_event& job) noexcept {
        if (job.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (job.flush_event) flushed_.fetch_add(1, std::memory_order_relaxed);
            parallel_->dequeued.fetch_add(1, std::memory_order_release);
        }
    }
    template <class Event>
    void write_one(sink& output, const Event& e) noexcept {
        try {
            const auto threshold = output.level_threshold();
            if (threshold != level::off && e.lvl >= threshold) {
                if constexpr (std::is_same_v<Event, log_event_view>) output.log_view(e);
                else output.log(e);
            }
        } catch (...) { error(); }
    }
    template <class Outputs, class Event>
    void write_to(const Outputs& outputs, const Event& e) noexcept {
        for (auto& output : outputs) write_one(*output, e);
    }
    void flush_one(sink& output) noexcept {
        try { output.flush(); } catch (...) { error(); }
    }
    void flush_sinks() noexcept {
        if (single_threaded_) {
            for (auto& output : current_sinks()) flush_one(*output);
            increment_local(flushed_);
        } else {
            if (auto current = current_sinks())
                for (auto& output : current) flush_one(*output);
            flushed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    static bool reached(std::size_t done, std::size_t target) noexcept {
        return static_cast<std::intptr_t>(done - target) >= 0;
    }
    std::chrono::steady_clock::time_point flush_deadline(std::chrono::steady_clock::time_point now) const noexcept {
        using clock_type = std::chrono::steady_clock;
        const auto maximum = (clock_type::duration::max)();
        if (async_->config.flush_every >= std::chrono::duration_cast<std::chrono::milliseconds>(maximum))
            return (clock_type::time_point::max)();
        const auto delta = std::chrono::duration_cast<clock_type::duration>(async_->config.flush_every);
        if (now.time_since_epoch() > maximum - delta) return (clock_type::time_point::max)();
        return now + delta;
    }
    void worker_loop() {
        log_event e;
        const bool periodic = async_->config.flush_every.count() > 0;
        auto next_flush = periodic ? flush_deadline(std::chrono::steady_clock::now())
                                   : std::chrono::steady_clock::time_point{};
        for (;;) {
            const auto n = queue_->pop_batch(async_->batch, async_->config.batch_max);
            if (n) {
                if (auto current = current_sinks()) {
                    if (views_only_.load(std::memory_order_acquire)) {
                        for (const auto& queued : async_->batch) {
                            const log_event_view view{queued.ts, queued.lvl, queued.tid,
                                queued.capture_name ? std::string_view(name_) : std::string_view{},
                                queued.payload, queued.seq, queued.loc};
                            write_to(current, view);
                            if (should_flush(view.lvl)) {
                                for (auto& output : current) flush_one(*output);
                                flushed_.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    } else {
                        for (auto& queued : async_->batch) {
                            e.ts = queued.ts;
                            e.tid = queued.tid;
                            e.lvl = queued.lvl;
                            e.loc = queued.loc;
                            e.seq = queued.seq;
                            e.payload = std::move(queued.payload);
                            if (queued.capture_name) {
                                if (e.name.empty()) e.name = name_;
                            } else e.name.clear();
                            write_to(current, e);
                            if (should_flush(e.lvl)) {
                                for (auto& output : current) flush_one(*output);
                                flushed_.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                async_->batch.clear(); // Release payloads before sleeping; reuse only the vector storage.
                std::string{}.swap(e.payload);
                async_->dequeued.fetch_add(n, std::memory_order_release);
                const auto heads = queue_->heads();
                async_->completed_hi.store(heads.hi, std::memory_order_release);
                async_->completed_lo.store(heads.lo, std::memory_order_release);
                async_->progress.fetch_add(1, std::memory_order_release);
                async_->progress.notify_all();
            } else if (async_->stop.load(std::memory_order_acquire)) {
                // Shutdown sets this only after every in-flight producer has left.
                if (queue_->size_relaxed() == 0) break;
            }
            auto wait = std::chrono::milliseconds(1000);
            if (periodic) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_flush) {
                    flush_sinks();
                    next_flush = flush_deadline(now);
                }
                // Clamp in the native clock duration before rounding to avoid
                // overflow for saturated deadlines or very large intervals.
                if (!n) wait = (std::max)(std::chrono::milliseconds(1),
                    std::chrono::ceil<std::chrono::milliseconds>(
                        (std::min)(next_flush - now, std::chrono::steady_clock::duration(std::chrono::seconds(1)))));
            }
            if (!n) queue_->wait_for_data(wait);
        }
    }

    // Async buffers and parallel scheduling exist only in the modes using them.
    struct async_state {
        explicit async_state(logger_config::async_cfg settings) : config(settings),
            queue(std::make_unique<dual_queue<detail::queued_event>>(settings.queue_capacity, settings.weighted_queue)) {
            config.batch_max = std::clamp<std::size_t>(config.batch_max, 1, queue->capacity());
            batch.reserve(config.batch_max);
        }
        logger_config::async_cfg config;
        std::unique_ptr<dual_queue<detail::queued_event>> queue;
        alignas(64) std::vector<detail::queued_event> batch;
        std::thread worker;
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> completed_hi{0}, completed_lo{0}, progress{0}, dequeued{0};
        alignas(64) std::atomic<std::size_t> dropped{0};
        // Producer admission must not invalidate the consumer's dispatch state.
        alignas(64) log_counters counters;
    };
    struct parallel_state {
        parallel_state(std::size_t threads, std::size_t pending) : pool_size(threads), capacity(pending) {}
        const std::size_t pool_size, capacity;
        std::unique_ptr<thread_pool> pool;
        std::atomic<thread_pool*> published{nullptr};
        std::atomic<std::size_t> dequeued{0};
    };
    log_counters& counters() const noexcept {
        return async_ ? async_->counters : inline_counters_;
    }
    thread_pool* current_pool() const noexcept {
        return parallel_ ? parallel_->published.load(std::memory_order_acquire) : nullptr;
    }
    // Single-thread mode uses plain loads/stores, sharing storage with MT counters.
    static std::uint64_t increment_local(std::atomic<std::uint64_t>& value) noexcept {
        const auto old = value.load(std::memory_order_relaxed);
        value.store(old + 1, std::memory_order_relaxed);
        return old;
    }

    std::string name_, pattern_;
    sink_node first_sink_{nullptr, 1};
    std::atomic<sink_node*> sinks_tail_{nullptr};
    dual_queue<detail::queued_event>* queue_ = nullptr;
    std::unique_ptr<async_state> async_;
    std::unique_ptr<parallel_state> parallel_;
    std::atomic<std::uint64_t> calls_{0};
    mutable log_counters inline_counters_;
    std::atomic<std::uint64_t> flushed_{0}, errors_{0};
    std::atomic<level> level_, flush_level_;
    std::atomic<unsigned char> capture_mask_{0};
    const unsigned char allowed_metadata_;
    unsigned char required_metadata_ = 0;
    const bool single_threaded_;
    std::atomic<bool> views_only_{true};
    detail::compact_mutex sinks_mu_, shutdown_mu_;
    bool shutdown_done_ = false;

};

// Convenience macros for capturing source_location without changing call-sites.
#define CHLOG_TRACE(lg, ...) (lg).log_at(::chlog::level::trace, std::source_location::current(), __VA_ARGS__)
#define CHLOG_DEBUG(lg, ...) (lg).log_at(::chlog::level::debug, std::source_location::current(), __VA_ARGS__)
#define CHLOG_INFO(lg, ...)  (lg).log_at(::chlog::level::info, std::source_location::current(), __VA_ARGS__)
#define CHLOG_WARN(lg, ...)  (lg).log_at(::chlog::level::warn, std::source_location::current(), __VA_ARGS__)
#define CHLOG_ERROR(lg, ...) (lg).log_at(::chlog::level::error, std::source_location::current(), __VA_ARGS__)
#define CHLOG_CRIT(lg, ...)  (lg).log_at(::chlog::level::critical, std::source_location::current(), __VA_ARGS__)

} // namespace chlog

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
