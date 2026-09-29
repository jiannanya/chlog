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
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include <system_error>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include <share.h>
#endif

#if defined(__APPLE__)
    #include <sys/time.h>
#endif

#if defined(CHLOG_USE_FMT)
    #include <fmt/format.h>
#else
    #include <format>
#endif

namespace chlog {

namespace detail {

// Borrow directly only for the exact built-in type. A subclass may override the
// owning log() callback; it must keep receiving that callback through log_view().
template <class T>
bool exact_sink_type(const T& output) noexcept {
#if defined(__cpp_rtti) || defined(_CPPRTTI)
    return typeid(output) == typeid(T);
#else
    (void)output;
    return false;
#endif
}

// Configuration locks are cold and do not need a platform mutex per object.
// Atomic waiting parks contending threads without retaining a spinning waiter.
class compact_mutex {
public:
    void lock() noexcept {
        unsigned char expected = 0;
        if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire)) return;
        while (state_.exchange(2, std::memory_order_acquire) != 0)
            state_.wait(2, std::memory_order_relaxed);
    }
    void unlock() noexcept {
        if (state_.exchange(0, std::memory_order_release) == 2) state_.notify_one();
    }
private:
    // 0: unlocked, 1: locked, 2: locked with possible waiters. A slow-path
    // owner preserves state 2 so every remaining waiter receives a handoff.
    std::atomic<unsigned char> state_{0};
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
inline constexpr bool plain_string_argument = sizeof...(Args) == 1 &&
    ((std::is_same_v<std::remove_cvref_t<Args>, std::string> ||
      std::is_same_v<std::remove_cvref_t<Args>, std::string_view>) && ...);

// ---------------------- Restricted-grammar fast formatting ----------------------
//
// The formatting functions re-parse their format string on every call: the
// compile-time check performed by format_string only validates the string, it
// does not pre-compile it. For a record like "v {}" that parse and its generic
// dispatch dominate the whole logging call, so the common shape - literal text
// with bare "{}" replacement fields and no format specs - is recognized here and
// executed directly.
//
// The subset is deliberately narrow, and everything else falls back to the
// general formatter, so behaviour is unchanged:
//   * a '{' must be immediately followed by '}' (no spec, no '{N}', no "{{"),
//   * a '}' outside a field, and any escaped brace, end the fast path,
//   * the number of fields must equal the number of arguments, so argument
//     mismatches keep reporting exactly what the general formatter reports,
//   * every argument type must have a representation that is provably identical
//     to the default (empty) format spec: integers and floating-point values via
//     std::to_chars (the shortest round-tripping decimal form the default spec
//     is defined to produce), bool as true/false, char as itself, and the string
//     types as their bytes.
//
// Types without such a guarantee (pointers, enums, custom formatters, types
// without a to_chars overload) disable the whole call at compile time, so the
// general formatter is used exactly once, and its exceptions still propagate.

// True when std::to_chars can produce the default representation of T. This
// excludes bool, char, and anything the implementation has no overload for (for
// example an extended 128-bit integer).
template <class T>
inline constexpr bool to_chars_default = requires(char* first, const T& value) {
    std::to_chars(first, first, value);
};

// The character-like integral types are excluded explicitly: their default
// representation depends on the formatting context, and an implementation may
// provide to_chars for them without the two agreeing. They do not occur in log
// format strings, and declining them costs nothing.
template <class T>
inline constexpr bool character_like = std::is_same_v<T, wchar_t> || std::is_same_v<T, char8_t> ||
    std::is_same_v<T, char16_t> || std::is_same_v<T, char32_t>;

template <class T>
inline constexpr bool bare_field_argument = [] {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, char>) return true;
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) return true;
    else if constexpr (std::is_array_v<U>)
        return std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>;
    else if constexpr (std::is_pointer_v<U>)
        return std::is_same_v<std::remove_cv_t<std::remove_pointer_t<U>>, char>;
    else if constexpr (character_like<U>) return false;
#if defined(CHLOG_USE_FMT)
    // fmt's default floating-point format is not std::to_chars' general format:
    // it switches to scientific notation in places where to_chars stays fixed
    // ("-3.9837757e+08" against "-398377568" for a float). Only the standard
    // formatter defines its empty spec as the shortest round-tripping to_chars
    // form, so with fmt every floating-point argument keeps the general
    // formatter. The equivalence test asserts this decline.
    else if constexpr (std::is_floating_point_v<U>) return false;
#else
    else if constexpr (std::is_floating_point_v<U> && to_chars_default<U>) return true;
#endif
    else if constexpr (std::is_integral_v<U> && to_chars_default<U>) return true;
    else return false;
}();

template <class... Args>
inline constexpr bool bare_field_arguments = (bare_field_argument<Args> && ...);

// One literal run of the format string. Fields sit between consecutive runs, so
// n fields are described by n + 1 runs.
struct bare_run {
    std::uint32_t begin;
    std::uint32_t size;
};

inline constexpr std::size_t bare_max_fields = 8;

// True when the text contains no brace at all, in which case there is nothing to
// substitute and the text itself is the payload. A single pass beats
// string_view::find_first_of, which builds a 256-entry lookup table per call:
// for the short format strings used in logging that setup costs more than
// reading the text outright.
inline bool has_no_braces(std::string_view text) noexcept {
    for (const char c : text)
        if (c == '{' || c == '}') return false;
    return true;
}

// Returns the number of replacement fields, or -1 when the text uses anything
// outside the subset. Nothing is written, so a rejected text leaves the output
// untouched for the general formatter.
inline int plan_bare_fields(std::string_view text, bare_run* runs, std::size_t max_runs) noexcept {
    if (text.size() > 0xFFFFFFFFu) return -1;
    std::size_t pos = 0, literal = 0, count = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '{') {
            if (pos + 1 >= text.size() || text[pos + 1] != '}') return -1;
            if (count + 2 > max_runs) return -1;
            runs[count].begin = static_cast<std::uint32_t>(literal);
            runs[count].size = static_cast<std::uint32_t>(pos - literal);
            ++count;
            pos += 2;
            literal = pos;
        } else if (c == '}') {
            return -1; // Escapes and stray braces keep the general formatter.
        } else {
            ++pos;
        }
    }
    if (count + 1 > max_runs) return -1;
    runs[count].begin = static_cast<std::uint32_t>(literal);
    runs[count].size = static_cast<std::uint32_t>(text.size() - literal);
    return static_cast<int>(count);
}

// Appends bytes with a member the output type actually provides. std::string
// takes (pointer, size); fmt's memory buffer takes a pointer pair. Both also
// provide push_back, which is all the fast path needs besides this.
inline void buffer_append(std::string& out, const char* data, std::size_t size) { out.append(data, size); }
#if defined(CHLOG_USE_FMT)
inline void buffer_append(fmt::basic_memory_buffer<char, 250>& out, const char* data, std::size_t size) {
    out.append(data, data + size);
}
#endif

template <class Output>
inline void append_literal(Output& out, const char* text, const bare_run& run) {
    if (run.size) buffer_append(out, text + run.begin, run.size);
}

template <class T, class = void>
struct reserve_tail_aware : std::false_type {};
template <class T>
struct reserve_tail_aware<T, std::void_t<decltype(std::declval<T&>().reserve_tail(std::size_t{})),
                                         decltype(std::declval<T&>().commit_tail(std::size_t{}))>>
    : std::true_type {};

// True for an output that can hand out a writable tail. Numbers are then written
// where they belong instead of into a scratch buffer and copied.
template <class T>
inline constexpr bool has_reserve_tail = reserve_tail_aware<T>::value;

template <class Output, class T>
inline void append_bare_value(Output& out, const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        static constexpr char yes[] = "true", no[] = "false";
        buffer_append(out, value ? yes : no, value ? 4u : 5u);
    } else if constexpr (std::is_same_v<U, char>) {
        out.push_back(value);
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) {
        if (!value.empty()) buffer_append(out, value.data(), value.size());
    } else if constexpr (std::is_array_v<U> || std::is_pointer_v<U>) {
        const auto* text = value;
        const auto size = std::char_traits<char>::length(text);
        if (size) buffer_append(out, text, size);
    } else if constexpr (has_reserve_tail<Output>) {
        // 64 bytes cover any value std::to_chars accepts here, including the
        // longest shortest-round-trip floating-point form.
        auto* digits = out.reserve_tail(64);
        const auto result = std::to_chars(digits, digits + 64, value);
        out.commit_tail(static_cast<std::size_t>(result.ptr - digits));
    } else {
        char digits[64];
        const auto result = std::to_chars(digits, digits + sizeof(digits), value);
        buffer_append(out, digits, static_cast<std::size_t>(result.ptr - digits));
    }
}

// A null C string is undefined behaviour for the general formatter too, but fmt
// prints "(null)" for it. Rejecting it here keeps that diagnostic reachable by
// falling back instead of dereferencing the pointer.
//
// Non-finite floating point is rejected for a different reason: to_chars appends
// the NaN payload ("nan(snan)", "-nan(ind)"), while the default format spec must
// print just "nan" / "-nan", and the exact spelling is the implementation's. The
// general formatter settles it, and infinity and NaN are rare enough that losing
// the fast path for them costs nothing.
template <class T>
inline bool bare_argument_valid(const T& value) noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_pointer_v<U> &&
                  std::is_same_v<std::remove_cv_t<std::remove_pointer_t<U>>, char>)
        return value != nullptr;
    else if constexpr (std::is_floating_point_v<U>)
        return std::isfinite(value);
    else return true;
}

template <class... Args>
inline bool bare_arguments_valid(const Args&... args) noexcept {
    return (bare_argument_valid(args) && ...);
}

// Emits the literal that precedes one field and then the field itself, so a
// comma fold over the arguments walks the runs and the fields in lockstep.
template <class Output, class T>
inline void emit_bare_field(Output& out, const char* text, const bare_run* runs,
                            std::size_t& run, const T& value) {
    append_literal(out, text, runs[run]);
    ++run;
    append_bare_value(out, value);
}

// Emits a plan produced by plan_bare_fields() for arguments already checked with
// bare_arguments_valid(). Declining and rendering are separate steps so a caller
// can decide before it takes any per-record bookkeeping (a log_guard must be
// entered exactly once per accepted record).
template <class Output, class... Args>
inline void emit_bare_fields(Output& out, const char* text, const bare_run* runs, const Args&... args) {
    std::size_t run = 0;
    (emit_bare_field(out, text, runs, run, args), ...);
    append_literal(out, text, runs[run]);
}

template <class Output, class... Args>
inline bool format_bare_fields(Output& out, std::string_view text, const Args&... args) {
    bare_run runs[bare_max_fields + 1];
    const auto fields = plan_bare_fields(text, runs, bare_max_fields + 1);
    if (fields != static_cast<int>(sizeof...(Args))) return false;
    if (!bare_arguments_valid(args...)) return false;
    emit_bare_fields(out, text.data(), runs, args...);
    return true;
}

template <class... Args>
inline std::string format_payload(format_string<Args...> fmt, Args&&... args) {
    if constexpr (plain_string_argument<Args...>) {
        // The unadorned string field already is the formatted payload. Keep
        // ownership semantics: formatting an rvalue does not consume it.
        if (format_view<Args...>(fmt) == "{}") return std::string(std::string_view(args)...);
    }
    if constexpr (sizeof...(Args) == 0) {
        const auto text = format_view<>(fmt);
        if (has_no_braces(text)) return std::string(text);
    }
    if constexpr (bare_field_arguments<Args...>) {
        std::string out;
        if (format_bare_fields(out, format_view<Args...>(fmt), args...)) return out;
    }
#if defined(CHLOG_USE_FMT)
    return fmt::format(fmt, std::forward<Args>(args)...);
#else
    return std::format(fmt, std::forward<Args>(args)...);
#endif
}

template <class... Args>
inline std::string vformat_payload(std::string_view fmt, Args&&... args) {
    if constexpr (plain_string_argument<Args...>) {
        if (fmt == "{}") return std::string(std::string_view(args)...);
    }
    if constexpr (sizeof...(Args) == 0) {
        if (has_no_braces(fmt)) return std::string(fmt);
    }
    if constexpr (bare_field_arguments<Args...>) {
        std::string out;
        if (format_bare_fields(out, fmt, args...)) return out;
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
    std::size_t sink_pool_size = 0; // 0 => one worker per sink (also forces a pool with one sink if > 0)
    std::size_t sink_queue_capacity = 1024; // bounded pending tasks; producers block when full
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

// Copies a short run inline instead of calling the C library. Record rendering is
// a long series of small copies (literal runs, field separators, digits), and on
// a dynamically linked target the call plus its lazy-binding stub costs more than
// the handful of bytes it moves. Longer runs are still handed to memcpy.
inline void copy_bytes(char* destination, const char* source, std::size_t count) noexcept {
    if (count <= 16) {
        for (std::size_t i = 0; i < count; ++i) destination[i] = source[i];
        return;
    }
    std::memcpy(destination, source, count);
}

// Storage blocks for records that outgrow the inline capacity of text_buffer.
//
// Without this, every record longer than the inline area costs one allocation
// and one free, even when the same thread immediately logs another record of
// the same shape - the buffer itself is a local, so its block died with it.
// Blocks up to block_retain_limit are therefore returned to a short per-thread
// free list instead of being deleted, and grow() takes one from that list
// before asking the allocator.
//
// Ownership stays exclusive: the list only ever holds blocks that no live
// buffer points at. A sink rendering while an enclosing record buffer is still
// alive (dispatch happens before the producer's buffer is destroyed) simply
// takes the second block, so nothing is ever shared and a sink that logs from
// inside its own log() cannot alias either. At most two blocks are kept and
// anything larger than block_retain_limit is freed at once, so a thread never
// pins more than 2 * block_retain_limit bytes no matter what it logged; the
// list is destroyed when the thread ends and releases everything it still holds.
inline constexpr std::size_t block_retain_limit = 8192;

class block_list;

// Whether this thread's list is still alive. A thread_local with a trivial
// destructor has a lifetime that lasts until the thread ends, with no
// destruction order to worry about, so this flag can still be read after the
// list's own destructor has run. That happens when a thread_local object that
// was created before the list logs a long record on its way out.
inline bool& local_blocks_alive() noexcept;

class block_list {
public:
    block_list() = default;
    ~block_list() noexcept {
        clear();
        local_blocks_alive() = false;
    }
    block_list(const block_list&) = delete;
    block_list& operator=(const block_list&) = delete;

    // Smallest retained block that can hold `need` bytes; nullptr when none
    // fits. Best fit keeps the larger blocks available for the longer record.
    char* take(std::size_t need, std::size_t& size) noexcept {
        std::size_t best = count_;
        for (std::size_t i = 0; i < count_; ++i)
            if (sizes_[i] >= need && (best == count_ || sizes_[i] < sizes_[best])) best = i;
        if (best == count_) return nullptr;
        char* block = blocks_[best];
        size = sizes_[best];
        --count_;
        blocks_[best] = blocks_[count_];
        sizes_[best] = sizes_[count_];
        return block;
    }
    void put(char* block, std::size_t size) noexcept {
        // Outliers are released immediately: a one-off megabyte record must not
        // become this thread's permanent footprint.
        if (size > block_retain_limit) {
            ::operator delete[](block);
            return;
        }
        if (count_ == slots) {
            // Full. Keep the larger of the smallest resident block and the
            // incoming one instead of always dropping the newcomer: a record
            // whose length drifts by a byte (an extra digit in {seq}) would
            // otherwise leave two blocks that are each one byte too small and
            // evict every block that could have served the next record.
            std::size_t victim = 0;
            for (std::size_t i = 1; i < slots; ++i)
                if (sizes_[i] < sizes_[victim]) victim = i;
            if (sizes_[victim] >= size) {
                ::operator delete[](block);
                return;
            }
            ::operator delete[](blocks_[victim]);
            blocks_[victim] = block;
            sizes_[victim] = size;
            return;
        }
        blocks_[count_] = block;
        sizes_[count_] = size;
        ++count_;
    }
private:
    void clear() noexcept {
        for (std::size_t i = 0; i < count_; ++i) ::operator delete[](blocks_[i]);
        count_ = 0;
    }
    static constexpr std::size_t slots = 2;
    char* blocks_[slots]{};
    std::size_t sizes_[slots]{};
    std::size_t count_ = 0;
};

inline block_list& local_blocks() noexcept {
    static thread_local block_list list;
    return list;
}

inline bool& local_blocks_alive() noexcept {
    static thread_local bool alive = true;
    return alive;
}

// Returning a block to the thread-local list from the destructor.
//
// This is deliberately not inlined. text_buffer is destroyed once per record,
// including every record that stayed inside the inline area and never touched
// the free list, so the destructor must stay small enough that callers inline
// it down to the heap_ test; pulling the list bookkeeping (and the thread-local
// guard that reaches it) into the destructor body instead costs an out-of-line
// call per record.
//
// The alive test is what keeps a record logged from a thread_local destructor
// safe: if that destructor outlives the list, the block is released directly
// instead of being handed to a destroyed object.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline void recycle_block(char* block, std::size_t size) noexcept {
    if (local_blocks_alive()) local_blocks().put(block, size);
    else ::operator delete[](block);
}

// A record-local buffer: short output stays on the stack. A record that
// outgrows the inline area takes its block from local_blocks() on the way up
// and hands it back on the way out, so only the first long record of a given
// shape on a thread touches the allocator.
class text_buffer {
public:
    using value_type = char;
    text_buffer() = default;
    text_buffer(const text_buffer&) = delete;
    text_buffer& operator=(const text_buffer&) = delete;
    ~text_buffer() noexcept {
        // Guarded by the null test first: a record that stayed inline never
        // reaches the thread-local, which is the overwhelmingly common case.
        if (heap_) recycle_block(heap_.release(), capacity_);
    }
    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::string_view view() const noexcept { return {data_, size_}; }
    void append(const char* text, std::size_t count) {
        if (count > capacity_ - size_) grow(count);
        if (count) copy_bytes(data_ + size_, text, count);
        size_ += count;
    }
    void append(std::string_view text) { append(text.data(), text.size()); }
    text_buffer& operator+=(std::string_view text) { append(text); return *this; }
    void push_back(char c) {
        if (size_ == capacity_) grow(1);
        data_[size_++] = c;
    }
    // Room for a producer that writes the bytes itself, for example std::to_chars
    // for a number: it skips both an intermediate buffer and the copy of it. Only
    // the bytes passed to commit_tail() are ever part of the value.
    char* reserve_tail(std::size_t count) {
        if (count > capacity_ - size_) grow(count);
        return data_ + size_;
    }
    void commit_tail(std::size_t count) noexcept { size_ += count; }
private:
    void grow(std::size_t extra) {
        constexpr auto limit = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
        if (extra > limit - size_) throw std::length_error("chlog: rendered record too large");
        const auto required = size_ + extra;
        // Leave room for the newline and small suffixes after a large payload.
        const auto next = (std::max)(required + (std::min)(std::size_t{64}, limit - required),
                                    capacity_ + (std::min)(capacity_ / 2, limit - capacity_));
        std::size_t reused = 0;
        // Reuse asks only for the bytes actually needed, not for the growth
        // headroom above: a resident block that fits the record exactly is
        // still reusable, and rejecting it would allocate a fresh block (and
        // retain the old one) every time a record grows by a byte - an extra
        // digit in {seq} is enough. The headroom matters only when allocating.
        // A grow from a thread_local destructor can run after the list is gone,
        // in which case this allocates as it did before the free list existed.
        char* block = local_blocks_alive() ? local_blocks().take(required, reused) : nullptr;
        if (block) {
            if (size_) copy_bytes(block, data_, size_);
            heap_.reset(block);
            data_ = block;
            capacity_ = reused;
            return;
        }
        auto storage = std::unique_ptr<char[]>(new char[next]);
        if (size_) copy_bytes(storage.get(), data_, size_);
        heap_ = std::move(storage);
        data_ = heap_.get();
        capacity_ = next;
    }
    char inline_[512];
    std::unique_ptr<char[]> heap_;
    char* data_ = inline_;
    std::size_t size_ = 0, capacity_ = sizeof(inline_);
};

// The rendering helpers below are declared with the fast-formatting group much
// earlier in this namespace, but this overload can only be written once
// text_buffer is complete. Both are found by argument-dependent lookup at the
// point where the helper templates are instantiated, so the split is invisible
// to callers.
inline void buffer_append(text_buffer& out, const char* data, std::size_t size) { out.append(data, size); }

// The owning sink serializes access, or explicitly operates in single-threaded
// mode. Accumulate short records before entering the CRT; large writes bypass
// the buffer. Storage is allocated on first use and retained across rotation.
class file_writer {
public:
    file_writer() = default;
    file_writer(const file_writer&) = delete;
    file_writer& operator=(const file_writer&) = delete;
    ~file_writer() { try { close(); } catch (...) {} }
    bool is_open() const noexcept { return file_ != nullptr; }
    void open(const std::filesystem::path& path) {
        if (file_) throw std::logic_error("chlog: file already open");
#ifdef _WIN32
        // Preserve stream-style sharing so readers can tail an active log.
        file_ = ::_wfsopen(path.c_str(), L"ab", _SH_DENYNO);
#else
        file_ = std::fopen(path.c_str(), "ab");
#endif
        if (!file_) fail();
        error_ = 0;
        used_ = 0;
        // Use one buffering layer. The CRT still owns the file handle and keeps
        // platform path/sharing semantics, but allocates no second data buffer.
        if (std::setvbuf(file_, nullptr, _IONBF, 0) != 0) fail();
    }
    void write(const char* data, std::size_t size) {
        check();
        if (buffer_ && size <= buffer_capacity - used_) {
            if (size) std::memcpy(buffer_.get() + used_, data, size);
            used_ += size;
            return;
        }
        write_tail(data, size);
    }
    void write_line(std::string_view text) {
        check();
        if (buffer_ && text.size() < buffer_capacity - used_) {
            if (!text.empty()) std::memcpy(buffer_.get() + used_, text.data(), text.size());
            used_ += text.size();
            buffer_[used_++] = '\n';
            return;
        }
        write(text.data(), text.size());
        write("\n", 1);
    }
    void flush() {
        check();
        drain();
#ifdef _WIN32
        const auto result = ::_fflush_nolock(file_);
#else
        const auto result = std::fflush(file_);
#endif
        if (result != 0) fail();
    }
    void close() {
        if (!file_) return;
        try { flush(); }
        catch (...) {
            std::fclose(std::exchange(file_, nullptr));
            used_ = 0;
            throw;
        }
        auto* file = std::exchange(file_, nullptr);
        error_ = 0;
        if (std::fclose(file) != 0) fail();
    }
private:
    void write_direct(const char* data, std::size_t size) {
#ifdef _WIN32
        const auto written = ::_fwrite_nolock(data, 1, size, file_);
#elif defined(__GLIBC__) && defined(__USE_MISC)
        const auto written = ::fwrite_unlocked(data, 1, size, file_);
#else
        const auto written = std::fwrite(data, 1, size, file_);
#endif
        if (written != size) fail();
    }
    void drain() {
        // A failed partial write must never be replayed by close/destruction.
        const auto size = std::exchange(used_, 0);
        if (size) write_direct(buffer_.get(), size);
    }
    void write_tail(const char* data, std::size_t size) {
        if (used_) {
            const auto count = (std::min)(size, buffer_capacity - used_);
            std::memcpy(buffer_.get() + used_, data, count);
            used_ += count;
            data += count;
            size -= count;
            drain();
        }
        if (size >= buffer_capacity) { write_direct(data, size); return; }
        if (!size) return;
        if (!buffer_) buffer_.reset(new char[buffer_capacity]);
        std::memcpy(buffer_.get(), data, size);
        used_ = size;
    }
    void check() const {
        if (error_ || !file_)
            throw std::ios_base::failure("chlog: log file unavailable",
                std::error_code(error_ ? error_ : EBADF, std::generic_category()));
    }
    [[noreturn]] void fail() {
        error_ = errno ? errno : EIO;
        throw std::ios_base::failure("chlog: log file I/O", std::error_code(error_, std::generic_category()));
    }
    std::FILE* file_ = nullptr;
    static constexpr std::size_t buffer_capacity = 4096;
    std::unique_ptr<char[]> buffer_;
    std::size_t used_ = 0;
    int error_ = 0;
};

struct time_parts {
    std::chrono::sys_seconds second{};
    bool valid = false;
    std::array<char, 20> timestamp{};
};

// Wall-clock sample used for record timestamps.
//
// std::chrono::system_clock is specified to read CLOCK_REALTIME and, on this
// platform, has a 1/1_000_000 s period backed by gettimeofday(). Calling
// gettimeofday() directly therefore yields the exact same time_point value
// while skipping the extra libc chrono/clock_gettime wrapper hop, which showed
// up as ~75% of the compiled-pattern hot path. Everywhere else we keep using
// system_clock::now() so the observable timestamp source never changes.
inline std::chrono::system_clock::time_point wall_now() noexcept {
#if defined(__APPLE__)
    ::timeval tv{};
    ::gettimeofday(&tv, nullptr);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds{tv.tv_sec} + std::chrono::microseconds{tv.tv_usec}));
#else
    return std::chrono::system_clock::now();
#endif
}

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

template <class Output>
inline void append_milliseconds(Output& out, std::chrono::system_clock::time_point tp) {
    auto ms = std::chrono::floor<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    if (ms < 0) ms += 1000;
    out.push_back(static_cast<char>('0' + ms / 100));
    out.push_back(static_cast<char>('0' + ms / 10 % 10));
    out.push_back(static_cast<char>('0' + ms % 10));
}

template <class Output, class T>
inline void append_number(Output& out, T value) {
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, static_cast<std::size_t>(result.ptr - buf));
}

inline bool has_zero_byte(std::uint64_t word) noexcept {
    return ((word - 0x0101010101010101ULL) & ~word & 0x8080808080808080ULL) != 0;
}

template <class Output>
inline void append_json_escaped(Output& out, std::string_view s) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::size_t begin = 0, pos = 0;
    while (pos < s.size()) {
        // Scan ordinary text eight bytes at a time without reading past the
        // string or assuming alignment. UTF-8 bytes pass through unchanged.
        while (s.size() - pos >= sizeof(std::uint64_t)) {
            std::uint64_t word;
            std::memcpy(&word, s.data() + pos, sizeof(word));
            if (has_zero_byte(word & 0xE0E0E0E0E0E0E0E0ULL) ||
                has_zero_byte(word ^ 0x2222222222222222ULL) ||
                has_zero_byte(word ^ 0x5C5C5C5C5C5C5C5CULL)) break;
            pos += sizeof(word);
        }
        while (pos < s.size()) {
            const auto c = static_cast<unsigned char>(s[pos]);
            if (c < 0x20 || c == '"' || c == '\\') break;
            ++pos;
        }
        if (pos == s.size()) break;
        if (pos != begin) out.append(s.data() + begin, pos - begin);
        const auto c = static_cast<unsigned char>(s[pos++]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
        }
        begin = pos;
    }
    if (pos != begin) out.append(s.data() + begin, pos - begin);
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
template <class Event, class Output>
inline void render_json_to(const Event& e, Output& out) {
    if constexpr (std::is_same_v<Output, std::string>)
        out.reserve(out.size() + 160 + e.name.size() + e.payload.size() +
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
}
template <class Event>
inline std::string render_json(const Event& e) {
    std::string out;
    render_json_to(e, out);
    return out;
}

class compiled_pattern {
    enum class token { literal, ts, date, time, ms, lvl, tid, name, msg, file, line, func };
    struct part { token kind; std::size_t begin; std::size_t size; };
    std::string pattern_;
    std::vector<part> parts_;
    bool needs_calendar_ = false;
public:
    explicit compiled_pattern(std::string pattern) : pattern_(std::move(pattern)) {
        if (pattern_ == "{msg}" || pattern_ == "{json}") return;
        constexpr std::string_view names[] = {"", "ts", "date", "time", "ms", "lvl", "tid", "name", "msg", "file", "line", "func"};
        std::size_t literal = 0;
        for (std::size_t pos = 0; pos < pattern_.size(); ++pos) {
            if (pattern_[pos] != '{') continue;
            constexpr std::string_view calendar = "{date} {time}.{ms}";
            if (std::string_view(pattern_).substr(pos).starts_with(calendar)) {
                // This common spelling is exactly {ts}. Coalesce it while
                // compiling, avoiding five separate parts on every record.
                if (pos > literal) parts_.push_back({token::literal, literal, pos - literal});
                parts_.push_back({token::ts, 0, 0});
                needs_calendar_ = true;
                pos += calendar.size() - 1;
                literal = pos + 1;
                continue;
            }
            const auto end = pattern_.find('}', pos + 1);
            if (end == std::string::npos) break;
            const auto key = std::string_view(pattern_).substr(pos + 1, end - pos - 1);
            for (std::size_t i = 1; i < std::size(names); ++i) {
                if (key != names[i]) continue;
                if (pos > literal) parts_.push_back({token::literal, literal, pos - literal});
                parts_.push_back({static_cast<token>(i), 0, 0});
                if (i <= static_cast<std::size_t>(token::time)) needs_calendar_ = true;
                pos = end;
                literal = end + 1;
                break;
            }
        }
        if (literal < pattern_.size()) parts_.push_back({token::literal, literal, pattern_.size() - literal});
    }

    template <class Event, class Output>
    void render_to(const Event& e, Output& out) const {
        if (pattern_ == "{msg}") { out += e.payload; return; }
        if (pattern_ == "{json}") { render_json_to(e, out); return; }
        if constexpr (std::is_same_v<Output, std::string>)
            if (!parts_.empty()) out.reserve(out.size() + pattern_.size() + e.payload.size() + e.name.size() + 64);
        const auto* calendar = needs_calendar_ ? cached_time(e.ts).timestamp.data() : nullptr;
        for (const auto& p : parts_) {
            switch (p.kind) {
                case token::literal: out.append(pattern_.data() + p.begin, p.size); break;
                case token::ts:
                    out.append(calendar, 19);
                    out.push_back('.');
                    append_milliseconds(out, e.ts);
                    break;
                case token::date: out.append(calendar, 10); break;
                case token::time: out.append(calendar + 11, 8); break;
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
    }
    template <class Event>
    std::string render(const Event& e) const {
        std::string out;
        render_to(e, out);
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

    bool message_pattern() const noexcept {
        return pattern_kind_.load(std::memory_order_acquire) == pattern_kind::message;
    }

    template <class Event>
    bool accepts(const Event& e) const {
        const auto threshold = level_threshold();
        return e.lvl != level::off && threshold != level::off && e.lvl >= threshold;
    }
    // Appends one record; callers may reuse a string or use a local buffer.
    template <class Event, class Output>
    void render_to(const Event& e, Output& out) const {
        for (;;) {
            switch (pattern_kind_.load(std::memory_order_acquire)) {
                case pattern_kind::message: out += e.payload; return;
                case pattern_kind::json: detail::render_json_to(e, out); return;
                case pattern_kind::default_pattern: default_pattern()->render_to(e, out); return;
                case pattern_kind::compiled: {
                    if (!thread_safe_) { compiled_->render_to(e, out); return; }
                    if constexpr (std::is_same_v<Output, detail::text_buffer>) {
                        // Local rendering has no user callback or I/O. Keep the
                        // snapshot alive with the lock instead of two refcounts.
                        std::lock_guard<detail::compact_mutex> lock(pattern_mutex_);
                        if (compiled_) { compiled_->render_to(e, out); return; }
                    } else {
                        std::shared_ptr<const detail::compiled_pattern> compiled;
                        {
                            std::lock_guard<detail::compact_mutex> lock(pattern_mutex_);
                            compiled = compiled_;
                        }
                        if (compiled) { compiled->render_to(e, out); return; }
                    }
                    // A simple pattern replaced the snapshot before the lock.
                    break;
                }
            }
        }
    }
    template <class Event>
    std::string render(const Event& e) const {
        for (;;) {
            switch (pattern_kind_.load(std::memory_order_acquire)) {
                case pattern_kind::message: return std::string(e.payload);
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

    bool uses_views() const noexcept override { return detail::exact_sink_type(*this); }
    void log(const log_event& e) override { consume(e); }
    void log_view(const log_event_view& e) override {
        if (detail::exact_sink_type(*this)) consume(e);
        else sink::log_view(e);
    }

private:
    template <class Event>
    void consume(const Event& e) {
        if (!accepts(e)) return;
        detail::text_buffer line;
        if (style_ == style::color) line += color_of(e.lvl);
        render_to(e, line);
        if (style_ == style::color) line += "\x1b[0m";
        line.push_back('\n');
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        std::cout.write(line.data(), static_cast<std::streamsize>(line.size()));
    }

public:
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
        open();
    }

    bool uses_views() const noexcept override { return detail::exact_sink_type(*this); }
    void log(const log_event& e) override { consume(e); }
    void log_view(const log_event_view& e) override {
        if (detail::exact_sink_type(*this)) consume(e);
        else sink::log_view(e);
    }

private:
    template <class Event>
    void consume(const Event& e) {
        if (!accepts(e)) return;
        if (message_pattern()) write_record(e.payload);
        else {
            detail::text_buffer line;
            render_to(e, line);
            write_record(line.view());
        }
    }

public:
    void flush() override {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (file_.is_open()) file_.flush();
    }

private:
    void write_record(std::string_view line) {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (!file_.is_open()) open();
        // Account for the newline before rotation; never split a record across files.
        if (bytes_ > 0 && (bytes_ >= max_bytes_ || line.size() >= max_bytes_ - bytes_)) rotate();
        file_.write_line(line);
        bytes_ += line.size() + 1;
    }
    std::filesystem::path numbered(std::size_t n) const {
        auto result = path_;
        result += "." + std::to_string(n);
        return result;
    }
    void open() {
        file_.open(path_);
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
    detail::file_writer file_;
    std::size_t bytes_ = 0;
    std::mutex m_;
};

class daily_file_sink : public sink {
public:
    explicit daily_file_sink(std::filesystem::path dir) : dir_(std::move(dir)) {
        if (!dir_.empty()) std::filesystem::create_directories(dir_);
        open(date_string(std::chrono::system_clock::now()));
    }
    unsigned required_metadata() const noexcept override { return timestamp; }

    bool uses_views() const noexcept override { return detail::exact_sink_type(*this); }
    void log(const log_event& e) override { consume(e); }
    void log_view(const log_event_view& e) override {
        if (detail::exact_sink_type(*this)) consume(e);
        else sink::log_view(e);
    }

private:
    template <class Event>
    void consume(const Event& e) {
        if (!accepts(e)) return;
        const auto day = date_string(e.ts);
        detail::text_buffer line;
        render_to(e, line);
        line.push_back('\n');
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (day != current_day_ || !file_.is_open()) {
            if (file_.is_open()) file_.close();
            open(day);
        }
        file_.write(line.data(), line.size());
    }

public:
    void flush() override {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        if (file_.is_open()) file_.flush();
    }

private:
    void open(const std::string& day) {
        file_.open(dir_ / (day + ".log"));
        current_day_ = day;
    }
    std::filesystem::path dir_;
    std::string current_day_;
    detail::file_writer file_;
    std::mutex m_;
};

class json_sink : public sink {
public:
    explicit json_sink(std::filesystem::path path) {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        file_.open(path);
    }
    unsigned required_metadata() const noexcept override { return all_metadata; }

    bool uses_views() const noexcept override { return detail::exact_sink_type(*this); }
    void log(const log_event& e) override { consume(e); }
    void log_view(const log_event_view& e) override {
        if (detail::exact_sink_type(*this)) consume(e);
        else sink::log_view(e);
    }

private:
    template <class Event>
    void consume(const Event& e) {
        if (!accepts(e)) return;
        detail::text_buffer line;
        detail::render_json_to(e, line);
        line.push_back('\n');
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        file_.write(line.data(), line.size());
    }

public:
    void flush() override {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        if (thread_safe_) lk.lock();
        file_.flush();
    }
private:
    detail::file_writer file_;
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

// Bounded spin-wait relaxation. The readiness check is an acquire load, so the
// loop already re-reads memory; this helper only keeps that loop opaque to the
// optimizer. It deliberately uses no CPU-specific instruction and no inline
// assembly: a target that provides a "pause"/"yield" hint is not required, and
// every architecture compiles the same code. Correctness never depends on the
// hint - the semaphore handshake below covers every sleep transition - so the
// spin phase is a latency optimization only.
inline void spin_backoff(unsigned iteration) noexcept {
    if (iteration < 32u) std::atomic_signal_fence(std::memory_order_seq_cst);
    else std::this_thread::yield();
}

// Lock for extremely short critical sections (a few pointer or index updates).
// Parking a thread and waking it again costs far more than the work it protects,
// so the contending thread spins instead and only yields after a bounded number
// of attempts. Never used for critical sections that can block or allocate.
class spin_mutex {
public:
    void lock() noexcept {
        for (unsigned retry = 0; flag_.exchange(true, std::memory_order_acquire); ++retry)
            while (flag_.load(std::memory_order_relaxed)) spin_backoff(retry);
    }
    void unlock() noexcept { flag_.store(false, std::memory_order_release); }
private:
    std::atomic<bool> flag_{false};
};

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
            spin_backoff(retry);
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

// =========================== Parallel Sink Dispatch ===========================
//
// One record fanned out to N sinks used to cost N+ allocator calls plus a
// std::function, a queue node and a shared_ptr, and every task took a mutex and
// woke a condition variable. The structures below keep the same observable
// behaviour (one task per sink, bounded pending tasks, per-record level flushes,
// one dequeued count per record) with no per-record allocation:
//   * a task is a trivially copyable {job, sink} pair,
//   * jobs are recycled through a chunked free list, and
//   * pending tasks live in a bounded task ring drained in batches.
namespace detail {

// Unit of work for the parallel sink pool. Trivially copyable, so it can be
// stored in a ring directly instead of behind std::function.
struct sink_task {
    struct parallel_job* job = nullptr;
    sink* output = nullptr;
};

// One record in flight. Tasks keep it alive with an intrusive reference count;
// the last task to finish returns it to the pool.
struct parallel_job {
    log_event event;
    std::atomic<std::size_t> remaining{0};
    bool flush_event = false;
    parallel_job* next_free = nullptr;
};

// Chunked job recycling for the parallel pool. Chunks are held until the
// owning pool is destroyed, so a handed-out job
// pointer stays valid for the pool's lifetime; the number of live jobs is
// bounded by the high-water mark of records in flight, not by the record count.
//
// The free list is deliberately split so that no lock is shared between the
// logging threads and the workers:
//   * a worker retires a finished job onto a lock-free stack with one CAS; a
//     node is never unlinked individually (a producer takes the whole stack at
//     once), so no ABA tagging is needed on this path;
//   * a logging thread acquires from its own cached list and only touches the
//     shared stack - or allocates a chunk - when that list runs dry.
// The only lock is the one protecting the acquire cache, which workers never
// touch. It therefore stays on the logging threads' side instead of bouncing
// between producers and workers for every record.
class job_pool {
public:
    parallel_job* acquire() {
        std::lock_guard<spin_mutex> lock(mutex_);
        if (!cache_) {
            cache_ = returns_.exchange(nullptr, std::memory_order_acq_rel);
            if (!cache_) grow();
        }
        auto* job = cache_;
        cache_ = job->next_free;
        job->next_free = nullptr;
        return job;
    }
    // Called by the worker that completes the record.
    void retire(parallel_job* job) noexcept {
        // Payload and name are not retained: the next record move-assigns a
        // freshly built string into the job, which frees these buffers anyway.
        // Releasing them here keeps a burst of large messages from pinning
        // memory for the pool's lifetime and moves the free off the producer.
        std::string{}.swap(job->event.payload);
        std::string{}.swap(job->event.name);
        job->flush_event = false;
        job->remaining.store(0, std::memory_order_relaxed);
        auto* head = returns_.load(std::memory_order_relaxed);
        do {
            job->next_free = head;
        } while (!returns_.compare_exchange_weak(head, job, std::memory_order_release,
                                                 std::memory_order_relaxed));
    }
private:
    void grow() {
        constexpr std::size_t chunk = 32;
        std::unique_ptr<parallel_job[]> block(new parallel_job[chunk]);
        for (std::size_t i = 0; i < chunk; ++i) {
            block[i].next_free = cache_;
            cache_ = &block[i];
        }
        chunks_.push_back(std::move(block));
    }
    mutable spin_mutex mutex_;
    parallel_job* cache_ = nullptr;
    alignas(64) std::atomic<parallel_job*> returns_{nullptr};
    std::vector<std::unique_ptr<parallel_job[]>> chunks_;
};

// Tasks are drained in batches; the buffer itself is reused by each worker.
inline constexpr std::size_t sink_batch_max = 64;

// Bounded multi-producer / multi-consumer queue of fixed-size sink tasks. Only a
// job pointer travels through the queue, so no record ever allocates here. A
// producer blocks once the pending count reaches the configured capacity - the
// documented backpressure contract - and a stopping queue releases every blocked
// producer so shutdown cannot hang behind a full pool.
//
// The ring itself is guarded by a spin lock: a push is a couple of index updates
// and a task copy, so a contending producer must not pay a kernel park and wakeup.
// Real waiting (queue full, or nothing to do) uses the condition variables, which
// is why they are `condition_variable_any` - they accept the spin lock, and only
// the genuinely idle paths ever reach them. A consumer also polls briefly before
// parking, and a push only signals a consumer that actually parked, so a busy
// pipeline stays entirely inside the producer and worker threads.
class task_queue {
public:
    explicit task_queue(std::size_t capacity)
        : capacity_((std::max)(std::size_t{1}, capacity)), buffer_(capacity_) {}
    task_queue(const task_queue&) = delete;
    task_queue& operator=(const task_queue&) = delete;

    // Pushes tasks in chunks bounded by the batch size, so one record's fan-out
    // costs one lock acquisition instead of one per sink. Returns how many were
    // accepted; fewer only when the queue is stopping. A waiting producer is
    // released by stop() and reports every remaining task as rejected.
    std::size_t push_batch(const sink_task* tasks, std::size_t count) {
        std::size_t pushed = 0;
        while (pushed < count) {
            const auto chunk = (std::min)(count - pushed, (std::min)(capacity_, sink_batch_max));
            std::unique_lock<spin_mutex> lock(mutex_);
            ++space_waiters_;
            space_.wait(lock, [&] { return stopped_ || pending_ + chunk <= capacity_; });
            --space_waiters_;
            if (stopped_) return pushed;
            for (std::size_t i = 0; i < chunk; ++i) {
                buffer_[tail_] = tasks[pushed + i];
                tail_ = tail_ + 1 == capacity_ ? 0 : tail_ + 1;
            }
            pending_ += chunk;
            hint_.store(true, std::memory_order_relaxed);
            const bool wake = parked_ != 0;
            lock.unlock();
            pushed += chunk;
            if (wake) data_.notify_all();
        }
        return pushed;
    }
    // Blocks until at least one task is available or the queue stops. Returns 0
    // only once the queue has stopped and drained. The drained tasks are counted
    // as active until the caller reports them with finish_batch().
    std::size_t pop_batch(sink_task* out, std::size_t max) {
        for (unsigned retry = 0; retry < 32; ++retry) {
            if (hint_.load(std::memory_order_relaxed)) {
                std::unique_lock<spin_mutex> lock(mutex_);
                const auto n = drain(out, max);
                const bool wake = n != 0 && space_waiters_ != 0;
                lock.unlock();
                if (wake) space_.notify_all();
                if (n) return n;
            }
            spin_backoff(retry);
        }
        std::unique_lock<spin_mutex> lock(mutex_);
        ++parked_;
        data_.wait(lock, [&] { return pending_ != 0 || stopped_; });
        --parked_;
        const auto n = drain(out, max);
        const bool wake = n != 0 && space_waiters_ != 0;
        lock.unlock();
        if (wake) space_.notify_all();
        return n;
    }
    // Reports tasks handed out by pop_batch() as fully processed.
    void finish_batch(std::size_t count) {
        std::unique_lock<spin_mutex> lock(mutex_);
        active_ -= count;
        if (active_ == 0 && pending_ == 0 && idle_waiters_ != 0) idle_.notify_all();
    }
    // Flush barrier: blocks until no task is queued and none is being processed.
    // A record that is still being submitted counts as concurrent with the
    // barrier, matching the documented flush contract.
    void wait_idle() {
        std::unique_lock<spin_mutex> lock(mutex_);
        ++idle_waiters_;
        idle_.wait(lock, [&] { return pending_ == 0 && active_ == 0; });
        --idle_waiters_;
    }
    void stop() {
        {
            std::lock_guard<spin_mutex> lock(mutex_);
            stopped_ = true;
            hint_.store(false, std::memory_order_relaxed);
        }
        space_.notify_all();
        data_.notify_all();
        idle_.notify_all();
    }
    std::size_t pending() const {
        std::lock_guard<spin_mutex> lock(mutex_);
        return pending_;
    }
    // Tasks queued or being written by a worker; the "dequeued" statistic is
    // derived from it without touching the logging path.
    std::size_t outstanding() const {
        std::lock_guard<spin_mutex> lock(mutex_);
        return pending_ + active_;
    }
private:
    // Requires the lock. Copies up to `max` tasks out, oldest first, and moves
    // them from the pending count to the active count.
    std::size_t drain(sink_task* out, std::size_t max) {
        std::size_t count = 0;
        while (count < max && pending_ != 0) {
            out[count++] = buffer_[head_];
            head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
            --pending_;
        }
        active_ += count;
        // Only this thread pushes the hint back down, and it holds the lock, so
        // no producer can be interleaved between the test and the store.
        if (pending_ == 0) hint_.store(false, std::memory_order_relaxed);
        return count;
    }
    const std::size_t capacity_;
    std::vector<sink_task> buffer_;
    // Producer-written and consumer-polled purely as a heuristic ("work may be
    // available"); relaxed suffices because a consumer that follows the hint
    // takes the lock and re-checks pending_ before touching the ring. Split onto
    // its own line to keep the ring indices and parked counter out of the same
    // coherence traffic.
    alignas(64) std::atomic<bool> hint_{false};
    mutable spin_mutex mutex_;
    std::condition_variable_any data_, space_, idle_;
    std::size_t head_ = 0, tail_ = 0, pending_ = 0, space_waiters_ = 0;
    std::size_t active_ = 0, idle_waiters_ = 0;
    alignas(64) std::size_t parked_ = 0;
    bool stopped_ = false;
};

} // namespace detail

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
          pool_size_(cfg.sink_pool_size), single_threaded_(cfg.single_threaded) {
        pattern_.shrink_to_fit();
        capture_mask_.store(static_cast<unsigned char>(configured_metadata()), std::memory_order_relaxed);
        if (!single_threaded_ && cfg.async.enabled) {
            async_ = std::make_unique<async_state>(cfg.async);
            queue_ = async_->queue.get();
            async_->worker = std::thread([this] { worker_loop(); });
        } else if (!single_threaded_ && cfg.parallel_sinks) {
            // The pool itself starts with the second sink: a single destination
            // has nothing to parallelize, so it is written by the caller until
            // fan-out exists (an explicit sink_pool_size still forces a pool).
            parallel_ = std::make_unique<parallel_state>(cfg.sink_pool_size, cfg.sink_queue_capacity);
        }
        // A queued logger always owns its records; otherwise the caller may
        // format into view sinks until proven wrong (see inline_dispatch()).
        inline_ok_.store(!queue_, std::memory_order_relaxed);
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
        if (!s->uses_views()) {
            views_only_.store(false, std::memory_order_release);
            inline_ok_.store(false, std::memory_order_release);
        }
        required_metadata_ |= static_cast<unsigned char>(s->required_metadata());
        update_metadata();
        auto* previous = sinks_tail_.load(std::memory_order_relaxed);
        auto node = previous ? std::make_unique<sink_node>(std::move(s), previous->count + 1) : nullptr;
        auto* added = node ? node.get() : &first_sink_;
        if (parallel_ && !parallel_->ready.load(std::memory_order_acquire) &&
            (added->count >= 2 || pool_size_ != 0))
            start_parallel(added->count);
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
        wait_parallel_idle();
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
        if (parallel_ && parallel_->tasks) stop_parallel();
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
        // Completed records: everything whose call already returned minus the
        // tasks still queued or being written. Exact once the pool has drained,
        // and derived without adding any per-record work to the logging path.
        if (async_) {
            result.dequeued = async_->dequeued.load(std::memory_order_acquire);
        } else if (parallel_ && parallel_->tasks) {
            const auto outstanding = parallel_->tasks->outstanding();
            const auto returned = static_cast<std::size_t>(counters().returns.load(std::memory_order_acquire));
            result.dequeued = returned > outstanding ? returned - outstanding : 0;
        } else {
            result.dequeued = static_cast<std::size_t>(counters().returns.load(std::memory_order_acquire));
        }
        result.flushed = static_cast<std::size_t>(flushed_.load(std::memory_order_relaxed));
        result.errors = static_cast<std::size_t>(errors_.load(std::memory_order_relaxed));
        if (queue_) result.queue_size = queue_->size_relaxed();
        else if (parallel_ && parallel_->tasks) result.queue_size = parallel_->tasks->pending();
        return result;
    }
    std::size_t queue_capacity() const noexcept { return queue_ ? queue_->capacity() : 0; }

private:
    struct parallel_state; // defined below with the rest of the dispatch state
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
            if constexpr (detail::plain_string_argument<Args...>) {
                if (text == "{}") { submit_view(lv, loc, std::string_view(args)...); return; }
            }
            if constexpr (sizeof...(Args) == 0) {
                if (detail::has_no_braces(text)) {
                    submit_view(lv, loc, text);
                    return;
                }
            }
            if constexpr (detail::bare_field_arguments<Args...>) {
                if (submit_bare(lv, loc, text, args...)) return;
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
            if constexpr (detail::plain_string_argument<Args...>) {
                if (fmt == "{}") { submit_view(lv, loc, std::string_view(args)...); return; }
            }
            if constexpr (sizeof...(Args) == 0) {
                if (detail::has_no_braces(fmt)) { submit_view(lv, loc, fmt); return; }
            }
            if constexpr (detail::bare_field_arguments<Args...>) {
                if (submit_bare(lv, loc, fmt, args...)) return;
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
    // Renders the accepted subset straight into a stack buffer: no owning string
    // and no library formatting call. Returns false when the text is outside the
    // subset, having written nothing and taken no per-record bookkeeping, so the
    // caller can hand the record to the general formatter instead.
    template <class... Args>
    bool submit_bare(level lv, const std::source_location& loc, std::string_view text, const Args&... args) {
        detail::bare_run runs[detail::bare_max_fields + 1];
        const auto fields = detail::plan_bare_fields(text, runs, detail::bare_max_fields + 1);
        if (fields != static_cast<int>(sizeof...(Args))) return false;
        if (!detail::bare_arguments_valid(args...)) return false;
        log_guard active(*this);
        if (!active) return true;
        detail::text_buffer buffer;
        bool rendered = false;
        try {
            detail::emit_bare_fields(buffer, text.data(), runs, args...);
            rendered = true;
        } catch (...) {
            // Same contract as a failing formatter: count the error and deliver
            // the unformatted text.
            error();
        }
        // One dispatch site only: this function is inlined into the hot logging
        // entry point, and the handler must not duplicate that body.
        dispatch_view(lv, loc, rendered ? buffer.view() : text, active.sequence);
        return true;
    }
    // True when the record can go straight from the caller into view sinks:
    // no queue and no running sink pool. A configured-but-idle pool does not
    // block the inline path, so a single-destination logger formats directly
    // into the sink's buffer instead of materializing a payload string. The
    // decision only ever flips to false (a non-view sink appears, or the pool
    // starts), so a relaxed load keeps this branch out of the memory-ordering
    // critical path.
    bool inline_dispatch() const noexcept {
        return inline_ok_.load(std::memory_order_relaxed);
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
        if (capture & sink::timestamp) e.ts = detail::wall_now();
        if (capture & sink::thread_id) e.tid = std::this_thread::get_id();
        if (capture & sink::logger_name) e.name = name_;
        if (capture & sink::source_location) e.loc = loc;
        e.lvl = lv;
        e.payload = message;
        e.seq = single_threaded_ ? increment_local(counters().entries) : sequence;
        if (single_threaded_) {
            write_to(current_sinks(), e);
            increment_local(counters().returns);
        } else if (auto current = current_sinks()) {
            write_to(current, e);
        }
        if (should_flush(lv)) flush_sinks();
    }
    template <class Format>
    void submit(level lv, const std::source_location& loc, Format format, std::string_view fallback) {
        log_guard active(*this);
        if (!active) return;
        log_event e;
        const auto capture = capture_mask_.load(std::memory_order_acquire);
        if (capture & sink::timestamp) e.ts = detail::wall_now();
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
        if (auto* pool = current_parallel(); pool && current && !current.empty()) {
            // Each event owns its strings once; all per-sink tasks share it.
            submit_parallel(*pool, current, lv, std::move(e));
            return;
        }
        if (current) write_to(current, e);
        if (should_flush(lv)) flush_sinks();
    }
    // Publishes one task per sink into the bounded pool ring. The job's remaining
    // count is set before the first task becomes visible, and a task that cannot
    // be accepted (pool stopped) still consumes its reference, so the job is
    // released exactly once.
    void submit_parallel(parallel_state& pool, const sink_snapshot& current, level lv, log_event&& e) {
        auto* job = pool.jobs.acquire();
        job->event = std::move(e);
        job->flush_event = should_flush(lv);
        job->remaining.store(current.size(), std::memory_order_relaxed);
        // Deliberately uninitialized: every entry handed to push_tasks() below
        // has been written, and zero-filling 64 tasks per record is pure work.
        std::array<detail::sink_task, detail::sink_batch_max> batch;
        std::size_t n = 0;
        bool stopped = false;
        for (auto& output : current) {
            if (stopped) {
                finish_job(pool, *job);
                continue;
            }
            batch[n++] = {job, output.get()};
            if (n == batch.size()) {
                stopped = !push_tasks(pool, batch.data(), n);
                n = 0;
            }
        }
        if (n) push_tasks(pool, batch.data(), n);
    }
    // Returns false when the queue was stopping, in which case the rejected
    // tasks have already been accounted for.
    bool push_tasks(parallel_state& pool, const detail::sink_task* tasks, std::size_t count) {
        const auto pushed = pool.tasks->push_batch(tasks, count);
        for (std::size_t i = pushed; i < count; ++i) finish_job(pool, *tasks[i].job);
        return pushed == count;
    }
    void finish_job(parallel_state& pool, detail::parallel_job& job) noexcept {
        if (job.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (job.flush_event) flushed_.fetch_add(1, std::memory_order_relaxed);
            pool.jobs.retire(&job); // Recycle for the next record.
        }
    }
    void start_parallel(std::size_t sinks) {
        const auto threads = (std::max)(std::size_t{1}, parallel_->pool_size ? parallel_->pool_size : sinks);
        parallel_->stop.store(false, std::memory_order_relaxed);
        parallel_->tasks = std::make_unique<detail::task_queue>(parallel_->capacity);
        parallel_->workers.reserve(threads);
        try {
            for (std::size_t i = 0; i < threads; ++i)
                parallel_->workers.emplace_back([this] { parallel_worker(); });
        } catch (...) {
            // Leave the logger fully functional in synchronous mode.
            parallel_->stop.store(true, std::memory_order_release);
            parallel_->tasks->stop();
            for (auto& worker : parallel_->workers) if (worker.joinable()) worker.join();
            parallel_->workers.clear();
            throw;
        }
        inline_ok_.store(false, std::memory_order_relaxed);
        parallel_->ready.store(true, std::memory_order_release);
    }
    void stop_parallel() noexcept {
        parallel_->stop.store(true, std::memory_order_release);
        parallel_->tasks->stop();
        for (auto& worker : parallel_->workers) if (worker.joinable()) worker.join();
        parallel_->workers.clear();
    }
    // Waits for every record accepted before the call: the queue must be empty
    // and no worker may still hold a drained batch. Producers running
    // concurrently are not blocked, so a record submitted during the barrier
    // may still be in flight; sink implementations shared with logging threads
    // must be thread-safe, which add_sink() already arranges for built-in
    // sinks.
    void wait_parallel_idle() {
        if (current_parallel()) parallel_->tasks->wait_idle();
    }
    void parallel_worker() {
        auto& pool = *parallel_;
        std::array<detail::sink_task, detail::sink_batch_max> batch{};
        for (;;) {
            const auto n = pool.tasks->pop_batch(batch.data(), batch.size());
            if (n == 0) {
                if (pool.stop.load(std::memory_order_acquire)) break;
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const auto& task = batch[i];
                write_one(*task.output, task.job->event);
                if (task.job->flush_event) flush_one(*task.output);
                finish_job(pool, *task.job);
            }
            pool.tasks->finish_batch(n);
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
        parallel_state(std::size_t threads, std::size_t pending)
            : pool_size(threads), capacity((std::max)(std::size_t{2}, pending)) {}
        const std::size_t pool_size, capacity; // pool_size 0 => one worker per sink

        std::unique_ptr<detail::task_queue> tasks;
        std::vector<std::thread> workers;
        detail::job_pool jobs; // also the flush barrier and the dequeued count
        std::atomic<bool> ready{false}, stop{false};
    };
    log_counters& counters() const noexcept {
        return async_ ? async_->counters : inline_counters_;
    }
    parallel_state* current_parallel() const noexcept {
        return parallel_ && parallel_->ready.load(std::memory_order_acquire) ? parallel_.get() : nullptr;
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
    const std::size_t pool_size_; // 0 => pool size follows the sink count
    const bool single_threaded_;
    std::atomic<bool> views_only_{true};
    std::atomic<bool> inline_ok_{true};
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
