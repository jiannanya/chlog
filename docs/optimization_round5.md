# chlog 第五轮优化与验证：内联路径的缓冲与写入

日期：2026-09-29。基线：第四轮结束态（`include/chlog/chlog.hpp` = `1243d4e1…`，即 `docs/optimization_round4.md` 的产物）。
本轮继续压缩**内联投递路径**（调用线程直接写 sink 的那条）的单条记录开销与堆分配，
并顺带修正 sink 侧渲染的小块拷贝。保持调用接口、过滤语义、错误处理、异步/并行分派与关闭契约不变；
不含任何 CPU 特异性指令或内联汇编。

## 定位：第四轮之后，剩下的就是缓冲本身

对 `sync_mt`（`info("v {}", i)`、`{msg}`、视图 sink）重新采样，第四轮结束时自耗时分布为：

| 帧 | 第四轮结束 | 第五轮结束 |
|---|---:|---:|
| `std::basic_string::append`（含 PLT 存根） | **33.0%** | 已消除 |
| `_platform_memmove` + `memcpy` 存根 | **17.8%** | 已消除 |
| `append_bare_value` | 27.5% | 30.8% |
| `dispatch_view`（登录守卫 + 遍历 + 虚调用） | 24.8% | 24.9% |
| `__itoa::__base_10_u32` | 8.5% | 13.0% |

即：格式化器被绕开之后，**记录缓冲自身**成了主体——每条记录两次 `std::string::append`
（`libc++` 的 string 实现在系统 dylib 里，每次都要过一次 PLT）加两次小块 `memcpy` 调用。
上限实验（同机 `-O2`，模拟 `"v {}"` 追加一个字面量与一个整数）：

| 写法 | ns/次 |
|---|---:|
| 两次 `memcpy` + `to_chars` 进栈缓冲再拷贝 | 3.52 |
| 小块拷贝改内联循环 | 2.84 |
| **`to_chars` 直接写进目标缓冲** | **1.52** |

## 实现变化

1. **内联路径改用栈上的 `text_buffer`，不再构造 `std::string`。**
   新增 `submit_bare()`：只有在"格式串落在受限语法内且实参类型可证等价"时才成立，
   随后用 `detail::text_buffer`（512 字节内联存储）直接渲染并投递视图。
   原 `std::string` 只剩在**通用格式化器兜底**时构造，避免为一次 20 字节的写入付一次堆分配。
   为保持每记录只进入一次 `log_guard`（否则 `stats().enqueued` 会重复计数），
   把"判定"与"渲染"拆开：计划与实参校验先做，通过后才取守卫，拒绝时一个字节都没写。

2. **数字直接写进目标缓冲（`reserve_tail` / `commit_tail`）。**
   `text_buffer` 暴露"可写尾部"接口，`append_bare_value` 对算术类型直接在该位置调用
   `std::to_chars`，省掉一次栈缓冲和一次拷贝（上限实验里这一步占收益的大头）。
   通过 `void_t` 检测该接口是否存在，因此**通用形态（`std::string` 等）行为不变**，
   仍走"栈缓冲 + 追加"的分支。

3. **短拷贝内联。** 新增 `copy_bytes`：≤16 字节用循环内联拷贝，更长仍交给 `memcpy`。
   记录渲染本质上是大量小块拷贝（字面量段、分隔符、数字），
   在动态链接目标上"调用 + 惰性绑定存根"比它搬运的那几个字节更贵。
   该函数用于 `text_buffer::append` 与 `grow`，因此**所有 sink**（控制台/轮转/按日/JSON）同时受益。

4. **fmt 后端不参与浮点快速路径（本轮发现的真实分歧）。**
   加测 fmt 后端时 `fastformat` 套件报出一处不一致：

   | 表达式 | 结果 |
   |---|---|
   | `std::to_chars(-398377568.0f)` | `-398377568` |
   | `std::vformat("{}", v)` | `-398377568` |
   | `fmt::format("{}", v)` | `-3.9837757e+08` |

   fmt 的默认浮点格式与 `to_chars` 的 general 形式不同（该走科学计数法时它会切换），
   而只有标准格式化器把空格式说明定义为"最短往返的 `to_chars` 形式"。
   因此 `CHLOG_USE_FMT` 下浮点参数一律交回通用格式化器（整数、`bool`、`char`、字符串不受影响），
   并在测试里用 `static_assert` 断言该策略、同时断言**在线格式化器仍然给出 fmt 自己的拼写**。

5. **测试与验证矩阵补上 fmt 后端。** 新增 `-DCHLOG_USE_FMT=ON` 的 Release 测试构建；
   上面的分歧就是它查出来的。`fastformat` 套件里浮点一节改为后端相关：
   标准后端逐字节比对 40000 个随机位模式，fmt 后端断言浮点被拒并比对通用格式化器的输出。

## 性能

测量均在受控 std::format 构建（`-DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON
-DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON`）下进行；基线为第四轮源码（`/tmp/chlog-r4`）编译的同一份驱动。

### 聚焦驱动（同一二进制对，段内即时交替，9 轮中位数）

| 场景 | 第四轮 | 第五轮 | 变化 | 胜负 |
|---|---:|---:|---:|---:|
| 单线程格式化 | 58.34 | 79.23 | **+35.8%** | 9/0 |
| 线程安全同步格式化 | 57.21 | 76.45 | **+33.6%** | 8/1 |
| 并行 sink，2 个 | 6.96 | 7.77 | +11.6% | 8/1 |
| 并行 sink，4 个 | 8.01 | 8.06 | +0.7% | 5/4 |
| 单线程 128 B 正文 | 188.5 | 173.3 | −7.8% | 0/13 |
| 线程安全 128 B 正文 | 183.6 | 171.2 | −6.8% | 0/13 |

（单位百万条/秒。并行 sink 走的是**持有型**路径：`format_payload` + `std::string`，
本轮没有改动它，因此 2/4 sink 的变化在噪声/布局量级。）

### 计数吞吐（`chlog_bench_loggers --iters 2000000`，6 轮）

| 场景 | 基线 | 本轮 | 变化 | 胜负 |
|---|---:|---:|---:|---:|
| 单线程格式化 | 57.94 | 82.37 | **+42.2%** | 6/0 |
| 线程安全同步格式化 | 59.78 | 83.71 | **+40.0%** | 6/0 |
| 并行 sink，1 个（默认配置） | 60.28 | 83.94 | **+39.2%** | 6/0 |
| 并行 sink，2 个 | 7.48 | 10.25 | +37.0% | 6/0 |
| 并行 sink，4 个 | 5.60 | 7.41 | +32.4% | 6/0 |
| 异步格式化，单生产者 | 7.99 | 7.79 | −2.5% | 3/3 |
| 异步格式化，四生产者 | 6.44 | 6.44 | 0.0% | 3/3 |
| 单线程纯文本 | 114.34 | 116.57 | +2.0% | 4/2 |
| 线程安全同步纯文本 | 117.24 | 117.80 | +0.5% | 2/4 |
| 异步纯文本 | 9.33 | 8.87 | −4.9% | 2/4 |
| 单线程 128 B / 1024 B 正文 | 205.05 / 212.94 | 194.17 / 193.61 | −5.3% / −9.1% | 0/6 / 1/5 |
| 线程安全 128 B / 1024 B 正文 | 196.81 / 202.32 | 193.65 / 191.07 | −1.6% / −5.6% | 3/3 / 1/5 |
| 异步 128 B / 1024 B 正文 | 5.25 / 4.89 | 5.16 / 4.75 | −1.7% / −2.8% | 3/3 / 4/2 |
| `filtered_out`（<1 ms，不参与结论） | 2709 | 1052 | −61.2% | 0/6 |

> 该轮里基线的绝对值整体偏低（例如并行 4 sink 基线 5.60，此前多轮为 6.7~8.0），
> 使并行各项的百分比偏乐观；**逐用例结论以聚焦驱动表为准**。
> 同一二进制的自比噪声底（6 轮）：格式/纯文本/正文各用例 |Δ| ≤ 3%，
> 异步纯文本 +13.6%（5/1，该用例本身极不稳），`filtered_out` −25.8%（0/6，该用例不可用）。

### 渲染与内存

| 项 | 基线 | 本轮 | 变化 | 胜负 |
|---|---:|---:|---:|---:|
| `BUFFER [{date}…] p128` | 0.0059 s | 0.0045 s | **+25.1%** | 5/0 |
| `BUFFER [{date}…] p1024` | 0.0116 s | 0.0101 s | **+13.1%** | 5/0 |
| `MEMORY allocated_bytes`（异步队列） | 4736064 | 4736064 | 0 | — |
| `ASYNC_NAME allocations` | 0 | 0 | 0 | — |

前两项正是 `copy_bytes` 的目标形态（默认模板 + 多个小块字面量追加进 `text_buffer`）；
同参数的噪声底为 ±3%（`BUFFER [{date}…]` 两例在噪声底里分别是 −0.3% / −0.7%）。
`chlog_bench_output`（文件写入受限）与其余 `RENDER/BUFFER` 项噪声底达 ±10~18%，不作结论。

### 每记录堆分配（替换全局 `operator new` 计数，内联路径，1000 条记录取平均）

| 记录字节数 | 第四轮 分配次数 / 请求字节 | 第五轮 分配次数 / 请求字节 |
|---:|---:|---:|
| 7 / 16 | 0 / 0 | 0 / 0 |
| 32 | 1 / 48 | **0 / 0** |
| 64 | 1 / 72 | **0 / 0** |
| 128 | 1 / 136 | **0 / 0** |
| 256 | 1 / 264 | **0 / 0** |
| 512 | 1 / 520 | 1 / 768 |
| 1024 | 1 / 1032 | 1 / 1090 |
| 4096 | 1 / 4104 | 1 / 4162 |

内联路径的免分配上限从 `std::string` 的 22 字节（SSO）提高到 `text_buffer` 的 512 字节，
即**常见长度的日志行在同步内联路径上完全不再分配**；超过 512 字节时分配次数不变，
请求字节数因 1.5× 增长策略略高（512→768、1024→1090），随记录结束即刻归还。

## 正确性验证

| 构建配置 | 结果 |
|---|---|
| macOS Release（Apple clang 17，`std::format`） | 10/10 通过 |
| macOS Release（`-DCHLOG_USE_FMT=ON`，fmt 12.1.0） | 10/10 通过，零告警 |
| macOS ASan + UBSan（RelWithDebInfo） | 10/10 通过，无报告 |
| macOS ThreadSanitizer（RelWithDebInfo） | 10/10 通过，无报告 |
| Release 6 套件重复 10 次 | 60/60 通过 |
| TSan 6 套件重复 10 次 | 60/60 通过 |
| ASan 5 套件重复 5 次 | 25/25 通过 |
| fmt 后端 4 套件重复 5 次 | 20/20 通过 |

`fastformat` 套件在**两个后端**都运行；浮点一节按后端分支（见实现变化 4）。
新增的 fmt 测试构建是本轮唯一发现真实分歧的手段，已纳入常规验证矩阵。

## 接受的一处代价：正文用例 −5%~−9%

`info("{}", payload)`（单个字符串实参、128/1024 字节）读数为 −5%~−9%（0/6、1/5），
聚焦驱动复核为 −7.8% / −6.8%（0/13、0/13），即约 **0.4 ns/条**。该用例**不执行本轮新代码**：
`"{}"` 走的是既有的零拷贝快捷分支。二分定位：

| 变体 | 128 B 正文 | 结论 |
|---|---:|---|
| 第四轮 | 188.5 | — |
| 本轮 | 173.3（−7.8%，0/13） | — |
| 本轮 + `submit_bare` 标记 `noinline` | 173.9（−7.2%，0/13） | 与内联函数体无关 |
| 本轮去掉快速路径调用点（其余保留） | 192.9（+2.3%，6/5） | **回到持平** |
| 同二进制自比噪声底 | — | ±1.4%（3/3） |

结论：代价来自 `submit_format` 为字符串实参实例化时**多出的那次快速路径判定分支**
（死代码，因其后紧跟的零拷贝分支必然先返回），属于代码布局效应，与执行路径无关。
判断：主路径 +34%~+42%、该用例 −0.4 ns（真实目的地是控制台/文件 sink，那里记录无论如何都要被渲染或落盘），
收益远大于此代价，**保留**。更激进的规避（例如把快捷分支上提到 `log()`）会把同一问题搬到别的函数，
不值得为此改动调用结构。

## 已测量后回退

- **`submit_bare` 加 `noinline`**：格式化 −0.1%（4/5，与内联版持平），正文用例仍 −7.2%，
  即对两个目标都无收益，**回退**（也不引入编译器属性宏）。

## 行为边界

- 快速路径与第四轮同样只覆盖"字面量与裸 `{}` 交替"且实参类型可证等价的调用；
  其余（格式说明、定位下标、转义花括号、类型不符、空 C 字符串指针、非有限浮点）一律交回通用格式化器。
- **fmt 后端不参与浮点快速路径**（`std::format` 与 `fmt` 对空格式说明的浮点拼写不同）。
- 内联路径的记录缓冲改为 `text_buffer`（512 字节内联，超出后按 1.5× 增长后释放）；
  该缓冲仍是**每记录局部**对象，不引入线程局部状态或跨记录保留。
  （**第六轮更新**：超出 512 字节的块自第六轮起由每线程空闲表保留复用，见
  `docs/optimization_round6.md`。）
- 未改变任何公开签名与配置项；`copy_bytes`/`reserve_tail`/`commit_tail` 均为内部细节。

## 复现入口

```sh
# 受控构建（std::format，关掉本机 Homebrew 的 fmt/spdlog）
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build build-release && ctest --test-dir build-release --output-on-failure

# fmt 后端回归（本轮新增，用于检出后端相关分歧）
cmake -S . -B build-fmt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHLOG_USE_FMT=ON \
  -DCHLOG_BUILD_BENCHMARKS=OFF -DCHLOG_BUILD_EXAMPLES=OFF
cmake --build build-fmt && ctest --test-dir build-fmt --output-on-failure

# sanitizer 与压力重复
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
ctest --test-dir build-tsan -R "queue|async|parallel|concurrent|registration|fastformat" \
  --repeat until-fail:10
```

A/B 复现：把第四轮头文件单独导出到 `/tmp/chlog-r4/inc`，与本轮头文件各编译同一份驱动，
**每轮交换两侧执行顺序**（ABBA）后取中位数——本轮实测发现"固定先跑基线"会在负载漂移下
系统性偏袒基线，见 `docs/optimization_round5.md` 的测量说明与记忆中的工具陷阱。

本轮验证在 macOS（Apple Silicon，ARM64）与 Apple clang 17 上完成；
未重新运行 Windows 与 Linux 的编译、测试与测量，上一轮的相应结论仍以当时构建为准。
