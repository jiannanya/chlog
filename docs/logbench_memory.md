# chlog vs spdlog memory report

Measured: 2026-09-13T21:31:21+08:00; 5 runs per case.

Host: Windows-10-10.0.26200-SP0; CPU: 12th Gen Intel(R) Core(TM) i9-12900K.

Each library, payload and case runs in a fresh process. Both use the same fmt backend, a borrowed-event counter sink, and one consumer with a 65,536-slot FIFO queue and blocking overflow. Counter-sink synchronous cases run 10,000 calls; async streaming also runs 10,000 calls. Backlog cases block the first sink callback, then fill all 65,536 queue slots before releasing the consumer. Every record must be processed.

## Process memory

These measurements use the original CRT allocator. Peak private committed bytes and peak working set come from GetProcessMemoryInfo, and include the executable, runtime, thread stacks, allocator overhead and malloc allocations made by fmt. They are absolute process totals, not logger-only heap sizes. Small differences near the runtime baseline are noise; streaming peaks also depend on scheduling.

| Case | Payload (B) | chlog peak private (MiB) | spdlog peak private (MiB) | chlog peak working set (MiB) | spdlog peak working set (MiB) |
|---|---:|---:|---:|---:|---:|
| sync_st | 13 | 0.906 | 0.906 | 4.699 | 4.723 |
| sync_st | 128 | 0.906 | 0.898 | 4.699 | 4.719 |
| sync_st | 1024 | 0.898 | 0.922 | 4.699 | 4.734 |
| sync_mt | 13 | 0.898 | 0.895 | 4.699 | 4.715 |
| sync_mt | 128 | 0.902 | 0.910 | 4.699 | 4.723 |
| sync_mt | 1024 | 0.906 | 0.918 | 4.703 | 4.734 |
| async | 13 | 6.996 | 26.512 | 10.867 | 30.359 |
| async | 128 | 8.125 | 26.512 | 11.922 | 30.359 |
| async | 1024 | 9.254 | 28.094 | 13.035 | 31.891 |
| backlog | 13 | 6.992 | 26.516 | 10.871 | 30.367 |
| backlog | 128 | 17.371 | 26.512 | 20.914 | 30.367 |
| backlog | 1024 | 78.398 | 93.945 | 80.023 | 95.703 |

## Many synchronous instances

Each instance owns its own counter sink. The common harness retains a vector of shared pointers for both libraries, and each logger processes one 13 B record. This also exposes allocator and process-memory effects that are too small to resolve with one instance.

| Mode | Instances | chlog peak private (MiB) | spdlog peak private (MiB) | chlog resident new bytes | spdlog resident new bytes |
|---|---:|---:|---:|---:|---:|
| objects_st | 1,000 | 1.250 | 1.359 | 304,039 | 368,039 |
| objects_st | 10,000 | 4.219 | 5.070 | 3,040,039 | 3,680,039 |
| objects_mt | 1,000 | 1.242 | 1.363 | 304,039 | 368,039 |
| objects_mt | 10,000 | 4.238 | 5.078 | 3,040,039 | 3,680,039 |

## File instances

Each logger owns a distinct built-in rotating-file sink with a message-only pattern. All files are open simultaneously and each receives one 13 B record. Snapshots include live file buffers before and after flushing. Both libraries use LF newlines, and every file is checked after destruction. These process totals include C++ allocations and CRT malloc buffers.

| Mode | Instances | chlog peak private (MiB) | spdlog peak private (MiB) | chlog peak working set (MiB) | spdlog peak working set (MiB) |
|---|---:|---:|---:|---:|---:|
| file_st | 32 | 1.176 | 1.250 | 5.082 | 5.332 |
| file_st | 128 | 1.664 | 1.785 | 5.543 | 5.863 |
| file_mt | 32 | 1.184 | 1.254 | 5.086 | 5.340 |
| file_mt | 128 | 1.664 | 1.793 | 5.547 | 5.875 |

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

These measurements cover the stated configuration. Existing owning-event chlog sinks can allocate when copying long messages; view_sink avoids that copy for inline dispatch. Arbitrary user sinks and every platform are outside this comparison.

Raw runs and source hashes: [logbench_memory.json](logbench_memory.json).
