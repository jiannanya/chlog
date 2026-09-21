# chlog 第三轮优化与验证：并行 sink 分派

日期：2026-09-22。基线：`4ef7c82`（第二轮结束）。本轮继续上一轮未完成的并行 sink 分派改造，
并完成正确性、内存与吞吐验证。保持原有调用接口、背压语义、关闭时等待在途调用和刷新屏障契约。

上一轮结束时，`parallel_sinks = true`（默认值）的同步 logger 即使只有一个 sink 也会为每条记录
创建线程池任务：N 个 sink 对应 N 次以上的堆分配、`std::function`、队列节点和 `shared_ptr`，
每次任务还要经过 mutex 与条件变量。本轮把这条路径改为零分配的任务环，并修复了改造中暴露的
刷新屏障缺陷。

## 实现变化

1. **并行分派不再逐记录分配。** 任务改为可平凡复制的 `{job, sink}` 对，直接存放在有界环里；
   记录由复用池中的 `parallel_job` 持有，用侵入式计数在所有 sink 任务完成后回收。
   每条记录只保留一次正文（与第二轮一致），sink 数量与分配次数解耦。
2. **整条记录的 fan-out 一次入队。** 生产者把一条记录的多个 sink 任务按批（上限 64）压入环，
   每个批次只取一次锁；消费者按批出队并用固定缓冲区处理，批次大小内的任务共享同一次锁与唤醒。
3. **job 归还不与生产者共锁。** worker 完成记录后把 job 以一次 CAS 推入无锁归还栈；记录正文与
   名称缓冲在归还时释放（下一条记录必然 move 覆盖它们，保留容量只会长期占用内存）。生产者只从
   自己的缓存取 job，仅在缓存耗尽时整体取走归还栈或分配新块，因此锁不再随每条记录在生产者与
   worker 之间来回传递。
4. **刷新屏障改到任务环。** `flush()` 等待“环内无待处理任务且没有 worker 持有正在写的批次”。
   旧实现用“已完成记录计数 ≥ 进入屏障时的接收计数”判断：在乱序完成（多 worker、多 sink）且
   屏障期间仍有新记录完成的场景下会提前返回，可能让调用方在记录尚未写完时继续执行。
   新屏障同时覆盖已入队但未处理、以及已出队但未写完的任务。
5. **单 sink 自动内联。** `parallel_sinks = true` 时，只有一个 sink 的 logger 由调用线程直接写入，
   不启动线程池；第二个 sink 出现时才启动（`sink_pool_size > 0` 仍可为单 sink 强制线程池）。
   单 sink 没有可并行的对象，线程池只会增加一次跨线程交接。视图 sink 在该模式下继续走
   “直接格式化进 sink 缓冲”的内联路径。
6. **`stats().dequeued` 改为派生值。** 未完成量 = 环内待处理 + worker 在途批次；已完成 =
   已返回调用数 − 未完成量。记录路径不再执行额外原子操作，池未启动或未运行时与 `returns` 等价，
   排空后与接收计数精确相等。
7. **刷新路径的等待者通知。** 任务环满位通知只在确有生产者等待时发出；消费者在真正空闲时才进入
   条件变量等待。唤醒只针对已停靠的线程，忙流水线不产生无效唤醒。
8. **自旋让步与架构无关。** 移除 x86 专用 `_mm_pause` 分支，统一为 `spin_backoff`：
   前 32 次仅阻止编译器优化，之后 `yield`。正确性不依赖该提示（信号量握手覆盖每次睡眠转换），
   所有架构编译同一份代码。

## 正确性验证

| 构建配置 | 结果 |
|---|---|
| macOS Release（Apple clang 17，std::format），9 项 | 9/9 通过 |
| macOS ASan + UBSan（RelWithDebInfo） | 9/9 通过，无检测报告 |
| macOS ThreadSanitizer（RelWithDebInfo） | 9/9 通过，无数据竞争报告 |
| TSan 下 queue、async、parallel、concurrent、registration 各重复 20 次 | 100/100 通过 |
| ASan 下 parallel、concurrent、registration 重复 5 次 | 15/15 通过 |
| 独立并发压力程序（Release、ASan+UBSan、TSan） | 计数一致，无报告 |

新增与加强的测试：

- **并行刷新屏障回归用例**：让一个 sink 在首批记录中阻塞，启动 `flush()` 后再提交新记录并让它们
  先完成，断言 `flush()` 在阻塞记录完成前不返回。该用例在旧的计数屏障下可稳定复现提前返回，
  在新实现下通过。
- **自动模式用例**：单 sink 由调用线程写入（`queue_size == 0`），第二个 sink 加入后线程池接管，
  两种阶段的计数都精确；显式 `sink_pool_size = 1` 时调用线程在 sink 回调阻塞期间即可返回。
- **加强断言**：格式化用例现在同时断言 `enqueued`，可捕获“异步 logger 被误判为可内联”的错误
  （该错误在本轮实现期间真实出现过一次，被测试拦截后修复）。
- **并发压力程序**：5 个生产者 × 20,000 条记录、4 个 sink、并发 `flush()`、并发 `add_sink()`
  与关闭；结束后每个初始 sink 均收到全部记录，接收、完成、队列计数一致。

## 测量方法

本轮只在本机 macOS 上测量。硬件为 Apple M4 Max（16 核），macOS 26.1，Apple clang 17.0.0，
`std::format` 后端，`-std=c++20 -O2 -DNDEBUG`。测量期间机器上有其它会话的后台负载
（一个无关基准进程间歇占满一个核心），因此所有对比都采用两个版本交替运行、取多轮中位数，
并同时给出聚焦用例的最大值。绝对吞吐只代表本次运行环境，比例用于说明变化方向。

- 计数吞吐：`chlog_bench_loggers --iters 2000000`，每轮 17 个用例，两版本交替 5 轮取中位数。
- 并行聚焦：仅运行 `sync_parallel` 的 1/2/4 sink 用例，每组 8 轮 × 5 次，取全部 40 轮中位数与最大值。
- 分配探针：替换全局 `operator new/delete` 计数，预热 1,000 条后统计 100,000 条记录的累计分配。
  分配数据反映请求次数与字节，不是 RSS。

两版本使用同一份 benchmark 源码与同一批参数；基线头文件从 `4ef7c82` 提取。

## 计数吞吐

单位：百万条/秒，越高越好。中位数比较。`sync_parallel1` 在新实现中由调用线程写入，
不再是线程池路径。

| 场景 | 基线 | 本轮 | 变化 |
|---|---:|---:|---:|
| 单线程格式化 | 33.283 | 34.241 | +2.9% |
| 线程安全同步格式化 | 32.723 | 34.088 | +4.2% |
| 并行 sink，1 个 sink（默认配置） | 3.397 | 34.483 | **+915%** |
| 并行 sink，2 个 sink | 2.326 | 5.887 | **+153%** |
| 并行 sink，4 个 sink | 1.630 | 7.113 | **+336%** |
| 异步格式化，单生产者 | 8.935 | 7.626 | -14.7%（噪声范围内，见下） |
| 异步格式化，四生产者 | 7.442 | 7.178 | -3.5% |
| 单线程纯文本 | 112.533 | 114.022 | +1.3% |
| 线程安全同步纯文本 | 109.413 | 110.814 | +1.3% |
| 异步纯文本 | 12.111 | 12.395 | +2.3% |
| 单线程 128 B 正文 | 185.170 | 194.249 | +4.9% |
| 线程安全同步 128 B 正文 | 178.930 | 186.867 | +4.4% |
| 单线程 1024 B 正文 | 182.779 | 195.449 | +6.9% |
| 线程安全同步 1024 B 正文 | 175.465 | 189.666 | +8.1% |
| 异步 128 B 正文 | 6.564 | 6.594 | +0.5% |
| 异步 1024 B 正文 | 5.898 | 5.928 | +0.5% |

非并行用例的路径只改动了内联判断：单 sink 同步写入的“是否可内联”从每次调用的 acquire 载入
（读取线程池指针与就绪标志）改为一个只向 false 翻转的缓存标志加 relaxed 载入。这与表中正文与
纯文本用例 +4% 至 +8% 的方向一致，但增幅仍在运行噪声量级，不作为稳定结论。异步单生产者用例
在 5 轮中位数上偏低 14.7%，逐轮原始数据中两版本区间重叠，且第二轮报告已说明该场景受调度影响
较大。`filtered_out` 用例耗时不足 1 ms，不在表中比较。

## 并行聚焦（1/2/4 sink）

单位：百万条/秒。每组 8 轮 × 5 次，共 40 轮。

| sink 数 | 基线中位数 | 基线最大值 | 本轮中位数 | 本轮最大值 | 倍数 |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.289 | 3.649 | 36.380 | 37.911 | 11.1× |
| 2 | 2.055 | 2.089 | 5.869 | 6.259 | 2.9× |
| 4 | 1.356 | 1.469 | 6.232 | 7.040 | 4.6× |

单 sink 的 11 倍来自第 5 项：默认配置不再为唯一的 sink 启动线程池，直接复用第二轮的同步写入路径。
2/4 sink 的收益来自零分配分派、批处理与屏障简化；4 sink 的收益高于 2 sink，因为基线的
N 次任务分配随 sink 数线性增长，而本轮与 sink 数无关。

## 分配开销

每组 100,000 条记录，探针统计替换 `operator new` 后的累计请求次数与字节（非 RSS）。
短消息为 SSO，不产生正文分配。

| 场景 | 基线分配次数 | 基线字节 | 本轮分配次数 | 本轮字节 |
|---|---:|---:|---:|---:|
| 4 个 sink，短消息 | 500,097 | 35,589,312 | 2 | 3,632 |
| 2 个 sink，短消息 | 300,079 | 24,311,520 | 0 | 0 |
| 4 个 sink，200 B 正文 | 600,076 | 56,303,200 | 100,004 | 20,807,248 |

短消息场景剩余 2 次分配来自首批 job 块与任务环缓冲；200 B 正文场景保留每条记录一次正文分配
（与第二轮一致；正文由记录持有并 move，不在 sink 任务间复制）。

## 复现入口

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
./build-release/chlog_bench_loggers --iters 2000000

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

原始各轮输出、头文件与 benchmark 源码 SHA-256 见
[optimization_round3_benchmarks.json](optimization_round3_benchmarks.json)（本地生成，未纳入版本库）。

## 行为边界

- 单 sink 的 `parallel_sinks = true` logger 由调用线程写入；需要把写入移出调用线程时设置
  `sink_pool_size > 0`，或改用 `async.enabled = true`。
- 多 sink 时线程池在第二个 sink 注册后启动；注册前写入的记录已经由调用线程完成。
- `flush()` 保证覆盖调用前被接受的记录；与屏障并发提交的记录仍可能在途。sink 回调不可重入
  自己的 logger（包括不得在 sink 内调用 `flush()`）。
- 运行中的统计快照沿用第二轮的说明：可能观察到正在发布的记录；排空后计数精确。
- 本轮验证在 macOS（Apple Silicon，ARM64）与 Apple clang 17 上完成；未重新运行 Windows 与
  Linux 的编译、测试与测量，上一轮的相应结论仍以当时构建为准。
