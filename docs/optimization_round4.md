# chlog 第四轮优化与验证：格式化热路径

日期：2026-09-29。基线：`4164b73`（第三轮结束）。本轮针对**日志调用的格式化路径**做优化：
先用采样定位，再把可证明等价的常见形状直接生成，其余形状一律交回通用格式化器。
保持原有调用接口、过滤器语义、错误处理（格式化失败仍计入 `stats().errors` 并回落到原始文本）、
异步/并行分派与关闭契约不变。改动不含任何 CPU 特异性指令或内联汇编。

## 定位：格式化器本身占了 95%

用 `sample` 对 `sync_mt`（单线程写入、`{msg}` 模板、视图 sink、`info("v {}", i)`）采样 6 秒，
按线程分段后统计各帧自耗时：

| 符号 | 自耗时占比 |
|---|---:|
| `std::__vformat_to`（含其内联的 replacement field 处理） | **78.4%** |
| `chlog::logger::dispatch_view`（含登录守卫、遍历、虚调用） | 11.8% |
| 其余（`__itoa`、`memmove`、sink 回调等） | <10% |

即：`submit_formatted_view` 占 95.2%，其中格式化器占 78.4%，chlog 自身的分发与 sink 开销不足 5%。
上限实验（同一台机器，`-O2`，单位 ns/次）说明问题不在 chlog 而在格式化器：

| 写法 | ns/op |
|---|---:|
| `std::format_to(back_inserter(string), "v {}", i)` | 28.3 |
| `std::format_to(back_inserter(string), "{}", i)` | 24.1 |
| 字面量 + `std::to_chars` 追加进 `string` | 12.5 |
| `to_chars` 只写进栈缓冲 | 2.0 |

`std::format_string` 的 `consteval` 构造只做**校验**，不生成编译期格式；libc++
（`__format/format_functions.h`）在每次调用时重新解析格式串并走通用分派。所以
"字面量只占 4 ns、机器本体占 24 ns"——这 24 ns 就是本轮要绕开的部分。

## 实现变化

1. **受限语法快速路径（`format_bare_fields`）。** 格式串形如"字面量与裸 `{}` 字段交替"时，
   单趟扫描出各字面量区间（n 个字段对应 n+1 个字面量区间），再按顺序执行：
   追加字面量 → 追加参数 → … → 追加字面量。字段与参数用一次逗号折叠同步推进，
   既不需要运行期解析参数下标，也不需要 `visit_format_arg` 分派。
   `std::format` 对 `{}`（空格式说明）的规定结果，正好是各类型的默认表示，
   因此可以**直接生成**而不必经过格式化器。

2. **参数类型的可证明等价集合。** 只有当每个参数都存在"与默认格式说明完全一致"的表示时才启用：

   | 参数类型 | 生成方式 | 与 `{}` 一致的理由 |
   |---|---|---|
   | 整数（`to_chars` 有重载者） | `std::to_chars` 十进制 | 默认说明即十进制、无本地化 |
   | 浮点 | `std::to_chars`（最短往返） | 默认说明定义为最短往返表示 |
   | `bool` | `"true"` / `"false"` | 默认说明规定 |
   | `char` | 原字符 | 默认说明规定 |
   | `std::string` / `std::string_view` | 原字节 | 默认说明规定 |
   | `const char*` / `char[N]` | `char_traits::length` 后原字节 | 默认说明规定 |

   其余类型（枚举、指针、自定义 formatter、扩展 128 位整数、`wchar_t`/`char16_t` 等字符型整数）
   在**编译期**整体关闭快速路径（`if constexpr`），只走一次通用格式化器，异常与诊断路径完全不变。

3. **运行期必须拒绝的形状**（退回通用格式化器，行为逐字不变）：
   带格式说明（`{:04}`）、定位下标（`{0}`）、转义花括号（`{{`/`}}`）、孤立花括号、
   字段数与实参数不符、超过 8 个字段、空 C 字符串指针。

4. **非有限浮点交给通用格式化器。** 这是本轮唯一需要"多让一步"的地方：`to_chars` 会输出
   NaN 的载荷（`nan(snan)`、`-nan(ind)`），而默认格式说明必须只输出 `nan` / `-nan`，
   且具体拼写由实现决定。因此在参数校验阶段用 `std::isfinite` 把 `inf`/`nan` 直接排除，
   由实现的格式化器给出它自己的拼写——等价性由构造保证，不依赖对标准的解读。

5. **`has_no_braces` 取代 `find_first_of("{}")`。** "无花括号即原样输出"是零参数路径上唯一的检查，
   而 libc++ 的 `string_view::find_first_of` 每次调用都要建 256 项查找表。
   单趟扫描实测 0.26 ns vs 0.56 ns，用于 `format_payload` / `vformat_payload` /
   `submit_format` / `submit_runtime` 四处。这一项独立于快速路径，本身即可测量。

6. **不做格式串缓存。** 快速路径每次调用重新扫描格式串（实测约 1.7 ns），不使用
   `thread_local` 表，也不以格式串地址为键。因此运行时格式串（`log(lv, runtime_string, ...)`）
   同样可以走快速路径，而没有任何"字符串已释放/同址复用导致陈旧计划"的隐患。

## 正确性验证

新增测试套件 `fastformat`（注册于 `tests/CMakeLists.txt`），核心是把快速路径的输出与
**通用格式化器**的输出逐字节比较，而不是与硬编码期望值比较：

- 40000 个随机位模式的 `double` / `float`（含非有限值，接受率 >39000/40000，保证测试真的覆盖到快速路径）；
- 整数全边界（`INT_MIN`、`INT64_MIN`、`ULLONG_MAX`、各短整型极值）、`char` 全部可打印取值、`bool`；
- 空串、内嵌 NUL 的 `std::string`/`string_view`、C 字符串、可变 `char[]`；
- 字段位置穷举（前缀/中缀/后缀/纯字段/纯字面量/多字段）；
- 编译期断言：接受类型集合与拒绝类型集合（`void*`、`int*`、`std::vector<int>`、自定义类型、
  `wchar_t`、`char16_t`）；
- 拒绝路径：带说明、定位下标、转义花括号、孤立花括号、字段数与实参不符、超字段预算、
  空 C 字符串指针，并断言**被拒绝时不写入任何字节**（因为调用方随后会用同一个缓冲跑通用格式化器）；
- 四种投递模式（单线程 / 同步多线程 / 异步 / 并行 sink）下两条 sink 收到的载荷逐字节一致。

| 构建配置 | 结果 |
|---|---|
| macOS Release（Apple clang 17，`std::format`） | 10/10 通过 |
| macOS ASan + UBSan（RelWithDebInfo） | 10/10 通过，无检测报告；5 个并发/格式化套件重复 5 次 = 25/25 |
| macOS ThreadSanitizer（RelWithDebInfo） | 10/10 通过，无数据竞争报告；6 个套件重复 10 次 = 60/60 |
| Release 6 个套件重复 10 次 | 60/60 通过 |
| 独立并发压力程序（Release） | 通过 |

测试实现过程中的两个自纠：一张用例文本写成 4 个字段却传 4 个实参（实现正确地拒绝，
测试自身写错）；并行模式下让 worker 乱序完成记录，最初的"按序比较"假设不成立，
改为多重集比较（仅并行模式允许乱序，其余模式仍断言顺序）。

## 测量方法

- 硬件 Apple M4 Max（16 核），macOS 26.1，Apple clang 17.0.0，`-std=c++20`，`CMAKE_BUILD_TYPE=Release`。
- **受控构建**：基线与本轮都显式加 `-DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON`，保证两侧都是 `std::format` 后端。
  （本机装了 Homebrew 的 fmt/spdlog，`benchmarks/CMakeLists.txt` 在 `find_package` 成功时
  会给基准目标加 `CHLOG_USE_FMT`；重新 configure 会静默切换基准后端，所以必须显式关掉。）
- 两侧交替执行、取 5 轮中位数，并给出逐轮胜负次数；噪声底用同一二进制与自身副本对比。
- 本机在会话期间存在明显的背景负载漂移：同一基线二进制两次运行的绝对值可相差一倍以上
  （例如 `async_4p` 基线在 6.1~12.4 M/s 之间波动）。因此**所有结论只依据同一次交替调用内的
  相对比较与逐轮胜负次数**，绝对值仅代表当时的运行环境。表中给出的一轮里，基线 `sync_mt`
  为 34.7 M/s，与此前各轮记录（33.9~34.5）及第三轮报告（34.09）一致。
- 计数吞吐：`chlog_bench_loggers --iters 2000000`；
  异步多生产者区分度低，另用聚焦驱动 `--iters 30000000 --producers 4` 取 9 轮。

## 计数吞吐（百万条/秒，中位数）

| 场景 | 基线 | 本轮 | 变化 | 胜负 |
|---|---:|---:|---:|---:|
| 单线程格式化（`sync_st`） | 35.14 | 60.01 | **+70.8%** | 5/0 |
| 线程安全同步格式化（`sync_mt`） | 34.72 | 59.60 | **+71.7%** | 5/0 |
| 并行 sink，1 个 sink（默认配置） | 35.13 | 60.14 | **+71.2%** | 5/0 |
| 并行 sink，2 个 sink | 5.38 | 7.98 | **+48.3%** | 5/0 |
| 并行 sink，4 个 sink | 7.57 | 7.89 | +4.3% | 5/0 |
| 异步格式化，单生产者 | 8.14 | 11.81 | **+45.1%** | 5/0 |
| 异步格式化，四生产者 | 12.41 | 12.00 | −3.3% | 1/4 |
| 单线程纯文本 | 113.35 | 121.05 | +6.8% | 4/1 |
| 线程安全同步纯文本 | 113.21 | 117.21 | +3.5% | 5/0 |
| 异步纯文本 | 13.26 | 13.44 | +1.4% | 4/1 |
| 单/多线程 128 B 正文 | 206.17 / 203.24 | 203.78 / 207.29 | −1.2% / +2.0% | 2/3 / 2/3 |
| 单/多线程 1024 B 正文 | 209.20 / 203.52 | 205.98 / 201.63 | −1.5% / −0.9% | 2/3 / 1/4 |
| 异步 128 B / 1024 B 正文 | 6.73 / 6.46 | 7.08 / 6.65 | +5.2% / +2.9% | 5/0 / 5/0 |
| `filtered_out`（<1 ms，不参与结论） | 3007 | 4457 | +48.2% | 4/1 |

噪声底（同一二进制与自身副本）：`sync_mt` +0.9%（6/5）、128 B 正文 ±2%（2/3~3/2）、
异步四生产者 −0.2%~−1.0%（5/8、5/4）。正文类用例两侧胜负都在半数附近，属布局噪声。

## 内存与其它基准

- `chlog_render_bench`：端到端 `sync_pattern` 0.0299 s → 0.0247 s（**+17.3%**，5/0）、
  `async_pattern` 0.0221 s → 0.0197 s（**+10.7%**，5/0）。
  `RENDER`/`BUFFER` 各项 ±9% 且胜负混杂（这些路径不经过本轮代码）。
- **分配量完全不变**：`MEMORY allocated_bytes` 两侧同为 4,736,064 字节；
  `ASYNC_NAME` 两侧同为 0 次分配。快速路径本身不新增分配
  （仅一个 72 字节栈数组，以及与原路径同样的一次 `std::string`）。
- `chlog_single_thread_bench`：38.55 → 62.25 M/s（**+61.5%**，5/0）。
- `chlog_bench_output`（文件写入受限）：±5%，`sync_st_pattern_1024` +3.5%（5/0）、
  `async_mt_message_1024` +4.8%（4/1）、`sync_st_message_128` −2.9%（0/5）。
  这些用例走的是既有的 `plain_string_argument` 快捷分支，不执行本轮代码，
  差异落在 I/O 与布局噪声内（同一用例在计数吞吐基准上另测为 +2.0%/+2.8%）。

## 与 spdlog 对比（参考，单轮）

本机装有 spdlog 1.17.0，`chlog_bench_loggers` 的 fmt 后端构建可直接对比
（`--iters 2000000`，各跑一轮，仅作量级参考）：

| 场景 | chlog | spdlog | 倍数 |
|---|---:|---:|---:|
| 单线程格式化 | 65.35 | 39.82 | 1.6× |
| 线程安全同步格式化 | 66.25 | 39.54 | 1.7× |
| 异步格式化，单生产者 | 11.04 | 5.65 | 2.0× |
| 异步格式化，四生产者 | 10.08 | 2.86 | 3.5× |
| 单线程纯文本 | 125.20 | 50.07 | 2.5× |
| 单线程 128 B 正文 | 204.44 | 19.31 | 10.6× |
| 异步 1024 B 正文 | 5.99 | 1.05 | 5.7× |
| 级别过滤（`filtered_out`） | 2293.8 | 522.7 | 4.4× |

两者 sink 语义不完全相同（chlog 的视图 sink 通过 `log_event_view` 消费，
spdlog 的计数器 sink 仍走 `log_msg`），倍数只说明量级。

## 已测量后回退的改动

1. **`push_blocking` 加入有界自旋（16 次）再休眠。** 异步四生产者场景下生产者会反复
   满队——先尝试原地自旋吸收一批释放，再进入 `space_epoch.wait`（代际在最后一次尝试前读取，
   不会丢失唤醒）。实测 `async_4p` −3.5%（0/13），与改动前的 −4.2%（1/12）相比没有任何改善，
   单/双生产者也没有变化，因此**回退**，不留复杂度和维护面。
2. **让线程池规模跟随 sink 数量（"每个 sink 一个 worker"）。** 观察：池在第二个 sink 注册时
   启动，`sink_pool_size = 0` 实际只会得到 2 个 worker。先用上限实验确认收益再决定：
   4 个 sink 下 worker 数 2/2/4/6/8 对应 8.27/8.03/5.48/0.98/0.24 M/s——worker 越多越差，
   且基线同形（7.61/7.88/3.51/1.03）。因此**不改**，并把"池规模不随 sink 数增长"记为既有特性，
   而不是本轮引入的问题。

## 异步四生产者 −3.3%：定位与结论

这是本轮唯一一处负向数据，做了三组控制实验来定性：

| 实验 | 结果 | 排除的解释 |
|---|---|---|
| 队列容量 64 Ki / 1 Mi / 16 Mi | −4.1% / −4.0% / −4.3% | 与背压无关（容量 16 Mi 时队列不会满） |
| 格式串换成 `"{:d}"`（快速路径被实例化但每次都拒绝，代码规模与新版一致） | **+0.4%（6/3）** | 与代码规模/布局无关 |
| 生产者数 1 / 2 / 4（30 M 条，9 轮） | +21.0%（9/0）/ +5.0%（9/0）/ **−2.8%（1/8）** | 见下 |

噪声底为 −0.7%（3/6）。结论：该配置下总吞吐由**共享的分派路径**决定——生产者更快返回，
只会更频繁地回到同一个有界环与通知路径上，于是生产者侧的节省无法体现，只剩共享部分的
微小抖动。单/双生产者分别 +21.0% / +5.0%，同一份代码在异步路径上净收益为正。
真正的改进需要改约束环的争用协议（例如批量提交或多级通知），属于独立议题，本轮不动。

## 行为边界

- 快速路径只覆盖"字面量 + 裸 `{}`"且参数类型在可证等价集合内的调用；其余一律走通用格式化器，
  输出与诊断逐字不变。
- 非有限浮点（`inf`/`nan`）由通用格式化器输出；`to_chars` 会带 NaN 载荷，故不作等价替换。
- 空 C 字符串指针被拒绝而不是解引用（`std::format` 下是未定义行为，fmt 会打印 `(null)`）。
- 运行时格式串同样享受快速路径；格式串每次调用重新扫描，不对其生命周期做任何假设。
- 未改变任何公开签名与配置项；未使用 CPU 特异性指令或内联汇编
  （`std::to_chars`、`std::isfinite` 均为标准库设施）。

## 复现入口

```sh
# 受控（std::format）基线与本轮对比构建
git archive HEAD | tar -x -C /tmp/chlog-baseline          # 基线源码
cmake -S /tmp/chlog-baseline -B /tmp/chlog-baseline/b-std -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build /tmp/chlog-baseline/b-std -j 16
cmake -S . -B /tmp/chlog-baseline/b-new-std -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build /tmp/chlog-baseline/b-new-std -j 16

# 回归矩阵
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release && ctest --test-dir build-release --output-on-failure
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure

# 压力重复（CTest 的耗时汇总会误导，用通过条数确认重复次数）
ctest --test-dir build-tsan -R "queue|async|parallel|concurrent|registration|fastformat" \
  --repeat until-fail:10
```

本轮验证在 macOS（Apple Silicon，ARM64）与 Apple clang 17 上完成；
未重新运行 Windows 与 Linux 的编译、测试与测量，上一轮的相应结论仍以当时构建为准。
