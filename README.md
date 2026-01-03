# chlog

`chlog` is a lightweight, modern, **header-only** C++20 logging library: simple at the call site, configurable for throughput (optional async), and extensible via sinks.

- **C++20**: uses `std::format` and `std::source_location`
- **Platforms**: Windows / Linux / macOS (`localtime_s` on Windows, `localtime_r` elsewhere)

> 中文一句话：`chlog` 主打“高性能、调用端轻、吞吐高、可选异步、可插拔 sink、 线程安全可选”，并且尽量把开销留在需要的时候。

## Highlights

- **Sync / async**: toggle via `logger_config::async.enabled`
- **Fast async wakeups**: bounded MPSC ring buffer + `std::counting_semaphore`
- **Single-threaded ultra-fast mode**: `logger_config::single_threaded` (not thread-safe; forces async + parallel_sinks off)
- **Bounded queue + priority dropping**: prefers dropping `trace/debug/info` while keeping `warn+`
- **Optional {fmt} backend**: define `CHLOG_USE_FMT` to use `{fmt}` for formatting (default: `std::format`)
- **Built-in sinks**: `console_sink`, `rotating_file_sink`, `daily_file_sink`, `json_sink`
- **Optional parallel sinks**: `logger_config::parallel_sinks` (sync mode only)
- **Call-site info**: `std::source_location` → `{file}` `{line}` `{func}` (pattern / JSON)
- **Robustness**: formatting failures / sink exceptions are swallowed (logging should not crash your app)

## Contents

- [chlog](#chlog)
  - [Highlights](#highlights)
  - [Contents](#contents)
  - [Latest results](#latest-results)
  - [Quick Start](#quick-start)
  - [Feature spotlight: message-only mode](#feature-spotlight-message-only-mode)
    - [Example: high-throughput message-only async file logger](#example-high-throughput-message-only-async-file-logger)
  - [Single-threaded mode (`logger_config::single_threaded`)](#single-threaded-mode-logger_configsingle_threaded)
  - [Parallel sinks (`logger_config::parallel_sinks`)](#parallel-sinks-logger_configparallel_sinks)
    - [Pattern](#pattern)
    - [Macros (Optional)](#macros-optional)
  - [Build with CMake](#build-with-cmake)
  - [Install via vcpkg](#install-via-vcpkg)
  - [Benchmarks (chlog vs spdlog)](#benchmarks-chlog-vs-spdlog)
    - [Dependencies (via vcpkg)](#dependencies-via-vcpkg)
    - [Configure with vcpkg + clang](#configure-with-vcpkg--clang)
    - [Run](#run)
    - [Generate Markdown report](#generate-markdown-report)
    - [Regenerate report + chart](#regenerate-report--chart)

## Latest results

![chlog vs spdlog benchmark chart](docs/logbench_summary.svg)

| Case | chlog | spdlog |
|---|---:|---:|
| async_mt | 5.026e+06 | 4.130e+06 |
| filtered_out | 4.510e+09 | 4.395e+08 |
| sync_mt | 1.737e+07 | 1.695e+07 |
| sync_st | 2.836e+07 | 2.288e+07 |

Full details (including per-case tables, CPU/memory, and library versions) are in [docs/logbench_results.md](docs/logbench_results.md).

## Quick Start

```cpp
#include <chlog/chlog.hpp>

int main() {
    using namespace chlog;

    logger_config cfg;
    cfg.name = "app";
    cfg.level = level::debug;
    cfg.pattern = "[{date} {time}.{ms}][{lvl}][{name}][{file}:{line}] {msg}";

    cfg.async.enabled = true;
    cfg.async.queue_capacity = 1u << 16;
    cfg.async.batch_max = 256;
    cfg.async.flush_every = std::chrono::milliseconds(200);

    auto lg = std::make_shared<logger>(cfg);
    lg->add_sink(std::make_shared<console_sink>(console_sink::style::plain));
    lg->add_sink(std::make_shared<rotating_file_sink>("logs/app.log", 32 * 1024 * 1024, 5));

    lg->info("hello {}", 123);
    lg->warn("disk {}%", 95);

    lg->shutdown();
}
```

## Feature spotlight: message-only mode

If you only need the formatted message (no timestamp / thread id / logger name / source location), set:

- `cfg.pattern = "{msg}";`

In this mode, `chlog` automatically disables metadata capture (`capture_timestamp`, `capture_thread_id`, `capture_logger_name`, `capture_source_location`) to minimize per-call overhead. This is especially useful for hot loops (e.g., game loops, trading strategies, telemetry) where you want high throughput and you don’t need rich metadata.

### Example: high-throughput message-only async file logger

```cpp
#include <chlog/chlog.hpp>

#include <chrono>
#include <memory>

int main() {
    using namespace chlog;

    logger_config cfg;
    cfg.name = "fast";
    cfg.level = level::info;

    // Feature: message-only mode disables metadata capture automatically.
    cfg.pattern = "{msg}";

    cfg.async.enabled = true;
    cfg.async.queue_capacity = 1u << 16;
    cfg.async.batch_max = 512;
    cfg.async.flush_every = std::chrono::milliseconds(200);

    auto lg = std::make_shared<logger>(cfg);
    lg->add_sink(std::make_shared<rotating_file_sink>("logs/fast.log", 8 * 1024 * 1024, 3));

    for (int i = 0; i < 100000; ++i) {
        lg->info("tick {}", i);
    }

    lg->shutdown();
}
```

Notes:

- If you need any metadata, use a pattern token (e.g. `[{lvl}] {msg}` or `[{date} {time}.{ms}] {msg}`) and keep the corresponding `capture_*` flags enabled.
- If you want structured output, prefer `{json}` (see “Pattern”).

## Single-threaded mode (`logger_config::single_threaded`)

If your application logs from exactly one thread (typical in some game loops, embedded, or single-threaded tools), you can enable:

- `cfg.single_threaded = true;`

Behavior:

- No thread-safety guarantees: calling the same logger/sinks from multiple threads is undefined.
- For maximum throughput, chlog forces `async.enabled = false` and `parallel_sinks = false` in this mode (no internal worker threads; no cross-thread sink writes).
- Built-in sinks also skip their internal mutexes when single-threaded.

## Parallel sinks (`logger_config::parallel_sinks`)

`parallel_sinks` controls whether **sync-mode** logging fans out a single log event to multiple sinks in parallel.

- When `cfg.async.enabled == false` and `cfg.parallel_sinks == true`, `logger::add_sink()` lazily creates an internal thread pool and each log event will enqueue one task per sink.
- When `cfg.async.enabled == true`, chlog intentionally keeps sink writes on the **single async worker thread** for best throughput and lower overhead; `parallel_sinks` does not change async behavior.

`logger_config::sink_pool_size` controls the number of worker threads used for parallel sinks:

- `0` (default): uses `sinks.size()` at the time the pool is created.
- `> 0`: uses that fixed size.

Trade-offs (important):

- **Ordering**: with `parallel_sinks` enabled, strict ordering across sinks (and even within the same sink under contention) is not guaranteed.
- **Flush semantics**: in sync mode with `parallel_sinks`, `flush_on_level` and `logger::flush()` are best-effort because sink writes are happening on background pool threads.

Recommendation:

- Use `parallel_sinks = true` if you have multiple slow sinks (e.g. file + network) and you prefer higher throughput over strict ordering/flush guarantees.
- Use `parallel_sinks = false` if you require strict ordering and synchronous flush behavior.

### Pattern

Default pattern:

- `[{date} {time}.{ms}][{lvl}][tid={tid}][{name}] {msg}`

Available tokens:

- `{ts}` `{date}` `{time}` `{ms}` `{lvl}` `{tid}` `{name}` `{msg}` `{file}` `{line}` `{func}`

Special pattern:

- `{json}`: outputs a single-line JSON record (includes file/line/func).

### Macros (Optional)

If you want to force capturing call-site info without changing your function signatures, you can use:

- `CHLOG_INFO(*lg, "msg {}", x)`
- `CHLOG_ERROR(*lg, "oops {}", err)`
- etc.

## Build with CMake

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Install via vcpkg

Once the `chlog` port is available in your vcpkg registry (or you use this repo's overlay port under `ports/chlog`), install with:

```powershell
vcpkg install chlog
```

Consume from CMake:

```cmake
find_package(chlog CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE chlog::chlog)
```

Example (enabled by default):

- `chlog_stress` (built from `examples/chlog_stress.cpp`)
- `chlog_single_thread_bench` (built from `examples/single_thread_bench.cpp`)

## Benchmarks (chlog vs spdlog)

This repo includes a simple benchmark executable that compares **chlog** vs **spdlog** in a tight loop,
using an in-memory counting sink (no I/O) to focus on call-site + formatting + dispatch overhead.

Note: for a fairer comparison, the benchmark enables `CHLOG_USE_FMT` for chlog when spdlog is available,
so both libraries use the same formatting backend.

Benchmark executable:

- `chlog_bench_loggers`

### Dependencies (via vcpkg)

Install spdlog:

```powershell
vcpkg install spdlog
```

### Configure with vcpkg + clang

Example using Ninja + clang on Windows:

```powershell
$env:VCPKG_ROOT = "C:\\path\\to\\vcpkg"
cmake -S . -B build-ninja-clang -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\\scripts\\buildsystems\\vcpkg.cmake" `
    -DCMAKE_CXX_COMPILER=clang

cmake --build build-ninja-clang
```

### Run

Iterations can be set via `--iters` or `CHLOG_BENCH_ITERS`:

```powershell
$env:CHLOG_BENCH_ITERS = "1000000"
./build-ninja-clang/chlog_bench_loggers

./build-ninja-clang/chlog_bench_loggers --iters 2000000
```

The program prints machine-parsable lines:

- `RESULT runner=... case=... calls=... seconds=... cps=... processed=... dropped=...`

### Generate Markdown report

```powershell
python ./tools/logbench_report.py --build-dir build-ninja-clang --out docs/logbench_results.md --iters 1000000
```

The report includes:

- CPU + total memory info
- vcpkg versions for `spdlog` (and `fmt` if present)

Note:

- results may differ between platforms

### Regenerate report + chart

```powershell
python ./tools/logbench_report.py --build-dir build-ninja-clang --out docs/logbench_results.md --iters 2000000
python ./tools/logbench_plot.py --in docs/logbench_results.md --out docs/logbench_summary.svg
```
  