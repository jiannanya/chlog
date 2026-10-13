# 第八轮：短日志与异步提交

2026-10-13。在第七轮工作区版本上继续优化，保留既有长格式化、JSON 转义、文件直写、
线程 ID 缓存和缓冲复用。公开日志、sink、配置及队列 API 不变，新增实现仅使用标准 C++20。

## 改动

- 一个裸 `{}` 字段直接检查前后两个字面量区间，省去通用解析器的动态字段计数和索引写入。
  计划数组按实际参数数目分配，仍限制最多 8 个字段；输出使用编译期索引展开。
  每次检查实际格式文本，不缓存字符串地址，保持运行时字符串、转义、错误格式、NUL 和自定义 formatter 的行为。
- 异步提交直接构造紧凑的队列事件，并向事件自己的字符串写入字面量、字符串和裸字段。
  省去完整 `log_event` 中间对象和短字符串搬运，正文仍由队列拥有，右值字符串参数不会被消耗。
  同步 owning sink 和并行 sink 也直接填充最终正文。
- 消费者释放槽位后，仅在有阻塞生产者时调用 `space_epoch.notify_all()`。
  阻塞接口也先尝试直接入队，成功时不修改等待者计数；确实需要等待时才登记，并在成功或停止后退出。
  等待者登记、epoch 读取/推进及通知条件使用顺序一致原子操作。
  在同一 SC 顺序下，消费者要么看到等待者并通知，要么生产者读取到新的 epoch，重试时看到槽位释放或停止。
  高低优先级队列在整个批次释放后只通知一次，减少共享 epoch 的写入和唤醒开销。
  若后续队列分配输出空间失败，作用域守卫仍通知已释放的槽位，避免异常路径漏唤醒。
  保留原有数据唤醒交换、精确队列容量、加权公平性、满队列策略和 flush/shutdown 屏障。

## 验证方式

同一份基准源码分别编译三个版本的头文件：R6 为 `9a5f81b`，R7 为本轮开始时保存的工作区头文件，
R8 为本轮实现。环境为 macOS / Apple M4 Max（16 核）、Apple clang 17、`-std=c++20 -O3 -DNDEBUG -pthread`，
分别使用 std 和 fmt 12.1.0，没有可选 spdlog 的隐式后端切换。

六轮轮换并反转三个版本的运行顺序。常用日志场景每项 10,000,000 条，热点每项 1,000,000 次，
小队列及并行 sink 每项 500,000 条，文件各 20,000 条。异步时间包括消费与 shutdown 排空，
同时检查 processed、dropped 或输出字节。吞吐变化为 `旧时间中位数 / 新时间中位数 - 1`。

机器上同时存在其他项目的 CPU/I/O 基准进程，尤其异步和磁盘结果会受到调度影响。
保留全部样本，不把负向结果自动归为噪声，也不将纳秒级热点收益外推为整个应用的收益。

## 基准结果

括号为 R8 在对应六轮配对中较快的次数；正数表示吞吐提高。

### 常用日志与队列

| 场景 | std vs R7 | fmt vs R7 | std vs R6 | fmt vs R6 |
|---|---:|---:|---:|---:|
| filtered_out | -0.6% (2/6) | +1.4% (2/6) | -1.1% | +1.4% |
| sync_st | +6.5% (6/6) | +8.0% (5/6) | +6.5% | +13.3% |
| sync_mt | +7.8% (6/6) | +9.5% (5/6) | +6.9% | +16.7% |
| async_mt | +7.3% (5/6) | -29.4% (2/6) | +5.0% | -23.1% |
| async_4p | +11.3% (3/6) | -0.0% (3/6) | +6.5% | +2.9% |
| sync_st_literal | +2.9% (4/6) | -0.9% (4/6) | +1.0% | -5.6% |
| sync_mt_literal | +3.4% (5/6) | +6.4% (5/6) | +1.9% | +4.4% |
| async_mt_literal | +3.3% (4/6) | +4.5% (3/6) | +3.3% | +12.9% |
| sync_st_payload128 | +0.9% (4/6) | +1.0% (4/6) | +0.6% | -5.0% |
| sync_mt_payload128 | +0.7% (4/6) | +4.4% (5/6) | +1.5% | +4.7% |
| sync_st_payload1024 | +2.4% (3/6) | +0.8% (5/6) | +3.1% | -0.4% |
| sync_mt_payload1024 | -2.6% (3/6) | +0.1% (2/6) | -0.4% | -1.1% |
| async_mt_payload128 | -2.6% (3/6) | +11.9% (5/6) | +1.2% | +4.0% |
| async_mt_payload1024 | -1.9% (1/6) | +2.7% (4/6) | -2.6% | +13.1% |
| async_mt_weighted | +101.4% (6/6) | +41.1% (4/6) | +109.8% | +61.7% |
| async_4p_weighted | +14.6% (6/6) | +2.6% (3/6) | +31.8% | +2.4% |

原始数据：[loggers std](optimization_round8_loggers_std.csv)、[loggers fmt](optimization_round8_loggers_fmt.csv)。

### 小队列与并行 sink

| 场景 | std vs R7 | fmt vs R7 | std vs R6 | fmt vs R6 |
|---|---:|---:|---:|---:|
| sync_parallel1 | +13.6% (5/6) | +6.4% (6/6) | +13.4% | +6.5% |
| sync_parallel2 | +16.7% (6/6) | -8.3% (1/6) | -49.1% | -52.4% |
| sync_parallel4 | +7.0% (5/6) | +0.9% (4/6) | -45.8% | -35.8% |
| async_mt_q8 | +3.9% (3/6) | +23.4% (5/6) | +11.3% | +37.1% |
| async_4p_q8 | -26.5% (2/6) | +1.8% (3/6) | -0.5% | +0.2% |

原始数据：[scheduling std](optimization_round8_scheduling_std.csv)、[scheduling fmt](optimization_round8_scheduling_fmt.csv)。

### 上一轮热点收益复测

| 场景 | std vs R7 | fmt vs R7 | std vs R6 | fmt vs R6 |
|---|---:|---:|---:|---:|
| bare_integer | +7.9% (5/6) | +11.1% (5/6) | +12.5% | +12.5% |
| format_specs | +0.6% (3/6) | -1.4% (2/6) | +2.5% | -0.4% |
| format_long | -0.6% (2/6) | +6.0% (3/6) | +14.1% | +4.8% |
| runtime_specs | +0.5% (4/6) | +5.1% (4/6) | +2.9% | +2.9% |
| async_pattern_8 | -3.4% (1/6) | -3.9% (2/6) | +1.5% | -3.7% |
| thread_ids_1 | +3.8% (3/6) | +8.8% (4/6) | +50.8% | +45.9% |
| thread_ids_8 | +2.6% (4/6) | +6.6% (4/6) | +4499.5% | +4736.5% |
| thread_ids_32 | -1.7% (2/6) | +9.1% (3/6) | -4.7% | -0.7% |
| json_ascii | +0.2% (3/6) | +5.0% (4/6) | +5.1% | +2.9% |
| json_controls | +2.7% (6/6) | -1.5% (3/6) | +13.5% | +12.2% |
| daily_128 | +13.3% (4/6) | +10.7% (5/6) | +2.9% | +13.0% |
| daily_4096 | -1.9% (2/6) | +3.1% (5/6) | +6.0% | +3.3% |

原始数据：[hotpaths std](optimization_round8_hotpaths_std.csv)、[hotpaths fmt](optimization_round8_hotpaths_fmt.csv)。

同步短整数日志在两个后端均有稳定收益：相对 R7 的常用日志驱动提升 6.5%–9.5%，
热点驱动提升 7.9%–11.1%。std 长格式化相对 R6 仍提升 14.1%；8 个交错线程 ID 查询仍约为
R6 的 46–48 倍；密集 JSON 转义相对 R6 仍提升 12.2%–13.5%。
fmt 长格式化相对 R7 的中位数提升 6.0%，但只有 3/6 轮胜出，不能视为稳定的额外收益。

加权队列本轮合并批次通知后，单生产者相对 R7 提升 std 101.4%、fmt 41.1%；四生产者提升
std 14.6%、fmt 2.6%。大容量异步正文、八生产者完整模板和多 sink 并行并未全部改善。
缓存仍限制为 16 条，32 个线程 ID 的循环工作集会持续失配，其代价保留在表中。

### 负向结果的独立复核

统一终测的 fmt `async_mt` 为 -29.4%，std `async_4p_q8` 为 -26.5%，与之前定向测试方向相反。
因此另外一次只运行一个场景，使同轮三个版本紧邻执行；保留上述完整终测数据，单独记录复核结果。
fmt 单生产者每项仍为 10,000,000 条，小队列和完整模板每项增加到 5,000,000 条，均六轮轮换。

| 单独复核场景 | R8 vs R7 | R8 vs R6 | 原始数据 |
|---|---:|---:|---|
| fmt，单生产者 FIFO 整数 | +9.8% (5/6) | +9.6% | [CSV](optimization_round8_isolated_fmt_async.csv) |
| std，四生产者，8 槽位 | -1.5% (3/6) | +3.8% | [CSV](optimization_round8_isolated_std_q8.csv) |
| fmt，四生产者，8 槽位 | +9.9% (6/6) | +3.0% | [CSV](optimization_round8_isolated_fmt_q8.csv) |
| std，八生产者完整模板 | +0.3% (4/6) | -5.6% | [CSV](optimization_round8_isolated_std_pattern.csv) |
| fmt，八生产者完整模板 | -0.0% (4/6) | -5.6% | [CSV](optimization_round8_isolated_fmt_pattern.csv) |

fmt 单生产者的 -29.4% 和 std 小队列的 -26.5% 大幅回退没有在单场景复测中重现。
但小队列 std 的 -1.5%、八生产者完整模板相对 R6 的 -5.6%，以及统一终测中的并行 sink 负向项仍保留。
八生产者完整模板相对 R7 基本持平，不能据此声称所有异步场景都更快。
同步短整数、加权队列和原有 JSON/缓存热点的收益有更一致的样本支持。

多 sink 并行相对 R6 的负向差异在本次高负载终测中很大，R7 也有这一现象。
保留了 R7 防止持锁线程被抢占后无限空转的退避修复；本轮没有独立隔离调度负载与该修复各自的影响。
fmt 双 sink 相对 R7 为 -8.3%，属于仍未证明改善的场景。

## 正确性与可移植性

| 本机配置 | 结果 |
|---|---|
| Release，std | 12/12 |
| Release，fmt | 12/12 |
| ASan + UBSan，std | 12/12 |
| ASan + UBSan，fmt | 12/12 |
| TSan，std | 12/12 |

新增回归覆盖 5,461 个短花括号组合、小队列反压和多生产者停止、异步 SSO 边界/长正文/NUL、
调用者复用缓冲、错误格式恢复、序号、线程 ID、时间和源码位置。原有嵌套 formatter、TLS 析构、
并发配置、队列公平性、flush/shutdown 和安装包消费者继续通过。

分配诊断沿用第七轮口径：队列初始化为 4,736,064 B / 10 次 C++ 分配；异步 logger 名称没有逐条分配，
fmt 的 1 KiB 同步复杂格式缓冲在热身后仍无 C++ 分配，超过 8192 B 的块仍直接释放。
探针不统计直接 `malloc`，不能用来推断 fmt 后端的全部分配量。
原始诊断：[std](optimization_round8_allocations_std.txt)、[fmt](optimization_round8_allocations_fmt.txt)。

[Windows / Linux / macOS CI](../.github/workflows/ci.yml)继续覆盖 std/fmt 及 Linux sanitizer。
本次实际运行的是 macOS，本机通过与远程 CI 通过不是同一件事。

## 复现

```sh
# 用归档补丁复原本轮开始时的 R7，避免把当前头文件误当成旧基线。
mkdir -p build-r8-r7/include/chlog build-r8-r6/include/chlog build-r8
git show 9a5f81b:include/chlog/chlog.hpp > build-r8-r6/include/chlog/chlog.hpp
cp build-r8-r6/include/chlog/chlog.hpp build-r8-r7/include/chlog/chlog.hpp
git apply --directory=build-r8-r7 docs/optimization_round8_r7.patch

# 三个版本使用同一驱动和参数。以下编译 R8；R6/R7 分别将 include
# 换为 build-r8-r6/include、build-r8-r7/include，输出改为对应目录。
c++ -std=c++20 -O3 -DNDEBUG -pthread -I include \
  benchmarks/loggers_bench.cpp -o build-r8/loggers
c++ -std=c++20 -O3 -DNDEBUG -pthread -I include \
  benchmarks/hotpaths_bench.cpp -o build-r8/hotpaths

python3 tools/benchmark_compare.py --driver loggers --iterations 10000000 --repeats 6 \
  --revision r6=build-r8-r6/loggers --revision r7=build-r8-r7/loggers --revision r8=build-r8/loggers \
  --case sync_st --case sync_mt --case async_mt --case async_4p --case async_mt_literal \
  --output build-r8/loggers.csv
python3 tools/benchmark_compare.py --driver hotpaths --iterations 1000000 --repeats 6 \
  --revision r6=build-r8-r6/hotpaths --revision r7=build-r8-r7/hotpaths --revision r8=build-r8/hotpaths \
  --output build-r8/hotpaths.csv

cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build build-perf
ctest --test-dir build-perf --output-on-failure
cmake -S . -B build-perf-fmt -DCMAKE_BUILD_TYPE=Release -DCHLOG_USE_FMT=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build build-perf-fmt
ctest --test-dir build-perf-fmt --output-on-failure
```

fmt 的三次编译同时增加 `-DCHLOG_USE_FMT=1` 和相同的 fmt include/library 参数。
`loggers` 的 `--case` 可重复，未指定时执行所有场景；`hotpaths` 在迭代次数后接受场景名称。
比较脚本检查返回的场景集合和消费计数，保留运行次序并输出中位数与逐轮胜出次数。

头文件 SHA-256：

- R6：`55bb6a73063d4d3b26dd4b871c4ca6c5e939496d1cd7454e5dc7af5c557cb262`
- R7：`f64d629b6b91b5ed089eb6c66f5184f9a5da96e03ebdc3aa748f4edb99bfbc33`
- R8：`ab2ad2cccd1b92077e2b6ac3aa5e087c1f735ecd72b21b0f7cd2b950c2300812`
