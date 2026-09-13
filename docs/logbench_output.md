# File-output comparison

Recorded: 2026-09-13T21:31:12.808227+08:00

CPU: 12th Gen Intel(R) Core(TM) i9-12900K. Host: Windows-10-10.0.26200-SP0. Affinity: 0x5555.

Median of 7 alternating-order runs; 500,000 records per 128-byte case and 100,000 per 1,024-byte case, for each library.
Backend: fmt; fmt: 110104; spdlog: 1.15.3.

Rotating-file sink object sizes: chlog 232 B; spdlog ST 392 B and MT 464 B. These sizeof values exclude the logger, allocator, file buffers and heap-owned state.

Both libraries use their built-in rotating-file sink with rotation disabled, identical logger names, prebuilt payloads formatted with `{}`, and binary LF newlines. The full pattern contains the local date, time, milliseconds, logger name and payload. Both async queues are blocking FIFO queues with 65,536 slots and one worker; chlog uses batches of 256 and no periodic flush. ST uses a single-thread sink; MT uses a thread-safe sink with one calling thread.

Timing includes logging and flushing library buffers; async also includes draining and joining the worker. Each library and case first performs a separate, untimed 10,000-record warmup with full output validation. Construction, directory creation and validation are outside the timed interval. Every record and output byte count is checked. Files use the system temporary directory and the normal filesystem cache; there is no fsync/durable-storage barrier. Results depend on the filesystem, cache and competing I/O.

| Case | chlog calls/s | spdlog calls/s | chlog / spdlog |
|---|---:|---:|---:|
| async_mt_message_1024 | 707,679 | 636,820 | 1.11x |
| async_mt_message_128 | 4,187,671 | 2,665,582 | 1.57x |
| async_mt_pattern_1024 | 656,570 | 588,714 | 1.12x |
| async_mt_pattern_128 | 3,217,994 | 1,659,792 | 1.94x |
| sync_mt_message_1024 | 838,894 | 723,450 | 1.16x |
| sync_mt_message_128 | 5,538,337 | 4,625,124 | 1.20x |
| sync_mt_pattern_1024 | 739,720 | 674,433 | 1.10x |
| sync_mt_pattern_128 | 3,635,454 | 3,281,897 | 1.11x |
| sync_st_message_1024 | 859,760 | 736,716 | 1.17x |
| sync_st_message_128 | 6,138,253 | 5,041,034 | 1.22x |
| sync_st_pattern_1024 | 749,052 | 678,516 | 1.10x |
| sync_st_pattern_128 | 3,866,375 | 3,410,709 | 1.13x |

Source SHA-256: `8831b4d7c5768d13c70cb81b8796fa48964d58da2c58a802b1f1c9f251615545`

[Raw measurements](logbench_output.json) | [Counter-sink throughput](logbench_results.md) | [Memory](logbench_memory.md)
