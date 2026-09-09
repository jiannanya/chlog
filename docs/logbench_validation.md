# Build and regression validation

Source SHA-256: `990c0588d38a5962e8edde41d2db146af026c39dcac614fbff5f4c3fa48212ad`

| Build | Configuration | Result |
|---|---|---|
| `build-comparison` | Clang 17 / fmt / Release | 9/9 passed |
| `build-release` | Clang 17 / std::format / Release | 9/9 passed |
| `build-fmt` | Clang 17 / fmt / Release | 9/9 passed |
| `build-msvc` | MSVC 19.38 / std::format / Debug | 9/9 passed |
| `build-linux` | GCC 13 / std::format / Release | 9/9 passed |
| `build-linux-asan` | Clang 18 / ASan + UBSan | 9/9 passed |
| `build-tsan` | Clang 18 / TSan | 9/9 passed |

All seven install-and-consume tests passed, and every installed header matches the source. No compiler warnings were reported. TSan also passed 20 repetitions each of formatting, queue, async, parallel, concurrent and registration suites (120 suite runs).

Coverage includes unique event sequences, concurrent shutdown in every threaded mode, bounded queue wakeups, partial sink registration, single-thread sink chains, event-view ownership, legacy sink interoperability, runtime formatting errors, file rotation and package consumers.

Commands:

```powershell
cmake --build build-comparison -j4
ctest --test-dir build-comparison --output-on-failure -j4
python tools/logbench_report.py --build-dir build-comparison --out docs/logbench_results.md --iters 2000000 --repeats 9 --affinity 0x5555
python tools/logbench_memory.py --build-dir build-comparison --out docs/logbench_memory.md --repeats 5
python tools/logbench_plot.py --in docs/logbench_results.json --memory docs/logbench_memory.json --out docs/logbench_summary.svg
```

The affinity mask is specific to the measured host; use a supported mask or omit it elsewhere. Performance results are counter-sink API measurements with the stated configuration, not measurements of arbitrary disk or network sinks.

[Throughput](logbench_results.md) | [Memory](logbench_memory.md)
