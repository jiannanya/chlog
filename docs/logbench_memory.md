# chlog vs spdlog memory report

Measured: 2026-09-10T00:05:42+08:00; 5 runs per case.

Host: Windows-10-10.0.26200-SP0; CPU: 12th Gen Intel(R) Core(TM) i9-12900K.

Each library, payload and case runs in a fresh process. Both use the same fmt backend, a borrowed-event counter sink, and one consumer with a 65,536-slot FIFO queue and blocking overflow. Synchronous cases run 10,000 calls; async streaming also runs 10,000 calls. Backlog cases block the first sink callback, then fill all 65,536 queue slots before releasing the consumer. Every record must be processed.

## Process memory

These measurements use the original CRT allocator. Peak private committed bytes and peak working set come from GetProcessMemoryInfo, and include the executable, runtime, thread stacks, allocator overhead and malloc allocations made by fmt. They are absolute process totals, not logger-only heap sizes. Small differences near the runtime baseline are noise; streaming peaks also depend on scheduling.

| Case | Payload (B) | chlog peak private (MiB) | spdlog peak private (MiB) | chlog peak working set (MiB) | spdlog peak working set (MiB) |
|---|---:|---:|---:|---:|---:|
| sync_st | 13 | 0.902 | 0.906 | 4.738 | 4.746 |
| sync_st | 128 | 0.906 | 0.902 | 4.742 | 4.746 |
| sync_st | 1024 | 0.922 | 0.922 | 4.754 | 4.766 |
| sync_mt | 13 | 0.898 | 0.902 | 4.734 | 4.742 |
| sync_mt | 128 | 0.906 | 0.902 | 4.734 | 4.746 |
| sync_mt | 1024 | 0.918 | 0.926 | 4.746 | 4.766 |
| async | 13 | 6.992 | 26.512 | 10.902 | 30.387 |
| async | 128 | 7.918 | 26.508 | 11.730 | 30.391 |
| async | 1024 | 8.859 | 28.102 | 12.598 | 31.930 |
| backlog | 13 | 6.996 | 26.516 | 10.910 | 30.398 |
| backlog | 128 | 17.379 | 26.512 | 20.953 | 30.398 |
| backlog | 1024 | 78.379 | 93.949 | 80.070 | 95.734 |

## Many synchronous instances

Each instance owns its own counter sink. The common harness retains a vector of shared pointers for both libraries, and each logger processes one 13 B record. This also exposes allocator and process-memory effects that are too small to resolve with one instance.

| Mode | Instances | chlog peak private (MiB) | spdlog peak private (MiB) | chlog resident new bytes | spdlog resident new bytes |
|---|---:|---:|---:|---:|---:|
| objects_st | 1,000 | 1.250 | 1.363 | 304,039 | 368,039 |
| objects_st | 10,000 | 4.227 | 5.070 | 3,040,039 | 3,680,039 |
| objects_mt | 1,000 | 1.238 | 1.355 | 304,039 | 368,039 |
| objects_mt | 10,000 | 4.219 | 5.074 | 3,040,039 | 3,680,039 |

## C++ new diagnostics

A separate instrumented executable tracks requested C++ new bytes; it is not used for the process memory table above. It excludes malloc/free and fmt's dynamic memory buffers, so zero allocations here does not establish zero total heap allocation. Resident bytes include logger, sink, queue and owned configuration immediately before logging. The workload column counts C++ allocations during logging and shutdown. All tracked bytes must be released after destruction.

| Case | Payload (B) | chlog resident new bytes | spdlog resident new bytes | chlog construction peak (B) | spdlog construction peak (B) | chlog workload new calls | spdlog workload new calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| sync_st | 13 | 288 | 352 | 288 | 352 | 0 | 0 |
| sync_st | 128 | 288 | 352 | 288 | 352 | 0 | 0 |
| sync_st | 1024 | 288 | 352 | 288 | 352 | 0 | 0 |
| sync_mt | 13 | 288 | 352 | 288 | 352 | 0 | 0 |
| sync_mt | 128 | 288 | 352 | 288 | 352 | 0 | 0 |
| sync_mt | 1024 | 288 | 352 | 288 | 352 | 0 | 0 |
| async | 13 | 6,315,087 | 26,740,007 | 6,315,087 | 26,740,007 | 0 | 0 |
| async | 128 | 6,315,087 | 26,740,007 | 6,315,087 | 26,740,007 | 10,000 | 0 |
| async | 1024 | 6,315,087 | 26,740,007 | 6,315,087 | 26,740,007 | 10,000 | 0 |
| backlog | 13 | 6,315,087 | 26,740,007 | 6,315,087 | 26,740,007 | 0 | 0 |
| backlog | 128 | 6,315,087 | 26,740,007 | 6,315,087 | 26,740,007 | 65,537 | 0 |
| backlog | 1024 | 6,315,087 | 26,740,007 | 6,315,087 | 26,740,007 | 65,537 | 0 |

The synchronous logger plus one counter sink retains 288 B for chlog and 352 B for spdlog. Construction peaks are 288 B and 352 B respectively. `sizeof(logger)` is 184 B for chlog and 280 B for spdlog.

These measurements cover the stated configuration. Existing owning-event chlog sinks can allocate when copying long messages; view_sink avoids that copy for inline dispatch. Disk I/O, arbitrary user sinks and every platform are outside this comparison.

Raw runs and source hashes: [logbench_memory.json](logbench_memory.json).
