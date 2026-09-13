# chlog vs spdlog benchmark report

- Executable: `E:\cc\AI\tokmon\chlog\build-comparison\chlog_bench_loggers.exe`
- Host: `Windows-10-10.0.26200-SP0`
- Python: `3.10.7`
- Iterations: `2000000`

- Measured: `2026-09-13T21:29:55+08:00`
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
| async_4p | 8.125e+06 | 1.680e+06 |
| async_mt | 5.592e+06 | 3.346e+06 |
| async_mt_literal | 6.244e+06 | 5.030e+06 |
| async_mt_payload1024 | 3.892e+06 | 2.224e+06 |
| async_mt_payload128 | 4.118e+06 | 2.617e+06 |
| filtered_out | 1.097e+09 | 4.860e+08 |
| sync_mt | 2.613e+07 | 2.145e+07 |
| sync_mt_literal | 4.890e+07 | 3.587e+07 |
| sync_mt_payload1024 | 5.472e+07 | 1.337e+07 |
| sync_mt_payload128 | 5.550e+07 | 2.481e+07 |
| sync_st | 3.202e+07 | 2.154e+07 |
| sync_st_literal | 7.100e+07 | 3.554e+07 |
| sync_st_payload1024 | 8.774e+07 | 1.345e+07 |
| sync_st_payload128 | 8.813e+07 | 2.488e+07 |

## Details

### async_4p

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.246154 | 8.125e+06 | 2000000 | 0 |
| spdlog | 2000000 | 1.190800 | 1.680e+06 | 2000000 | 0 |

### async_mt

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.357637 | 5.592e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.597700 | 3.346e+06 | 2000000 | 0 |

### async_mt_literal

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.320295 | 6.244e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.397640 | 5.030e+06 | 2000000 | 0 |

### async_mt_payload1024

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.513891 | 3.892e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.899371 | 2.224e+06 | 2000000 | 0 |

### async_mt_payload128

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.485626 | 4.118e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.764273 | 2.617e+06 | 2000000 | 0 |

### filtered_out

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.001823 | 1.097e+09 | 0 | 0 |
| spdlog | 2000000 | 0.004115 | 4.860e+08 | 0 | 0 |

### sync_mt

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.076528 | 2.613e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.093227 | 2.145e+07 | 2000000 | 0 |

### sync_mt_literal

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.040901 | 4.890e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.055760 | 3.587e+07 | 2000000 | 0 |

### sync_mt_payload1024

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.036547 | 5.472e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.149640 | 1.337e+07 | 2000000 | 0 |

### sync_mt_payload128

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.036035 | 5.550e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.080607 | 2.481e+07 | 2000000 | 0 |

### sync_st

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.062456 | 3.202e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.092848 | 2.154e+07 | 2000000 | 0 |

### sync_st_literal

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.028168 | 7.100e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.056276 | 3.554e+07 | 2000000 | 0 |

### sync_st_payload1024

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.022795 | 8.774e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.148688 | 1.345e+07 | 2000000 | 0 |

### sync_st_payload128

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.022694 | 8.813e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.080370 | 2.488e+07 | 2000000 | 0 |

