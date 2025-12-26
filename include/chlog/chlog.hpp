#pragma once

// chlog: header-only logging library
// C++20 required (std::format, std::source_location)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <semaphore>
#include <shared_mutex>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(CHLOG_USE_FMT)
    #include <fmt/format.h>
#endif

namespace chlog {

namespace detail {

template <class... Args>
inline std::string format_payload(std::format_string<Args...> fmt, Args&&... args) {
#if defined(CHLOG_USE_FMT)
    // Use fmt for speed; std::format_string still provides compile-time checking.
    return fmt::format(fmt::runtime(fmt.get()), std::forward<Args>(args)...);
#else
    return std::format(fmt, std::forward<Args>(args)...);
#endif
}

template <class... Args>
inline std::string vformat_payload(std::string_view fmt, Args&&... args) {
#if defined(CHLOG_USE_FMT)
    return fmt::vformat(fmt::string_view(fmt.data(), fmt.size()), fmt::make_format_args(std::forward<Args>(args)...));
#else
    return std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...));
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

        // Kept for compatibility with the original project; now maps to a faster
        // two-tier priority queue implementation.
        bool weighted_queue = true;
    } async;

    bool parallel_sinks = true;
    std::size_t sink_pool_size = 0; // 0 => = sinks.size()
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

struct metrics_snapshot {
    std::size_t dropped{};
    std::size_t enqueued{};
    std::size_t dequeued{};
    std::size_t flushed{};
    std::size_t queue_size{};
};

struct metrics {
    std::atomic<std::size_t> dropped{0};
    std::atomic<std::size_t> enqueued{0};
    std::atomic<std::size_t> dequeued{0};
    std::atomic<std::size_t> flushed{0};
    std::atomic<std::size_t> queue_size{0};
};

// =========================== Time / Formatting Utils ===========================

inline std::tm localtime_safe(std::time_t t) noexcept {
    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif
    return tm;
}

inline std::string make_timestamp(std::chrono::system_clock::time_point tp) {
    const auto tt = std::chrono::system_clock::to_time_t(tp);
    const auto tm = localtime_safe(tt);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    std::ostringstream ts;
    ts << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms;
    return ts.str();
}

inline std::string date_string(std::chrono::system_clock::time_point tp) {
    const auto tt = std::chrono::system_clock::to_time_t(tp);
    const auto tm = localtime_safe(tt);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d");
    return oss.str();
}

inline std::string time_string(std::chrono::system_clock::time_point tp) {
    const auto tt = std::chrono::system_clock::to_time_t(tp);
    const auto tm = localtime_safe(tt);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

inline std::string thread_id_string(std::thread::id tid) {
    std::ostringstream oss;
    oss << tid;
    return oss.str();
}

inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04X}", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// =========================== Sink Interface ===========================

class sink {
public:
    virtual ~sink() = default;
    virtual void set_pattern(std::string pat) { pattern_ = std::move(pat); }
    virtual void set_level(level lv) { level_ = lv; }
    virtual void set_thread_safe(bool enabled) noexcept { thread_safe_ = enabled; }
    virtual level level_threshold() const { return level_; }
    virtual void log(const log_event& e) = 0;
    virtual void flush() {}

protected:
    std::string pattern_ = "[{date} {time}.{ms}][{lvl}][{name}] {msg}";
    level level_ = level::trace;
    bool thread_safe_ = true;

    std::string render(const log_event& e) const {
        if (pattern_ == "{json}") {
            return std::format(
                R"({{"ts":"{}","lvl":"{}","tid":"{}","name":"{}","seq":{},"file":"{}","line":{},"func":"{}","msg":"{}"}})",
                make_timestamp(e.ts),
                level_name(e.lvl),
                thread_id_string(e.tid),
                e.name,
                e.seq,
                json_escape(e.loc.file_name()),
                e.loc.line(),
                json_escape(e.loc.function_name()),
                json_escape(e.payload));
        }

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.ts.time_since_epoch()).count() % 1000;

        std::string out = pattern_;
        auto replace_all = [&](std::string& s, std::string_view key, std::string_view val) {
            std::size_t pos = 0;
            while ((pos = s.find(key, pos)) != std::string::npos) {
                s.replace(pos, key.size(), val);
                pos += val.size();
            }
        };

        replace_all(out, "{ts}", make_timestamp(e.ts));
        replace_all(out, "{date}", date_string(e.ts));
        replace_all(out, "{time}", time_string(e.ts));
        replace_all(out, "{ms}", std::format("{:03}", ms));
        replace_all(out, "{lvl}", std::string(level_name(e.lvl)));
        replace_all(out, "{name}", e.name);
        replace_all(out, "{tid}", thread_id_string(e.tid));
        replace_all(out, "{msg}", e.payload);
        replace_all(out, "{file}", std::string(e.loc.file_name()));
        replace_all(out, "{line}", std::to_string(e.loc.line()));
        replace_all(out, "{func}", std::string(e.loc.function_name()));

        return out;
    }
};

class console_sink : public sink {
public:
    enum class style { plain, color };
    explicit console_sink(style s = style::plain) : style_(s) {}

    void log(const log_event& e) override {
        if (static_cast<int>(e.lvl) < static_cast<int>(level_)) return;
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
        if (!path_.parent_path().empty()) {
            std::filesystem::create_directories(path_.parent_path());
        }
        open();
    }

    void log(const log_event& e) override {
        if (static_cast<int>(e.lvl) < static_cast<int>(level_)) return;
        const auto line = render(e);
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (!file_.is_open()) return;
            file_ << line << '\n';
            bytes_ += line.size() + 1;
            if (bytes_ >= max_bytes_) rotate();
        } else {
            if (!file_.is_open()) return;
            file_ << line << '\n';
            bytes_ += line.size() + 1;
            if (bytes_ >= max_bytes_) rotate();
        }
    }

    void flush() override {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (file_.is_open()) file_.flush();
        } else {
            if (file_.is_open()) file_.flush();
        }
    }

private:
    void open() {
        file_.open(path_, std::ios::out | std::ios::app);
        std::error_code ec;
        bytes_ = std::filesystem::exists(path_, ec) ? static_cast<std::size_t>(std::filesystem::file_size(path_, ec)) : 0;
        if (ec) bytes_ = 0;
    }

    void rotate() {
        if (!file_.is_open()) return;
        file_.flush();
        file_.close();

        std::error_code ec;
        // Remove the last one to make space.
        const auto last = path_.string() + "." + std::to_string(max_files_);
        if (std::filesystem::exists(last, ec)) {
            std::filesystem::remove(last, ec);
        }

        for (std::size_t i = max_files_ - 1; i >= 1; --i) {
            const auto src = path_.string() + "." + std::to_string(i);
            const auto dst = path_.string() + "." + std::to_string(i + 1);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::rename(src, dst, ec);
            }
            if (i == 1) break;
        }

        const auto first = path_.string() + ".1";
        if (std::filesystem::exists(path_, ec)) {
            std::filesystem::rename(path_, first, ec);
        }

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
        std::filesystem::create_directories(dir_);
        open(date_string(std::chrono::system_clock::now()));
    }

    void log(const log_event& e) override {
        if (static_cast<int>(e.lvl) < static_cast<int>(level_)) return;
        const auto day = date_string(e.ts);
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (day != current_day_) rotate(day);
            if (!file_.is_open()) return;
            file_ << render(e) << '\n';
        } else {
            if (day != current_day_) rotate(day);
            if (!file_.is_open()) return;
            file_ << render(e) << '\n';
        }
    }

    void flush() override {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (file_.is_open()) file_.flush();
        } else {
            if (file_.is_open()) file_.flush();
        }
    }

private:
    void open(const std::string& day) {
        current_day_ = day;
        const auto path = dir_ / (day + ".log");
        file_.open(path, std::ios::out | std::ios::app);
    }

    void rotate(const std::string& day) {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        open(day);
    }

    std::filesystem::path dir_;
    std::string current_day_;
    std::ofstream file_;
    std::mutex m_;
};

class json_sink : public sink {
public:
    explicit json_sink(std::filesystem::path path) : path_(std::move(path)) {
        if (!path_.parent_path().empty()) {
            std::filesystem::create_directories(path_.parent_path());
        }
        file_.open(path_, std::ios::out | std::ios::app);
    }

    void log(const log_event& e) override {
        if (static_cast<int>(e.lvl) < static_cast<int>(level_)) return;
        const auto js = std::format(
            R"({{"ts":"{}","lvl":"{}","tid":"{}","name":"{}","seq":{},"file":"{}","line":{},"func":"{}","msg":"{}"}})",
            make_timestamp(e.ts),
            level_name(e.lvl),
            thread_id_string(e.tid),
            e.name,
            e.seq,
            json_escape(e.loc.file_name()),
            e.loc.line(),
            json_escape(e.loc.function_name()),
            json_escape(e.payload));

        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (!file_.is_open()) return;
            file_ << js << '\n';
        } else {
            if (!file_.is_open()) return;
            file_ << js << '\n';
        }
    }

    void flush() override {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lk(m_);
            if (file_.is_open()) file_.flush();
        } else {
            if (file_.is_open()) file_.flush();
        }
    }

private:
    std::filesystem::path path_;
    std::ofstream file_;
    std::mutex m_;
};

// =========================== Lock-free Dual Queue (MPSC, bounded) ===========================
// Goal: "industrial-grade" async performance.
//
// - Multi-producer / single-consumer.
// - Bounded, lock-free fast path (CAS-based ring, no mutex on try_push/pop).
// - Two independent rings:
//   - high priority for warn+
//   - low priority for trace/debug/info
//   This reserves capacity so low-priority bursts can't starve warn+.

namespace detail {

inline constexpr bool is_pow2(std::size_t x) noexcept { return x != 0 && ((x & (x - 1)) == 0); }

inline constexpr std::size_t round_up_pow2(std::size_t x) noexcept {
    if (x <= 1) return 1;
    --x;
    for (std::size_t i = 1; i < sizeof(std::size_t) * 8; i <<= 1) x |= (x >> i);
    return x + 1;
}

struct queue_wait {
    std::mutex m;
    std::condition_variable cv_not_full;
    // Hint to reduce producer-side cacheline traffic: producers notify only if the
    // consumer is likely waiting.
    std::atomic<bool> sleeping{false};
    // Semaphore used purely as a wakeup mechanism for the single consumer.
    // counting_semaphore<1> behaves like a binary semaphore and avoids permit buildup.
    std::counting_semaphore<1> sem_not_empty{0};
    std::atomic<bool> stop{false};
};

template <class T>
class mpsc_ring {
public:
    mpsc_ring(std::size_t cap, queue_wait* wait) : cap_(detail::round_up_pow2(cap)), mask_(cap_ - 1), wait_(wait) {
        if (!detail::is_pow2(cap_)) {
            cap_ = detail::round_up_pow2(cap_);
            mask_ = cap_ - 1;
        }
        buffer_.reset(new cell[cap_]);
        for (std::size_t i = 0; i < cap_; ++i) buffer_[i].seq.store(i, std::memory_order_relaxed);
    }

    ~mpsc_ring() {
        // Best-effort drain to destroy remaining elements.
        T tmp;
        while (try_pop(tmp)) {
        }
    }

    mpsc_ring(const mpsc_ring&) = delete;
    mpsc_ring& operator=(const mpsc_ring&) = delete;

    bool try_push(T&& v) {
        if (wait_ && wait_->stop.load(std::memory_order_relaxed)) return false;

        cell* c = nullptr;
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            c = &buffer_[pos & mask_];
            const std::size_t seq = c->seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        new (&c->storage) T(std::move(v));
        c->seq.store(pos + 1, std::memory_order_release);

        // Wake consumer only if it is likely sleeping.
        if (wait_ && wait_->sleeping.exchange(false, std::memory_order_relaxed)) {
            wait_->sem_not_empty.release();
        }
        return true;
    }

    void push_blocking(T&& v) {
        // Blocking is not the hot path; used primarily for warn+ overload cases.
        for (;;) {
            if (wait_ && wait_->stop.load(std::memory_order_relaxed)) return;
            if (try_push(std::move(v))) return;

            if (!wait_) {
                std::this_thread::yield();
                continue;
            }

            std::unique_lock<std::mutex> lk(wait_->m);
            wait_->cv_not_full.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return wait_->stop.load(std::memory_order_relaxed);
            });
        }
    }

    std::size_t pop_batch(std::vector<T>& out, std::size_t max_batch) {
        std::size_t n = 0;
        T item;
        while (n < max_batch && try_pop(item)) {
            out.push_back(std::move(item));
            ++n;
        }

        if (n > 0 && wait_) {
            wait_->cv_not_full.notify_all();
        }

        return n;
    }

    void wait_for_data(std::chrono::milliseconds dur) {
        if (!wait_) {
            std::this_thread::sleep_for(dur);
            return;
        }
        // Single consumer: sleep using semaphore to reduce overhead vs condition_variable.
        wait_->sleeping.store(true, std::memory_order_relaxed);
        if (wait_->stop.load(std::memory_order_relaxed)) {
            wait_->sleeping.store(false, std::memory_order_relaxed);
            return;
        }
        (void)wait_->sem_not_empty.try_acquire_for(dur);
        wait_->sleeping.store(false, std::memory_order_relaxed);
    }

    void signal_stop() {
        if (!wait_) return;
        wait_->stop.store(true, std::memory_order_relaxed);
        // Wake consumer if sleeping.
        wait_->sem_not_empty.release();
        wait_->cv_not_full.notify_all();
    }

    std::size_t size_relaxed() const noexcept {
        // Approximate size (may transiently include reserved-but-not-published slots).
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }
    std::size_t capacity() const noexcept { return cap_; }

private:
    struct cell {
        std::atomic<std::size_t> seq{0};
        alignas(T) unsigned char storage[sizeof(T)];
    };

    bool try_pop(T& out) {
        cell* c = nullptr;
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            c = &buffer_[pos & mask_];
            const std::size_t seq = c->seq.load(std::memory_order_acquire);
            const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        T* obj = reinterpret_cast<T*>(&c->storage);
        out = std::move(*obj);
        obj->~T();
        c->seq.store(pos + cap_, std::memory_order_release);
        return true;
    }

    std::size_t cap_;
    std::size_t mask_;
    std::unique_ptr<cell[]> buffer_;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    queue_wait* wait_;
};

} // namespace detail

template <class T>
class dual_queue {
public:
    explicit dual_queue(std::size_t total_cap)
        : wait_(),
          hi_(std::max<std::size_t>(1, total_cap / 4), &wait_),
          lo_(std::max<std::size_t>(1, total_cap - std::max<std::size_t>(1, total_cap / 4)), &wait_) {}

    bool try_push(T&& v, int weight) {
        if (weight >= 3) return hi_.try_push(std::move(v));
        return lo_.try_push(std::move(v));
    }

    void push_blocking(T&& v, int weight) {
        if (weight >= 3) hi_.push_blocking(std::move(v));
        else lo_.push_blocking(std::move(v));
    }

    std::size_t pop_batch(std::vector<T>& out, std::size_t max_batch) {
        std::size_t n = 0;
        n += hi_.pop_batch(out, max_batch);
        if (n < max_batch) n += lo_.pop_batch(out, max_batch - n);
        return n;
    }

    void wait_for_data(std::chrono::milliseconds dur) {
        if (size_relaxed() > 0) return;
        wait_.sleeping.store(true, std::memory_order_relaxed);
        if (wait_.stop.load(std::memory_order_relaxed)) {
            wait_.sleeping.store(false, std::memory_order_relaxed);
            return;
        }
        // If producers enqueue while we're sleeping, they will release sem_not_empty.
        (void)wait_.sem_not_empty.try_acquire_for(dur);
        wait_.sleeping.store(false, std::memory_order_relaxed);
    }

    void signal_stop() {
        wait_.stop.store(true, std::memory_order_relaxed);
        wait_.sem_not_empty.release();
        wait_.cv_not_full.notify_all();
    }

    std::size_t size_relaxed() const noexcept { return hi_.size_relaxed() + lo_.size_relaxed(); }

private:
    detail::queue_wait wait_;
    detail::mpsc_ring<T> hi_;
    detail::mpsc_ring<T> lo_;
};

// =========================== Thread Pool ===========================

class thread_pool {
public:
    explicit thread_pool(std::size_t n) : stop_(false) {
        if (n == 0) n = 1;
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(m_);
                        cv_.wait(lk, [&] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~thread_pool() { shutdown(); }

    void enqueue(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stop_) return;
            tasks_.push(std::move(f));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_;
};

// =========================== Logger ===========================

class logger {
public:
    explicit logger(logger_config cfg) : cfg_(std::move(cfg)), seq_(0) {
        single_threaded_ = cfg_.single_threaded;
        if (single_threaded_) {
            // Keep the runtime truly single-threaded: no worker thread, no pool.
            cfg_.async.enabled = false;
            cfg_.parallel_sinks = false;
        }

        if (cfg_.async.enabled) {
            queue_ = std::make_unique<dual_queue<log_event>>(cfg_.async.queue_capacity);
            worker_ = std::thread([this] { worker_loop(); });
        }

        // If the user explicitly opts into message-only output, default to skipping metadata
        // capture to maximize throughput.
        if (cfg_.pattern == "{msg}") {
            cfg_.capture_timestamp = false;
            cfg_.capture_thread_id = false;
            cfg_.capture_logger_name = false;
            cfg_.capture_source_location = false;
        }
    }

    ~logger() { shutdown(); }

    void add_sink(std::shared_ptr<sink> s) {
        if (single_threaded_) {
            s->set_pattern(cfg_.pattern);
            s->set_thread_safe(false);
            sinks_st_.push_back(std::move(s));
            return;
        }

        std::lock_guard<std::mutex> lk(sinks_mu_);
        s->set_pattern(cfg_.pattern);
        s->set_thread_safe(true);

        auto current = std::atomic_load_explicit(&sinks_, std::memory_order_acquire);
        if (!current) current = std::make_shared<sink_list>();
        auto next = std::make_shared<sink_list>(*current);
        next->push_back(std::move(s));
        std::atomic_store_explicit(&sinks_, std::static_pointer_cast<const sink_list>(next), std::memory_order_release);

        // NOTE: For peak performance in async mode, we keep sink writes on the single worker thread.
        // The thread_pool is only used for sync-mode parallel_sinks.
        if (!cfg_.async.enabled && cfg_.parallel_sinks && !pool_) {
            const std::size_t n = (cfg_.sink_pool_size != 0) ? cfg_.sink_pool_size : next->size();
            pool_ = std::make_unique<thread_pool>(n);
        }
    }

    void set_level(level lv) noexcept { cfg_.level = lv; }

    void set_pattern(std::string pat) {
        if (single_threaded_) {
            cfg_.pattern = std::move(pat);
            for (auto& s : sinks_st_) s->set_pattern(cfg_.pattern);
            return;
        }

        std::lock_guard<std::mutex> lk(sinks_mu_);
        cfg_.pattern = std::move(pat);
        auto current = std::atomic_load_explicit(&sinks_, std::memory_order_acquire);
        if (!current) return;
        for (auto& s : *current) s->set_pattern(cfg_.pattern);
    }

    void set_flush_on(level lv) noexcept { cfg_.flush_on_level = lv; }

    template <class Fmt, class... Args>
    void log(level lv, Fmt&& fmt, Args&&... args)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        if (static_cast<int>(lv) < static_cast<int>(cfg_.level)) return;
        if (cfg_.capture_source_location) {
            log_at(lv, std::source_location::current(), std::string_view(fmt), std::forward<Args>(args)...);
        } else {
            log_at_no_loc(lv, std::string_view(fmt), std::forward<Args>(args)...);
        }
    }

    // Fast path for compile-time checked format strings (avoids std::vformat).
    template <class... Args>
    void log(level lv, std::format_string<Args...> fmt, Args&&... args) {
        if (static_cast<int>(lv) < static_cast<int>(cfg_.level)) return;
        if (cfg_.capture_source_location) {
            log_at(lv, std::source_location::current(), fmt, std::forward<Args>(args)...);
        } else {
            log_at_no_loc(lv, fmt, std::forward<Args>(args)...);
        }
    }

    template <class Fmt, class... Args>
    void log_at(level lv, const std::source_location& loc, Fmt&& fmt, Args&&... args)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        if (static_cast<int>(lv) < static_cast<int>(cfg_.level)) return;

        log_event e;
        if (cfg_.capture_timestamp) e.ts = std::chrono::system_clock::now();
        e.lvl = lv;
        if (cfg_.capture_thread_id) e.tid = std::this_thread::get_id();
        if (cfg_.capture_logger_name) e.name = cfg_.name;
        e.loc = loc;
        try {
            e.payload = detail::vformat_payload(std::string_view(fmt), std::forward<Args>(args)...);
        } catch (...) {
            // Keep logging non-fatal even if formatting fails.
            e.payload = std::string(std::string_view(fmt));
        }
        if (single_threaded_) {
            e.seq = seq_st_++;
            sink_batch_write_one(e);
            ++enqueued_st_;
            ++dequeued_st_;
            if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
            return;
        }

        e.seq = seq_.fetch_add(1, std::memory_order_relaxed);

        if (cfg_.async.enabled) {
            const int w = level_weight(lv);
            const bool ok = queue_->try_push(std::move(e), w);
            if (ok) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (cfg_.async.drop_when_full) {
                    if (static_cast<int>(lv) >= static_cast<int>(level::warn)) {
                        queue_->push_blocking(std::move(e), w);
                        stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    queue_->push_blocking(std::move(e), w);
                    stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        // Sync mode
        sink_batch_write_one(e);
        if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
    }

    template <class... Args>
    void log_at(level lv, const std::source_location& loc, std::format_string<Args...> fmt, Args&&... args) {
        if (static_cast<int>(lv) < static_cast<int>(cfg_.level)) return;

        log_event e;
        if (cfg_.capture_timestamp) e.ts = std::chrono::system_clock::now();
        e.lvl = lv;
        if (cfg_.capture_thread_id) e.tid = std::this_thread::get_id();
        if (cfg_.capture_logger_name) e.name = cfg_.name;
        e.loc = loc;
        try {
            e.payload = detail::format_payload(fmt, std::forward<Args>(args)...);
        } catch (...) {
            // Keep logging non-fatal even if formatting fails.
            e.payload = std::string(fmt.get());
        }

        if (single_threaded_) {
            e.seq = seq_st_++;
            sink_batch_write_one(e);
            ++enqueued_st_;
            ++dequeued_st_;
            if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
            return;
        }

        e.seq = seq_.fetch_add(1, std::memory_order_relaxed);

        if (cfg_.async.enabled) {
            const int w = level_weight(lv);
            const bool ok = queue_->try_push(std::move(e), w);
            if (ok) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (cfg_.async.drop_when_full) {
                    if (static_cast<int>(lv) >= static_cast<int>(level::warn)) {
                        queue_->push_blocking(std::move(e), w);
                        stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    queue_->push_blocking(std::move(e), w);
                    stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        // Sync mode
        sink_batch_write_one(e);
        if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
    }

    template <class Fmt, class... Args>
    void log_at_no_loc(level lv, Fmt&& fmt, Args&&... args)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        // NOTE: capture_source_location is intentionally avoided on this path.
        log_event e;
        if (cfg_.capture_timestamp) e.ts = std::chrono::system_clock::now();
        e.lvl = lv;
        if (cfg_.capture_thread_id) e.tid = std::this_thread::get_id();
        if (cfg_.capture_logger_name) e.name = cfg_.name;
        try {
            e.payload = detail::vformat_payload(std::string_view(fmt), std::forward<Args>(args)...);
        } catch (...) {
            e.payload = std::string(std::string_view(fmt));
        }

        if (single_threaded_) {
            e.seq = seq_st_++;
            sink_batch_write_one(e);
            ++enqueued_st_;
            ++dequeued_st_;
            if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
            return;
        }

        e.seq = seq_.fetch_add(1, std::memory_order_relaxed);

        if (cfg_.async.enabled) {
            const int w = level_weight(lv);
            const bool ok = queue_->try_push(std::move(e), w);
            if (ok) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (cfg_.async.drop_when_full) {
                    if (static_cast<int>(lv) >= static_cast<int>(level::warn)) {
                        queue_->push_blocking(std::move(e), w);
                        stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    queue_->push_blocking(std::move(e), w);
                    stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        sink_batch_write_one(e);
        if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
    }

    template <class... Args>
    void log_at_no_loc(level lv, std::format_string<Args...> fmt, Args&&... args) {
        log_event e;
        if (cfg_.capture_timestamp) e.ts = std::chrono::system_clock::now();
        e.lvl = lv;
        if (cfg_.capture_thread_id) e.tid = std::this_thread::get_id();
        if (cfg_.capture_logger_name) e.name = cfg_.name;
        try {
            e.payload = detail::format_payload(fmt, std::forward<Args>(args)...);
        } catch (...) {
            e.payload = std::string(fmt.get());
        }

        if (single_threaded_) {
            e.seq = seq_st_++;
            sink_batch_write_one(e);
            ++enqueued_st_;
            ++dequeued_st_;
            if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
            return;
        }

        e.seq = seq_.fetch_add(1, std::memory_order_relaxed);

        if (cfg_.async.enabled) {
            const int w = level_weight(lv);
            const bool ok = queue_->try_push(std::move(e), w);
            if (ok) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (cfg_.async.drop_when_full) {
                    if (static_cast<int>(lv) >= static_cast<int>(level::warn)) {
                        queue_->push_blocking(std::move(e), w);
                        stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    queue_->push_blocking(std::move(e), w);
                    stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        sink_batch_write_one(e);
        if (static_cast<int>(lv) >= static_cast<int>(cfg_.flush_on_level)) flush();
    }

    template <class... Args>
    void trace(std::format_string<Args...> f, Args&&... a) { log(level::trace, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void trace(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::trace, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void debug(std::format_string<Args...> f, Args&&... a) { log(level::debug, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void debug(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::debug, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void info(std::format_string<Args...> f, Args&&... a) { log(level::info, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void info(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::info, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void warn(std::format_string<Args...> f, Args&&... a) { log(level::warn, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void warn(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::warn, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void error(std::format_string<Args...> f, Args&&... a) { log(level::error, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void error(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::error, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    template <class... Args>
    void critical(std::format_string<Args...> f, Args&&... a) { log(level::critical, f, std::forward<Args>(a)...); }
    template <class Fmt, class... Args>
    void critical(Fmt&& f, Args&&... a)
        requires(std::is_convertible_v<Fmt, std::string_view> &&
                 !(std::is_array_v<std::remove_reference_t<Fmt>> &&
                   std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<Fmt>>>, char>)) {
        log(level::critical, std::forward<Fmt>(f), std::forward<Args>(a)...);
    }

    void flush() {
        if (single_threaded_) {
            for (auto& s : sinks_st_) {
                try {
                    s->flush();
                } catch (...) {
                }
            }
            ++flushed_st_;
            return;
        }

        auto current = std::atomic_load_explicit(&sinks_, std::memory_order_acquire);
        if (current) {
            for (auto& s : *current) {
                try {
                    s->flush();
                } catch (...) {
                }
            }
        }
        stats_.flushed.fetch_add(1, std::memory_order_relaxed);
    }

    void shutdown() {
        if (single_threaded_) {
            flush();
            return;
        }

        bool expected = false;
        if (!stop_requested_.compare_exchange_strong(expected, true)) return;

        if (worker_.joinable()) {
            queue_->signal_stop();
            worker_.join();
        }

        if (pool_) pool_->shutdown();
        flush();
    }

    metrics_snapshot stats() const {
        metrics_snapshot snap;
        if (single_threaded_) {
            snap.dropped = dropped_st_;
            snap.enqueued = enqueued_st_;
            snap.dequeued = dequeued_st_;
            snap.flushed = flushed_st_;
            snap.queue_size = 0;
            return snap;
        }
        snap.dropped = stats_.dropped.load(std::memory_order_relaxed);
        snap.enqueued = stats_.enqueued.load(std::memory_order_relaxed);
        snap.dequeued = stats_.dequeued.load(std::memory_order_relaxed);
        snap.flushed = stats_.flushed.load(std::memory_order_relaxed);
        snap.queue_size = cfg_.async.enabled && queue_ ? queue_->size_relaxed() : 0;
        return snap;
    }

private:
    void sink_batch_write_one(const log_event& e) {
        if (single_threaded_) {
            for (auto& s : sinks_st_) {
                try {
                    if (static_cast<int>(e.lvl) >= static_cast<int>(s->level_threshold())) s->log(e);
                } catch (...) {
                }
            }
            return;
        }

        auto current = std::atomic_load_explicit(&sinks_, std::memory_order_acquire);
        if (!current) return;

        if (cfg_.parallel_sinks && pool_) {
            for (auto& s : *current) {
                pool_->enqueue([s, ev = e] {
                    try {
                        if (static_cast<int>(ev.lvl) >= static_cast<int>(s->level_threshold())) s->log(ev);
                    } catch (...) {
                    }
                });
            }
        } else {
            for (auto& s : *current) {
                try {
                    if (static_cast<int>(e.lvl) >= static_cast<int>(s->level_threshold())) s->log(e);
                } catch (...) {
                }
            }
        }
    }

    void worker_loop() {
        std::vector<log_event> batch;
        batch.reserve(cfg_.async.batch_max);
        auto last_flush = std::chrono::steady_clock::now();

        while (!stop_requested_.load(std::memory_order_relaxed)) {
            batch.clear();
            const std::size_t n = queue_->pop_batch(batch, cfg_.async.batch_max);
            if (n == 0) {
                queue_->wait_for_data(std::chrono::milliseconds(100));
            } else {
                stats_.dequeued.fetch_add(n, std::memory_order_relaxed);

                // Peak throughput mode: in async logging, keep sink writes on the single worker thread.
                // This avoids per-event task scheduling + extra copying.
                const auto sinks_snapshot = std::atomic_load_explicit(&sinks_, std::memory_order_acquire);
                if (sinks_snapshot) {
                    for (const auto& e : batch) {
                        for (auto& s : *sinks_snapshot) {
                            try {
                                if (static_cast<int>(e.lvl) >= static_cast<int>(s->level_threshold())) s->log(e);
                            } catch (...) {
                            }
                        }
                        if (static_cast<int>(e.lvl) >= static_cast<int>(cfg_.flush_on_level)) {
                            for (auto& s : *sinks_snapshot) {
                                try {
                                    s->flush();
                                } catch (...) {
                                }
                            }
                            stats_.flushed.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= cfg_.async.flush_every) {
                flush();
                last_flush = now;
            }
            stats_.queue_size.store(queue_->size_relaxed(), std::memory_order_relaxed);
        }

        // Drain
        std::vector<log_event> drain;
        drain.reserve(cfg_.async.batch_max);
        for (;;) {
            drain.clear();
            const std::size_t n = queue_->pop_batch(drain, cfg_.async.batch_max);
            if (n == 0) break;

            stats_.dequeued.fetch_add(n, std::memory_order_relaxed);

            const auto sinks_snapshot = std::atomic_load_explicit(&sinks_, std::memory_order_acquire);
            if (sinks_snapshot) {
                for (const auto& e : drain) {
                    for (auto& s : *sinks_snapshot) {
                        try {
                            if (static_cast<int>(e.lvl) >= static_cast<int>(s->level_threshold())) s->log(e);
                        } catch (...) {
                        }
                    }
                }
                for (auto& s : *sinks_snapshot) {
                    try {
                        s->flush();
                    } catch (...) {
                    }
                }
                stats_.flushed.fetch_add(1, std::memory_order_relaxed);
            }
        }

        stats_.queue_size.store(0, std::memory_order_relaxed);
    }

    logger_config cfg_;
    bool single_threaded_ = false;
    using sink_list = std::vector<std::shared_ptr<sink>>;
    std::atomic<std::shared_ptr<const sink_list>> sinks_{std::make_shared<sink_list>()};
    mutable std::mutex sinks_mu_;

    sink_list sinks_st_;

    std::atomic<std::uint64_t> seq_;
    std::uint64_t seq_st_ = 0;
    std::unique_ptr<dual_queue<log_event>> queue_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};

    metrics stats_;
    std::unique_ptr<thread_pool> pool_;

    // Single-threaded counters (no atomics).
    std::size_t dropped_st_ = 0;
    std::size_t enqueued_st_ = 0;
    std::size_t dequeued_st_ = 0;
    std::size_t flushed_st_ = 0;
};

// Convenience macros for capturing source_location without changing call-sites.
#define CHLOG_TRACE(lg, fmt, ...) (lg).log_at(::chlog::level::trace, std::source_location::current(), (fmt) __VA_OPT__(,) __VA_ARGS__)
#define CHLOG_DEBUG(lg, fmt, ...) (lg).log_at(::chlog::level::debug, std::source_location::current(), (fmt) __VA_OPT__(,) __VA_ARGS__)
#define CHLOG_INFO(lg, fmt, ...)  (lg).log_at(::chlog::level::info,  std::source_location::current(), (fmt) __VA_OPT__(,) __VA_ARGS__)
#define CHLOG_WARN(lg, fmt, ...)  (lg).log_at(::chlog::level::warn,  std::source_location::current(), (fmt) __VA_OPT__(,) __VA_ARGS__)
#define CHLOG_ERROR(lg, fmt, ...) (lg).log_at(::chlog::level::error, std::source_location::current(), (fmt) __VA_OPT__(,) __VA_ARGS__)
#define CHLOG_CRIT(lg, fmt, ...)  (lg).log_at(::chlog::level::critical, std::source_location::current(), (fmt) __VA_OPT__(,) __VA_ARGS__)

} // namespace chlog
