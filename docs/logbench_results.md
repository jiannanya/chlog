# chlog vs spdlog benchmark report

- Executable: `D:\CS\src3\chlog\build-ninja-clang\chlog_bench_loggers.exe`
- Host: `Windows-10-10.0.26100-SP0`
- Python: `3.10.7`
- Iterations: `2000000`

## System

- CPU: `12th Gen Intel(R) Core(TM) i9-12900K`
- CPU cores (logical): `24`
- Memory (total): `31.75 GiB`

## Library versions

- chlog: `0.1.0`
- spdlog: `1.15.3` (vcpkg)
- fmt: `11.0.2#1` (vcpkg)

## Summary (calls/s, higher is better)

| Case | chlog | spdlog |
|---|---:|---:|
| async_mt | 5.026e+06 | 4.130e+06 |
| filtered_out | 4.510e+09 | 4.395e+08 |
| sync_mt | 1.737e+07 | 1.695e+07 |
| sync_st | 2.836e+07 | 2.288e+07 |

## Details

### async_mt

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.397941 | 5.026e+06 | 2000000 | 0 |
| spdlog | 2000000 | 0.484228 | 4.130e+06 | 2000000 | 0 |

### filtered_out

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.000443 | 4.510e+09 | 0 | 0 |
| spdlog | 2000000 | 0.004550 | 4.395e+08 | 0 | 0 |

### sync_mt

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.115142 | 1.737e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.117963 | 1.695e+07 | 2000000 | 0 |

### sync_st

| Runner | calls | seconds | calls/s | processed | dropped |
|---|---:|---:|---:|---:|---:|
| chlog | 2000000 | 0.070527 | 2.836e+07 | 2000000 | 0 |
| spdlog | 2000000 | 0.087405 | 2.288e+07 | 2000000 | 0 |

