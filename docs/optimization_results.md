# chlog 优化与验证报告

日期：2026-09-08。比较基线：`d63ceda126cc6165c8cf1101ae5f16db8978882d`。

本次完成了队列、生命周期、格式化、文件输出、配置并发、构建集成与测试方面的改进。
保持 C++20、header-only 和原有日志调用方式；新增 API 均为补充接口。

## 功能与正确性

| 原问题 | 当前行为 |
|---|---|
| 容量为 1 的环形队列会混淆空槽和已发布槽，可能覆盖尚未消费的对象 | 单环最小容量 2；双队列最小总容量 4，测试覆盖 0/1/2/3/4/7 等配置和反复回绕 |
| 消费者检查空队列和进入休眠之间可能丢失唤醒；重复停止信号可能使二进制信号量许可累积 | 使用发布与休眠握手；重新休眠前消费已承诺的唤醒许可；关闭可重复调用 |
| 阻塞生产者即使收到空位通知，仍可能等待完整 1 ms | 使用空位代数与 C++20 `atomic::wait/notify_all`，及时唤醒并支持关闭取消 |
| 高优先级持续占满时，低优先级可能长期得不到服务 | 批次在两个优先级队列之间分配服务，批量大小为 1 时也轮流服务 |
| `weighted_queue = false` 没有效果 | 使用一个 FIFO 环形队列 |
| `batch_max = 0` 无法消费；巨大 batch 导致过量预分配 | 规范化为 `[1, 实际队列容量]` |
| 异步 `flush()` 只刷新 sink，不等待记录写完 | 捕获两个队列的发布位置，等待对应记录完成再刷新，避免优先级超车造成假完成 |
| 并发关闭与在途生产者、阻塞提交存在竞态；关闭后仍可能记录或虚增统计 | 关闭接收入口、取消阻塞提交、等待在途调用，再排空工作线程与任务池；多次关闭等待同一完成结果 |
| 同步模式的统计缺失，消费计数可能先于实际 sink 写入 | 各模式均统计接收/完成记录；最终计数在 shutdown 后可核对 |
| 多个 `log_at` 实现重复，部分入口绕过过滤；`off` 可能被当作记录级别 | 统一提交路径，各入口遵守过滤，`off` 不输出 |
| 动态修改日志级别、刷新级别与渲染模板存在数据竞争 | 原子级别配置、不可变模板快照；内置 sink 可并发渲染与更新模板 |
| 正文中的 `{file}`、`{line}`、`{func}` 会再次参与字符串替换 | 模板只解析一次，插入的正文与 logger 名称作为字面内容处理 |
| JSON 的 logger 名称未转义 | 所有 JSON 字符串字段统一转义，覆盖控制字符、引号、反斜杠及 NUL |
| `{msg}` 自动禁用元数据后，daily/JSON sink 输出错误；切换模板也无法恢复 | sink 声明所需元数据；只跳过不需要的字段，保留显式 `capture_* = false` 约束 |
| 文件打开/写入/轮转失败被静默忽略；Windows 换行导致轮转字节计算不准确 | 二进制写入、打开失败抛异常、运行时失败计入 `stats().errors`；在完整记录前轮转 |
| Windows 负时间戳可能将无效 `tm` 传入 CRT；负毫秒不规范 | 不支持的本地日期回退到 UTC，毫秒规范化，极值日期安全处理 |
| fmt 后端仍依赖 `<format>`，不支持仅定义 `fmt::formatter` 的类型 | 使用 fmt 原生格式字符串与 formatter；运行时格式参数使用有效的左值引用 |
| MSVC 传统预处理器下无附加参数的日志宏编译失败 | 宏将格式与参数作为同一变参组传递 |

新增 `log_raw()`、`should_log()`、`queue_capacity()`、`stats().errors`、
`sink::required_metadata()` 和 `sink_queue_capacity`。

## 性能与内存实现

- 模板预解析成 token/literal 序列；仅生成模板需要的字段，正文不再经过多轮搜索替换。
- 每线程缓存秒级日期文本和线程号文本；数字使用 `to_chars`，JSON 直接追加转义内容。
- 双队列按实际配置分配槽位，不再对高低队列分别向上取整。
- 消费者独占 head，移除无用的消费端 CAS；分离部分争用原子的缓存行。
- 异步内部记录省去每条独立的 logger 名称，仅记录是否需要捕获名称；消费者复用名称存储。
- 复用同一个批次 vector，关闭时不再额外分配 drain vector，空闲前释放已消费的大 payload。
- 并行 sink 的等待任务数受 `sink_queue_capacity` 限制，默认 1024；另有正在执行的任务。
  每条记录的字符串由各 sink 任务共享，避免按 sink 数量重复复制。

容量限制约束的是记录/任务数量。超长 payload、自定义 sink 内部缓存以及同时提交的调用仍会增加内存；
本次测量不代表进程 RSS 的上限。

## 同机测量

环境：Windows x64，Intel Core i9-12900K，约 31.75 GiB RAM，Clang 17.0.6，
MSVC STL 14.38，`std::format` 后端。两个版本使用**同一份修正后的 benchmark 源码**，
以 `-std=c++20 -O2 -DNDEBUG` 编译。基线头文件从上述 Git 提交提取。
两版本交替运行，各 5 次，表格报告中位数；测量时未同时执行本任务的构建或测试。
原始运行输出见 [optimization_benchmarks.json](optimization_benchmarks.json)。

### 完整日志调用与模板渲染

每组 100,000 条消息，包含日志 API、消息格式化、元数据捕获、完整默认模板渲染、
sink 分发与最终排空，sink 渲染到内存并累计输出字节，不包含磁盘 I/O。

| 场景 | 优化前 | 优化后 | 加速 |
|---|---:|---:|---:|
| 单线程完整模板 | 422.560 ms | 16.837 ms | 25.10× |
| 异步完整模板 | 408.341 ms | 22.674 ms | 18.01× |

### 单独渲染热点

每组 100,000 次渲染，固定事件，预热后开始计时。
此探针同时记录堆分配，因此倍数包含分配计数本身的开销；它不是整个应用的加速倍数。

| 模板 | 优化前 | 优化后 | 每条堆分配：前 → 后 |
|---|---:|---:|---:|
| `{msg}` | 408.821 ms | 3.320 ms | 26 → 1 |
| 日期、时间、级别、名称和正文 | 466.778 ms | 5.863 ms | 28 → 1 |
| `{json}` | 256.432 ms | 14.327 ms | 17 → 1 |

### 堆分配

以下为替换 `operator new` 统计的请求分配字节，**不是 RSS**。

| 场景 | 优化前 | 优化后 |
|---|---:|---:|
| 配置 65,536 槽的 async logger 构造及 worker 初始化/关闭 | 9,888,510 B | 6,314,735 B（降低 36.14%） |
| 128 字符 logger 名称，预热后记录 100,000 条短消息 | 100,001 次分配，14,428,711 B | 0 次分配，0 B |

基线的 65,536 配置实际上分配了 81,920 个槽；当前实现分配 65,536 个槽，且槽中的内部记录更小。
长名称探针的 0 分配是在指定预热、短 payload、计数 sink 条件下测得，不适用于所有消息和 sink。

### 纯计数 sink 的成本与取舍

每组 1,000,000 次调用，async 使用固定 65,536 槽和阻塞溢出策略。
过滤场景之外，两版本所有调用均处理完成，丢弃为 0。

| 场景 | 优化前 calls/s | 优化后 calls/s | 变化 |
|---|---:|---:|---:|
| 过滤掉的调用 | 3.74532e9 | 3.03859e9 | -18.87% |
| 单线程，计数 sink | 2.31001e7 | 2.18904e7 | -5.24% |
| 线程安全同步，单生产者 | 1.52882e7 | 1.25003e7 | -18.24% |
| 异步，单生产者 | 4.27009e6 | 3.59618e6 | -15.78% |
| 异步，四生产者 | 8.07197e6 | 6.61735e6 | -18.02% |

这里存在明确回退：关闭期间的在途调用保护、线程安全配置、完成统计与正确唤醒增加了同步成本，
空 sink 无法从模板渲染优化中受益。本次没有通过关闭这些保证来追求计数 sink 的最高跑分。
真实收益取决于模板、消息长度、sink 和并发方式；需要用实际业务负载验证。

## 编译、测试与压力验证

| 配置 | 结果 |
|---|---|
| Windows Clang 17 Release，std 后端 | 8/8 通过 |
| Windows MSVC 19.38 Debug，std 后端 | 8/8 通过 |
| Windows Clang 17 Release，fmt 11.1.4 | 8/8 通过 |
| Linux/WSL GCC 13.3 Release，std 后端 | 8/8 通过 |
| Linux/WSL Clang 18，AddressSanitizer + UndefinedBehaviorSanitizer | 8/8 通过，无检测报告 |
| Linux/WSL Clang 18，ThreadSanitizer | 8/8 通过，无数据竞争报告 |

每组的第 8 项会将头文件与 CMake package 安装到构建目录，使用独立 CMake 项目
`find_package(chlog CONFIG REQUIRED)` 编译运行，验证 Threads/fmt 依赖传递。
Windows 消费者还先包含 `windows.h`，检查 min/max 宏兼容性。
安装消费者使用普通 Release 配置；sanitizer 覆盖前 7 项核心测试。

ThreadSanitizer 下额外将 queue、async、parallel、concurrent 四组各重复 20 次，合计 80 次通过。
测试涵盖实际数据唯一性、优先级保留、取消阻塞、重复/并发关闭、刷新屏障、任务容量限制、
轮转内容、打开失败、元数据恢复、显式禁用字段、两种后端的自定义 formatter 和运行时格式参数。

Windows Clang 17 的 ASan 曾因本机 sanitizer 运行库拦截 `memcpy` 失败而无法启动测试；
该环境不计入通过项，内存及未定义行为验证改在 Linux Clang 18 完成。
未在 macOS 或 ARM 硬件上运行本次验证。

原有 `chlog_stress` 示例实际运行 20 个生产线程、300,000 次调用、四种输出 sink：

- 耗时 186 ms，处理 51,518 条、主动丢弃 248,482 条，队列最终为 0。
- `enqueued == dequeued`，且 `enqueued + dropped == 300000`。
- JSON 文件逐行解析通过；全部 300 条 ERROR 和 1,200 条 WARN 均被保留。
- 示例开启丢弃策略，上述低优先级丢弃是预期过载行为；无丢弃场景另外由阻塞队列测试及百万次基准验证。

`chlog_single_thread_bench 500000` 同样运行成功，计数 501,000（包含 1,000 条预热）。
示例的吞吐计算已排除预热数量，压力示例也避免了耗时为 0 时除零。

## 使用与复现

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
./build-release/chlog_bench_loggers --iters 1000000
./build-release/chlog_render_bench 100000
```

fmt 后端使用 `-DCHLOG_USE_FMT=ON` 与已安装 fmt 的 `CMAKE_PREFIX_PATH`。
手工包含头文件时，fmt 宏必须在所有翻译单元中保持一致，并链接 fmt。
运行时参数封装使用命名左值，符合 [fmt 官方 API 的生命周期要求](https://fmt.dev/11.0/api/)。

Linux 检测构建：

```sh
cmake -S . -B build-linux-asan -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-linux-asan
ctest --test-dir build-linux-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -fno-pie" \
  -DCMAKE_EXE_LINKER_FLAGS=-no-pie \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

行为边界：关闭后的新调用被忽略；单线程模式仍要求外部单线程使用；sink 回调不可重入自己的 logger；
动态模板切换会影响尚未渲染的排队记录，明确分界需先 `flush()`；流刷新不等同于磁盘持久化同步。
具体配置说明见 [README](../README.md)。
