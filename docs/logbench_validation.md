# Build and regression validation

Source SHA-256: `8831b4d7c5768d13c70cb81b8796fa48964d58da2c58a802b1f1c9f251615545`

| Build | Configuration | Result |
|---|---|---|
| `build-comparison` | Clang 17 / fmt / Release | 9/9 passed |
| `build-release` | Clang 17 / std::format / Release | 9/9 passed |
| `build-fmt` | Clang 17 / fmt / Release | 9/9 passed |
| `build-msvc` | MSVC 19.38 / std::format / Debug | 9/9 passed |
| `build-linux` | GCC 13 / std::format / Release | 9/9 passed |
| `build-linux-asan` | Clang 18 / ASan + UBSan | 9/9 passed |
| `build-tsan` | Clang 18 / TSan | 9/9 passed |
| `build-no-rtti` | GCC 13 / std::format / Release / `-fno-rtti` | 9/9 passed |

All eight configurations passed 72 tests in total, including install-and-consume checks, and every installed header matches the source. No compiler warnings were reported in the final builds. TSan also passed 20 repetitions each of formatting, files, queue, async, parallel, concurrent and registration suites (140 suite runs).

Coverage includes unique event sequences, concurrent shutdown in every threaded mode, bounded queue wakeups, partial sink registration, single-thread sink chains, event-view ownership, legacy sink interoperability, runtime formatting errors, file rotation and package consumers. Rendering tests cover all 256 byte values at 24 offsets, short tails, embedded NUL, buffer boundaries through 65,536 bytes, console colors, and concurrent pattern replacement through both rendering APIs. String formatting checks preserve rvalue inputs and retain width, precision, escaping and custom formatter behavior. File tests include concurrent long records, UTF-8 paths, reading an active writer, append semantics and failed writes/flushes on Linux `/dev/full`.

The allocation-instrumented renderer checks 128-byte and 1,024-byte payloads with message, full timestamp and JSON patterns. The local output buffer uses zero C++ heap allocations for each 128-byte case, and one allocation for each 1,024-byte case, including the final newline. These are rendering-buffer measurements; logger and file-runtime allocations are outside that claim.

Additional coverage exercises contended compact-mutex handoffs, borrowed records through all four logger modes, legacy callbacks in subclasses of every built-in sink, and the owning-event fallback without RTTI. File-buffer checks cover empty writes, 4 KiB boundaries, 128 KiB direct writes, embedded NUL, destructor draining and reopening earlier daily files. Calendar-token folding is checked against timestamp rendering with repeated tokens, unknown suffixes and pre-epoch timestamps.

The reports recorded on 2026-09-13 use this same source header. All 14 counter-sink throughput medians favor chlog (1.22–6.52× spdlog), as do all 12 file-output medians (1.10–1.94×). The memory report contains 360 fresh-process runs and 72 medians. Synchronous instance allocations, multiple-instance process memory, file-instance process memory and all measured async/backlog process peaks are lower for chlog. Single synchronous process totals remain near the runtime baseline: some peak-private measurements tie or differ by a few pages in spdlog's favor. These results establish the stated workloads on this host, not superiority across every memory metric, workload or platform.

The summary SVG was regenerated and rendered for visual inspection. All 28 throughput values and six process-memory values in the SVG match the raw reports.

Commands:

```powershell
cmake --build build-comparison -j4
ctest --test-dir build-comparison --output-on-failure -j4
python tools/logbench_report.py --build-dir build-comparison --out docs/logbench_results.md --iters 2000000 --repeats 9 --affinity 0x5555
python tools/logbench_output.py --build-dir build-comparison --out docs/logbench_output.md --iters 100000 --short-iters 500000 --repeats 7 --affinity 0x5555
python tools/logbench_memory.py --build-dir build-comparison --out docs/logbench_memory.md --repeats 5
python tools/logbench_plot.py --in docs/logbench_results.json --memory docs/logbench_memory.json --out docs/logbench_summary.svg
```

The affinity mask is specific to the measured host; use a supported mask or omit it elsewhere. Counter-sink results measure API throughput. The separate file-output report measures matched built-in rotating-file sinks with normal filesystem caching and flushed library buffers; it does not measure durable-storage latency or arbitrary network sinks. Each library and file case receives a separate 10,000-record warmup before measurement. The memory report includes simultaneously open file instances and their live write buffers.

[Throughput](logbench_results.md) | [File output](logbench_output.md) | [Memory](logbench_memory.md)
