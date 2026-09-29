# chlog 第六轮优化与验证：时间戳读取与长记录缓冲复用

日期：2026-09-29。基线：第五轮结束态（`include/chlog/chlog.hpp` = `f0a4c7095e87…`，
即 `docs/optimization_round5.md` 的产物）。本轮头文件 = `55bb6a73063d…`。

本轮处理内联投递路径上剩余的两处**固定开销**：

1. 默认模板（`[{date} {time}.{ms}]…`）下每条记录的挂钟读取；
2. 超过内联区（512 B）的记录，其记录缓冲在每条记录结束时把堆块还给了分配器。

调用接口、过滤语义、错误处理、异步/并行分派与关闭契约均未改动；不含任何 CPU 特异性指令或内联汇编。

## 定位

### 1) 默认模板的记录开销里，挂钟读取是最大单项

同一台机器、`-O2` 下的空循环微基准（`clockbench.cpp`，ns/次）：

| 读取方式 | ns/次 |
|---|---:|
| `std::chrono::system_clock::now()` | 12.43 |
| `std::chrono::steady_clock::now()` | 11.74 |
| `clock_gettime_nsec_np(CLOCK_REALTIME)` | 9.01 |
| `::gettimeofday` | 7.45 |
| `std::this_thread::get_id()` | 1.12 |

基线的 `sync_mt_pattern` 是 44.5 M 条/秒，即**每条记录 22.5 ns**，其中 12.4 ns 花在 `system_clock::now()` 上。
之前的实验已排除其它变体（`noinline` 包装 +17.4%/+16.1%，`clock_gettime_nsec_np` +8.3%/+7.5%，均低于下面采用的写法）。

### 2) 长记录：逐条分配 + 逐条释放

`detail::text_buffer` 有 512 B 内联区；更长的记录（长正文、默认模板、JSON）会 `new char[]`，
记录结束即 `delete[]`。三个使用点都如此：日志侧 `submit_bare` 的渲染缓冲、sink 侧
`consume()`/`buffered()` 的逐条行缓冲、以及 JSON 渲染。替换全局 `operator new` 的探针
（`allocprobe.cpp`，2000 条记录）显示，长记录是**每条记录一次分配**，而记录形状在同一线程上通常不变。

## 实现变化

1. **`detail::wall_now()`（实验 D）。** Apple 平台直接调用 `::gettimeofday`，
   其余平台仍走 `std::chrono::system_clock::now()`：

   ```cpp
   inline std::chrono::system_clock::time_point wall_now() noexcept {
   #if defined(__APPLE__)
       ::timeval tv{};
       ::gettimeofday(&tv, nullptr);
       return std::chrono::system_clock::time_point(std::chrono::duration_cast<
           std::chrono::system_clock::duration>(std::chrono::seconds{tv.tv_sec} +
                                                 std::chrono::microseconds{tv.tv_usec}));
   #else
       return std::chrono::system_clock::now();
   #endif
   }
   ```

   只替换 `dispatch_view()` 与 `submit()` 两个热点的 `e.ts = …`；`daily_file_sink` 构造等非热路径不动。
   **语义不变**的依据（本机 libc++）：`system_clock::period = 1/1000000`、`duration` 8 字节，
   `system_clock::now()` 就是同一口钟；20 万次交叉采样中 `wall_now()` 与前后两次
   `system_clock::now()` 的差值为 `0 ns`（195851 次）或 `1000 ns`（4147 次，即两次读取之间跨过一个微秒刻度），
   没有偏移也没有粗化。

2. **每线程块空闲表（`block_list` / `local_blocks()` / `recycle_block()`）。**
   `text_buffer` 析构时不再 `delete[]` 堆块，而是交给线程局部的 2 槽空闲表；`grow()` 先向空闲表要块。
   - **所有权独占**：表里只放没有活缓冲指向的块。sink 在记录渲染途中再打日志（嵌套）时，第二个缓冲取不到
     在用的块，只会新分配，因此不存在别名。
   - **上限固定**：每线程最多留 2 个块、每块 ≤ `block_retain_limit = 8192` 字节；更大的块立即释放。
   - **best fit + 淘汰**：取最小的可用块；表满时，仅当来者比表内最小块更大才替换它。
   - **`take()` 只按"实际所需字节数"匹配**，不按带 64 字节余量的目标容量匹配。这是本轮发现并修掉的一处抖动：
     记录只要长一个字节（`{seq}` 多一位数字），带余量的请求就会拒绝驻留块，于是每条记录重新分配、
     旧块驻留——`chlog_render_bench` 的 `{json}` p1024 在 10 万条里出现 4 次分配，正是 4 个位数进位点。
   - **`recycle_block()` 强制不内联**（MSVC 用 `__declspec(noinline)`，GCC/Clang 用 `__attribute__((noinline))`，
     其它编译器退化为普通 `inline`）。析构函数必须保持小到能被调用方内联为一次 `heap_` 判空；
     若把空闲表逻辑（连同 `thread_local` 守卫）留在析构函数体内，编译器会把析构函数整个外提，
     **每条记录多一次函数调用**——这一点在反汇编里直接可见，见"已测量后回退"。
   - **`local_blocks_alive()` 守卫**：一个平凡析构的 `thread_local bool`。线程内某个 `thread_local` 对象
     若先于空闲表构造，则会在空闲表析构之后才析构；此时它若打一条长记录，`grow()`/`recycle_block()`
     会看到"表已不在"，改为直接分配/直接释放，而不是写入已析构对象。

3. **测试：新增 `reuse` 套件**（`tests/chlog_tests.cpp` + `tests/CMakeLists.txt`）。
   覆盖：同一长度重复、跨 1.5× 增长点收缩/扩张、跨 512 B 内联边界、跨 8 KiB 保留上限（8300 B 立即释放）、
   带字面量前缀的模板（强制 `grow()` 拷贝已写字节）、嵌套 sink（两个缓冲同时存活）、4 线程各留各的块、
   以及 thread_local 析构晚于空闲表的情形。

4. **基准契约更新：`benchmarks/render_bench.cpp`。** 该基准原先断言"p128 零分配、p1024 每次调用一次分配"，
   并把不满足当作失败退出。本轮起稳态契约变为**零分配**（前置一次预热，让首个长记录的 1~N 次增长移出测量窗口），
   断言相应改为"稳态分配 = 0"。检查放在打印之后，因此旧行为（每次调用分配）仍能输出自己的数字。

## 性能

测量条件：Apple M4 Max（16 核）、macOS 26.1、Apple clang 17；`-std=c++20 -O2 -DNDEBUG`；
受控 `std::format` 构建（`-DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON`）。
机器同期负载较高（load 8~16），因此全部结论以**同段交替（ABBA）的相对值 + 胜负计数**为准，
并给出布局对照与逐指令比对。

### 聚焦驱动（同一二进制对，段内即时交替，9 轮中位数，百万条/秒）

| 场景 | 第五轮基线 | 本轮 | 变化 | 胜负 |
|---|---:|---:|---:|---:|
| 默认模板，线程安全同步（`sync_mt_pattern`） | 44.50 | 53.22 | **+19.6%** | 9/0 |
| 默认模板，持有型 sink（`owning_pattern`） | 40.21 | 46.89 | **+16.6%** | 9/0 |
| 长正文 1024 B，内联路径（`prefix_payload1024`） | 27.53 | 39.80 | **+44.6%** | 9/0 |
| 长正文 4096 B，内联路径（`prefix_payload4096`） | 14.62 | 18.78 | **+28.4%** | 9/0 |
| 长正文 16384 B（超过保留上限，对照组） | 4.515 | 4.459 | −1.2% | 3/6 |
| 线程安全同步 `{msg}`（`sync_mt`） | 83.05 | 81.90 | −1.4% | 2/7 |
| 单线程 `{msg}`（`sync_st`） | 83.01 | 80.73 | −2.8% | 3/6 |
| 持有型 sink `{msg}`（`owning`） | 62.13 | 61.24 | −1.4% | 0/9 |
| 正文 128 B（`payload128`） | 176.95 | 171.66 | −3.0% | 2/7 |
| 正文 128 B + 前缀（`prefix_payload128`） | 88.10 | 86.06 | −2.3% | 2/7 |
| 异步队列（`async`） | 12.095 | 12.101 | 0.0% | 7/2 |
| 并行 sink ×2 / ×4 | 9.39 / 9.78 | 9.76 / 9.80 | +3.9% / +0.2% | 6/3 / 4/5 |

`prefix_payload16384` 是**对照组**：记录所需块超过 8192 B，保留策略不生效，所以两版之间不应有差别
（实测 −1.2%，胜负 3/6）。它是"收益来自复用而非布局"的直接证据。

收尾（注释修订后的同一份源码，5 轮复测）与上表一致：`sync_mt_pattern` +20.1%（5/0）、
`owning_pattern` +15.7%（5/0）、`prefix_payload1024` +44.2%（5/0）、`prefix_payload4096` +27.0%（5/0）、
`sync_mt` +1.5%、`sync_st` +1.1%（后两项再次换向，进一步说明其量级属于布局噪声）。

### 布局对照与逐指令比对（解释小节那里的 1~3% 负值）

`{msg}` 各用例（以及 128 B 正文）**执行的工作没有变化**：这些记录始终停在内联区，空闲表与时钟都不参与。
逐指令比对（`otool -tV` 规范化后 diff，同一份驱动）：

| 函数 | 基线指令数 | 本轮指令数 | 差异 |
|---|---:|---:|---|
| `logger::dispatch_view` | 153 | 153 | **逐条相同** |
| `logger::log<…>` | 62 | 62 | 仅 `adrp` 页引用 |
| `emit_bare_field<text_buffer,…>` | 155 | 155 | 仅 `adrp` 页引用 |
| `submit_bare<unsigned long long>` | 214 | 208 | 寄存器分配不同，**少 6 条** |
| `logger::submit<…>` | 332 | 340 | 仅时钟替换（`bl system_clock::now` → `bl gettimeofday` + timeval 拆装） |

也就是说这几条用例的负值来自**函数入口对齐/代码布局**：同代码、地址不同。

- 对照 A（同一二进制自比噪声底 7 轮）：`sync_st` +0.5%（7/0）、`sync_mt` −1.0%（3/4）。
- 对照 B（把两侧都按 `-falign-functions=64` 编译，9 轮）：

| 场景 | 基线(64) | 本轮(64) | 变化 | 胜负 |
|---|---:|---:|---:|---:|
| `sync_mt` | 80.48 | 82.12 | +2.0% | 7/2 |
| `sync_st` | 80.54 | 82.90 | +2.9% | 9/0 |
| `owning` | 59.54 | 61.03 | +2.5% | 9/0 |
| `payload128` | 174.21 | 173.69 | −0.3% | 4/5 |
| `prefix_payload128` | 87.86 | 86.93 | −1.1% | 3/6 |
| `sync_mt_pattern` | 44.25 | 52.70 | **+19.1%** | 9/0 |
| `prefix_payload1024` | 27.54 | 39.73 | **+44.3%** | 9/0 |

固定入口对齐后，`{msg}` 那几项的负值消失（三项转为 +2.0%~+2.9%），两个主收益基本不动
（+19.1% / +44.3%）。对照 C 是上一轮的同类结论：仅改 `-falign-functions` 就能让同一用例在 −2.6%~+2.5% 间移动。

### 渲染台架（`chlog_render_bench 100000`，同一对二进制各跑 3 次取中位数）

| 模板 | 正文 | 基线时间(s) | 本轮时间(s) | 变化 | 基线分配次数 | 本轮分配次数 |
|---|---:|---:|---:|---:|---:|---:|
| `{msg}` | 128 B | 0.000536 | 0.000506 | −5.6% | 0 | 0 |
| `{msg}` | 1024 B | 0.003180 | 0.001711 | **−46.2%** | 100000 | **0** |
| 默认模板 | 128 B | 0.002097 | 0.002123 | +1.3% | 0 | 0 |
| 默认模板 | 1024 B | 0.005215 | 0.004040 | **−22.5%** | 100000 | **0** |
| `{json}` | 128 B | 0.004738 | 0.004733 | −0.1% | 0 | 0 |
| `{json}` | 1024 B | 0.015777 | 0.014238 | **−9.8%** | 100000 | **0** |

（这是共享基准的单次场景对比，绝对值受本机负载影响 ±10% 量级；分配次数是确定值。）

### 每记录堆分配（替换全局 `operator new` 计数，2000 条记录，`allocprobe.cpp`）

| 模式（缓冲区位置） | 正文 | 基线 分配次数 / 请求字节 | 本轮 |
|---|---:|---:|---:|
| 视图 sink + 前缀（日志侧 `submit_bare` 缓冲） | 512 | 2000 / 1,536,000 | **0 / 0** |
| 同上 | 1024 | 2000 / 2,180,000 | **0 / 0** |
| 同上 | 4096 | 2000 / 8,324,000 | **0 / 0** |
| 同上 | 8300（> 保留上限） | 2000 / 16,732,000 | 2000 / 16,732,000 |
| 文件 sink（sink 侧行缓冲） | 64 | 0 / 0 | 0 / 0 |
| 同上 | 1024 | 2000 / 2,274,000 | **0 / 0** |
| 同上 | 4096 | 2000 / 8,418,000 | **0 / 0** |
| 同上 | 8300（> 保留上限） | 2000 / 16,826,000 | 2000 / 16,826,000 |

超出保留上限时行为与基线一致（立即释放），这是保留策略的边界对照组。

### 内存与既有指标（同一构建配置下逐项比对）

| 项 | 基线 | 本轮 |
|---|---:|---:|
| `MEMORY queue_capacity=65536 allocated_bytes` | 4736064 | 4736064 |
| `MEMORY … allocations` | 10 | 10 |
| `ASYNC_NAME allocated_bytes / allocations / processed` | 0 / 0 / 100001 | 0 / 0 / 100001 |
| `REGISTRATION`（16/256/1024 sink）| 2656/47、42976/767、172000/3071 | 完全相同 |
| `SIZES`（logger/sink/config/metrics） | 184 / 56 / 128 / 128 | 完全相同 |
| `chlog_bench_allocations`（backlog / sync_st / sync_mt / async / file_st / objects_st） | — | 逐字段相同 |

### 线程退出与守卫（替换 `operator new/delete` 计数探针，`threadprobe.cpp`）

| 场景 | 基线 | 本轮 |
|---|---|---|
| 工作线程记 4 条 2000 B 记录后退出 | 7 次分配 / 7 次释放 | **4 / 4**（4 条记录只分配 1 个块，线程退出时该块被释放） |
| 主线程记 4 条，进程退出前 / `atexit` 后 | 4 / 4 → 4 / 12 | **1 / 0 → 1 / 3**（保留块由主线程 TLS 析构释放） |
| thread_local 析构晚于空闲表（有守卫 / 无守卫） | — | 释放 5 次 / **释放 4 次（漏掉一个块）** |

## 正确性验证

| 构建 | 结果 |
|---|---|
| Release（`std::format` 后端） | 11/11 通过：`formatting`、`fastformat`、`queue`、`async`、`parallel`、`files`、`concurrent`、`configuration`、`registration`、**`reuse`**、`package` |
| Release，`--repeat until-fail:5` | 11/11 全通过 |
| Release + `-DCHLOG_USE_FMT=ON` | 11/11 通过 |
| ASan + UBSan（RelWithDebInfo） | 11/11 通过；`-R "reuse\|concurrent\|parallel" --repeat until-fail:5` 通过 |
| TSan | 11/11 通过 |

补充说明：

- 本机 Apple clang 的 ASan **不支持 LeakSanitizer**（`ASAN_OPTIONS=detect_leaks=1` 会直接 abort，
  报 "detect_leaks is not supported on this platform"）。因此泄漏面改用替换 `operator new/delete`
  的计数探针验证，结论见上表：线程退出与进程退出时"保留块"都被释放，`allocations == frees`。
- 无守卫版本在同样的"thread_local 析构晚于空闲表"场景下少释放一个块（真实泄漏），
  守卫把它变为直接释放；该场景同时是"向已析构对象写入"的未定义行为，守卫一并消除。
- 守卫只覆盖本轮新增的线程局部状态；"从析构函数打日志"本身仍要求 logger 与 sink 仍存活
  （这是库的既有前提，与本轮无关）。

## 行为边界

- 复用对内容**不可见**：`reuse` 套件逐条比对渲染结果，覆盖增长/收缩、边界长度、前缀模板、嵌套与多线程；
  该套件对第五轮基线头文件同样通过（它是行为测试，不依赖本轮实现）。
- 每线程最多驻留 2 × 8192 字节，超过即释放；线程退出释放全部驻留块。
- `wall_now()` 只在 Apple 上换成 `gettimeofday`；其它平台仍是 `system_clock::now()`，
  时间戳来源与分辨率不变（同钟、同微秒刻度）。
- 新的线程局部状态是标准 C++ `thread_local`；平台相关部分仅限 `#if defined(__APPLE__)` 的头文件与调用，
  以及 `noinline` 属性的编译器分支（MSVC / GCC / Clang / 其它），没有 CPU 特异性指令或内联汇编。
- 未改变任何公开签名与配置项；`block_list`、`recycle_block`、`wall_now` 均为内部细节。

## 已测量后回退（本轮）

- **手写十进制渲染**：随机 64 位 `to_chars` 10.77 ns 对分块写法 5.90 ns，但真实取值域（0~2×10⁷）
  `to_chars` 仅 1.65 ns，分块 1.95 ns、`/10` 循环 2.65 ns——实际记录的取值域上 `std::to_chars` 已是最优，回退。
- **把空闲表逻辑直接留在 `text_buffer` 析构函数里**：析构函数因此被外提为函数调用，`{msg}` 用例
  实测 −5.8%/−3.7%（0/9、1/8，且 `prefix_payload128` −6.5%）。改为 `recycle_block` 不内联后，
  同一用例回到 +0.3%/+0.6%（对照 B 下 +2.9%）。这是本轮唯一"先引入再修掉"的代价。
- **`take()` 按带余量的目标容量匹配**：记录每长一个字节就重新分配一次（10 万条 4 次），
  改为按实际所需字节数匹配后稳态归零（见 `{json}` p1024 的分配次数 4 → 0）。
- 上一轮已回退的措施依旧保留结论：`-fno-stack-protector`、惰性/分片环形缓冲分配、
  条件元数据的 `queued_event`、单调钟混合方案、`submit_bare` 的 `noinline`。

## 复现入口

```sh
# 受控构建（std::format，关掉本机 Homebrew 的 fmt/spdlog）
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build build-release && ctest --test-dir build-release --output-on-failure

# fmt 后端回归
cmake -S . -B build-fmt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHLOG_USE_FMT=ON \
  -DCHLOG_BUILD_BENCHMARKS=OFF -DCHLOG_BUILD_EXAMPLES=OFF
cmake --build build-fmt && ctest --test-dir build-fmt --output-on-failure

# sanitizer 与压力重复
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCHLOG_BUILD_EXAMPLES=OFF -DCHLOG_BUILD_BENCHMARKS=OFF
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
ctest --test-dir build-release --repeat until-fail:5

# 渲染/分配指标（需要 fmt + spdlog 才能构建 memory 目标）
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -Dspdlog_DIR=/opt/homebrew/lib/cmake/spdlog -Dfmt_DIR=/opt/homebrew/lib/cmake/fmt
cmake --build build-bench && ./build-bench/chlog_render_bench 100000
./build-bench/chlog_bench_allocations chlog backlog 64
```

R6 的 A/B：把第五轮头文件导出到 `/tmp/chlog-r6-base/chlog.hpp`，与本轮头文件各编译同一份
驱动（`/tmp/chlog-prof/r6.cpp`），**逐段交替（ABBA）**后取中位数与胜负计数；
布局对照用两侧都加 `-falign-functions=64` 的同一对源码编译。
分配计数用替换全局 `operator new/delete` 的探针（`allocprobe.cpp`、`threadprobe.cpp`）。

本轮验证在 macOS（Apple Silicon，ARM64）与 Apple clang 17 上完成；
未重新运行 Windows 与 Linux 的编译、测试与测量，之前各轮的相应结论仍以当时构建为准。
