# chlog vs spdlog benchmark report

- Executable: `E:\cc\AI\tokmon\chlog\build-comparison\chlog_bench_loggers.exe`
- Host: `Windows-10-10.0.26200-SP0`
- Python: `3.10.7`
- Iterations: `2000000`

- Measured: `2026-09-10T00:08:05+08:00`
- Runs: `9`; medians; library order alternates
- Compiler: `clang-17.0.6`; Release flags: `-O3 -DNDEBUG`
- CPU affinity: `0x5555`
- Raw measurements: [logbench_results.json](logbench_results.json)

## System

- CPU: `12th Gen Intel(R) Core(TM) i9-12900K`
- CPU cores (logical): `24`
- Memory (total): `31.75 GiB`

## Library versions

- chlog: `1.0.0`
- spdlog: `1.15.3` (reported by the benchmark executable)
- fmt: `11.1.4` (reported by the benchmark executable)

## Workload

Both libraries use the same fmt backend and a sink that performs one relaxed atomic increment per event. There is no sink-level formatting, extra sink mutex, console output, or disk I/O. Formatted cases log `v {}` with an integer; literal cases log `message ready`. Cases ending in `_payload128` or `_payload1024` format a prebuilt string of that byte length with `{}`. chlog uses `view_sink`; spdlog's sink receives its native borrowed log message. Existing chlog sinks using owning `log_event` remain supported and may require string copies.

Async cases use one worker, a 65,536-slot FIFO queue, blocking overflow, and include final draining and worker shutdown. chlog uses `weighted_queue=false`. All recorded calls are verified as processed with zero drops; filtered calls correctly process zero events.

The sync_st and sync_mt cases each have one producer; chlog selects its corresponding logger mode. The spdlog logger and atomic counter sink are the same in both. Library metadata policies remain native: chlog's `{msg}` mode omits unused fields, while spdlog still captures its normal event metadata. These are logging API and dispatch measurements, not end-to-end file throughput. Filter-only runs are very short and sensitive to timing noise.

## Summary (calls/s, higher is better)

| Case | chlog | spdlog |
|---|---:|---:|
| async_4p | 8.105e+06 | 1.863e+06 |
| async_mt | 5.252e+06 | 3.101e+06 |
| async_mt_literal | 6.381e+06 | 5.443e+06 |
| async_mt_payload1024 | 3.136e+06 | 2.365e+06 |
| async_mt_payload128 | 4.095e+06 | 3.373e+06 |
| filtered_out | 1.086e+09 | 4.741e+08 |
| sync_mt | 2.572e+07 | 2.146e+07 |
| sync_mt_literal | 4.780e+07 | 3.555e+07 |
| sync_mt_payload1024 | 1.628e+07 | 1.260e+07 |
| sync_mt_payload128 | 3.425e+07 | 2.433e+07 |
| sync_st | 3.212e+07 | 2.154e+07 |
| sync_st_literal | 6.999e+07 | 3.581e+07 |
| sync_st_payload1024 | 1.854e+07 | 1.320e+07 |
| sync_st_payload128 | 4.508e+07 | 2.430e+07 |

## Details

### async_4p

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.246770 | 8.105e+06 | 2000000 | 0 |
| spdlog | 2000000 | 1.073390 | 1.863e+06 | 2000000 | 0 |

### async_mt

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.380818 | 5.252e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.644903 | 3.101e+06 | 2000000 | 0 |

### async_mt_literal

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.313454 | 6.381e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.367448 | 5.443e+06 | 2000000 | 0 |

### async_mt_payload1024

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.637673 | 3.136e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.845729 | 2.365e+06 | 2000000 | 0 |

### async_mt_payload128

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.488435 | 4.095e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.593011 | 3.373e+06 | 2000000 | 0 |

### filtered_out

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.001841 | 1.086e+09 | 0 | 0 |
| spdlog | 2000000 | 0.004219 | 4.741e+08 | 0 | 0 |

### sync_mt

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.077766 | 2.572e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.093176 | 2.146e+07 | 2000000 | 0 |

### sync_mt_literal

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.041841 | 4.780e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.056263 | 3.555e+07 | 2000000 | 0 |

### sync_mt_payload1024

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.122873 | 1.628e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.158669 | 1.260e+07 | 2000000 | 0 |

### sync_mt_payload128

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.058397 | 3.425e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.082214 | 2.433e+07 | 2000000 | 0 |

### sync_st

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.062259 | 3.212e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.092871 | 2.154e+07 | 2000000 | 0 |

### sync_st_literal

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.028574 | 6.999e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.055845 | 3.581e+07 | 2000000 | 0 |

### sync_st_payload1024

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.107847 | 1.854e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.151500 | 1.320e+07 | 2000000 | 0 |

### sync_st_payload128

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.044361 | 4.508e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.082316 | 2.430e+07 | 2000000 | 0 |

