# 第七轮性能优化与跨平台回归

2026-10-13。基线为 `9a5f81b`，对照使用同一份基准源码分别编译旧、新头文件。
本轮改善了长格式化、交错线程 ID 渲染和密集 JSON 转义，减少按日文件输出的正文复制，
并修复短锁退避与本机标准库格式化的边界问题。没有修改公开 API、配置项或队列生命周期协议。

实现改动：

- `cached_thread_id()` 从单条缓存改为每个渲染线程最多 16 条的工作集，最近命中仍优先检查。
  保留 `std::thread::id` 和标准流的文本表示，不依赖线程 ID 的平台类型、位宽或内部布局。
  超过容量会替换旧条目；32 个 ID 循环访问会持续失配，因此也测量了这个负向边界。
- fmt 同步视图路径的复杂格式缓冲复用现有线程局部块池。小缓冲仍在栈上，
  两个块共享原来的每块 8192 B 保留上限，超大记录立即释放，嵌套格式化各自持有独立缓冲。
  不新增常驻池；线程退出及晚于池析构的日志调用沿用现有生命周期守卫。
- 标准后端复杂格式改用 `std::format` / `std::vformat` 的连续输出，再向 sink 借用结果。
  本机 libc++ 的 `format_to(back_inserter(string), ...)` 在长字段恰好填满内部 256 B 块、
  后接字面量时会越界。新测试对原版头文件也触发 ASan 的 stack-buffer-overflow，
  因此没有保留虽然较快、但仍经过该路径的 `std::string` 缓存实验。
  新实现没有操作标准库私有 API，也没有依赖二次格式化来预估长度；自定义 formatter 只调用一次。
- JSON 的一个转义序列合并为一次追加，保持全部字节、UTF-8 和控制字符的原有输出。
- `daily_file_sink` 的 `{msg}` 直接交给文件行写入器；其他模板仍先渲染。
  空行、内嵌 NUL、4 KiB 边界、日期往返切换和模式切换均有字节级回归验证。
- `spin_mutex` 在实际等待锁的内层循环递增并饱和退避计数。
  原计数仅在外层 CAS 重试时增加，持锁线程被抢占后，等待方可能始终停在空转阶段。
  仍使用标准 C++ 原子与 `std::this_thread::yield()`，没有引入特定 CPU 指令。

测试环境为 macOS / Apple Silicon，Apple clang 17，Release `-O3 -DNDEBUG`。
标准后端与 fmt 12.1.0 分开构建，关闭可选 spdlog 自动发现，避免基准后端被隐式切换。
每项 CPU 用例 500,000 次，文件用例 20,000 条，五轮交替先后顺序，取两侧时间中位数。
吞吐变化为 `基线时间 / 新版时间 - 1`；这些是热点基准，不代表所有应用的整体收益。

| 场景 | std 吞吐变化 | fmt 吞吐变化 | 解释 |
|---|---:|---:|---|
| 裸整数短日志 | -4.1% | -5.2% | 路径工作量未增加，仍需考虑编译布局与计时影响 |
| 复杂格式短日志 | +1.6% | -6.1% | 未证明普遍改善，fmt 此项五轮均较慢 |
| 1 KiB 长字段带格式说明 | +12.8% | +4.2% | std 五轮均较快；fmt 仅 2/5 轮胜出 |
| 运行时复杂格式 | -1.3% | +1.6% | 没有稳定收益 |
| 8 生产者异步完整模板 | -1.6% | +3.2% | 未观察到可靠的整体吞吐提升 |
| 单一线程 ID 查询 | +31.2% | +4.1% | 纳秒级微基准，不应外推为日志吞吐收益 |
| 8 个交错线程 ID 查询 | 47.6 倍 | 42.4 倍 | 避免反复构造流；两侧五轮均胜出 |
| 32 个交错线程 ID 查询 | -3.9% | -6.3% | 超出 16 条缓存，失配查找有额外成本 |
| 1 KiB 普通 JSON 文本 | +0.5% | +2.0% | 基本持平 |
| 1 KiB 密集控制字符转义 | +10.2% | +15.9% | std 5/5、fmt 4/5 轮胜出 |
| 按日文件，128 B | +17.0% | +21.9% | 含 flush 和文件内容验证；受存储负载影响 |
| 按日文件，4096 B | -6.7% | +39.5% | 磁盘波动很大，不据此承诺性能提升 |

原始数据：[std](optimization_round7_std.csv)、[fmt](optimization_round7_fmt.csv)。
额外将两侧同时加 `-falign-functions=64`，五轮对照中 std 长格式化仍提升 9.3%，
密集 JSON 仍提升 10.2%；裸整数差异缩小至 -1.8%，4096 B 文件变为 +2.4%。
这说明小用例对构建布局和磁盘环境敏感，但不构成所有负向差异都属于噪声的证明。
详见[对齐对照原始数据](optimization_round7_aligned.csv)。

既有 `chlog_bench_loggers` 同样跑了五轮交替、每项 500,000 次，并检查 processed/dropped：
`sync_mt` +2.2%、`sync_parallel2` +7.2%、`sync_parallel4` -4.1%、`async_mt` -11.4%、
`async_4p` +4.4%。例如 `async_mt` 基线各轮为 44–78 ms，新版 46–67 ms，分布有明显重叠。
本轮没有证明整个队列或线程池整体更快，保留完整[原始记录](optimization_round7_loggers.csv)，
不把热点微基准结果宣传为全场景提升。

内存诊断使用同一份 `chlog_render_bench`，每种带动态宽度的复杂格式写 2,000 条：

| std 格式化正文 | 基线 C++ 分配次数 / 请求字节 | 新版 C++ 分配次数 / 请求字节 |
|---|---:|---:|
| 64 B | 2000 / 160000 | 2000 / 160000 |
| 1024 B | 6000 / 3696000 | 4000 / 4644000 |
| 16384 B | 14000 / 67056000 | 4000 / 66084000 |

1 KiB 场景减少了分配次数，但请求的累计字节增加约 25.6%；不是所有内存指标都下降。
fmt 的新版 1 KiB 格式缓冲热身后 C++ 分配为零；16 KiB 超出保留上限，仍立即释放。
该探针仅替换 `operator new`，**不统计直接 `malloc`**。fmt 12.1.0 的旧缓冲使用 `malloc`，
所以不能拿旧版输出中的零与新版进行总分配次数比较。
队列初始化仍为 4,736,064 B / 10 次 C++ 分配；异步 logger 名称、sink 注册分配指标均与基线一致。
完整[分配诊断](optimization_round7_allocations.txt)保留了这项统计口径限制。

正确性验证：

| 本机配置 | 结果 |
|---|---|
| Release，std | 12/12 通过 |
| Release，fmt | 12/12 通过 |
| ASan + UBSan，std | 12/12 通过 |
| ASan + UBSan，fmt | 12/12 通过 |
| TSan，std | 12/12 通过 |

新增 `hotpaths` 套件覆盖缓存命中/淘汰/多读线程/异步生产者、复杂格式边界、运行时格式错误、
嵌套自定义 formatter、晚期 TLS 析构日志与按日文件的原始字节。
既有队列、flush/shutdown、并行 sink、配置并发和安装包消费者测试继续通过。
本机 ASan 没有启用不受支持的 LeakSanitizer。
新增 [CI 配置](../.github/workflows/ci.yml)覆盖 Windows / Linux / macOS 的 std 与 fmt 六种组合，
以及 Linux sanitizer 作业。**本次没有实际运行 Windows、Linux 或远程 CI**，不能把配置等同于通过验证。

复现构建和回归：

```sh
cmake -S . -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build build-perf
ctest --test-dir build-perf --output-on-failure

cmake -S . -B build-perf-fmt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHLOG_USE_FMT=ON -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=ON
cmake --build build-perf-fmt
ctest --test-dir build-perf-fmt --output-on-failure

# 相同驱动、不同头文件；以下是 macOS/Linux 编译示例。
mkdir -p build-perf-base/include/chlog
git show 9a5f81b:include/chlog/chlog.hpp > build-perf-base/include/chlog/chlog.hpp
c++ -std=c++20 -O3 -DNDEBUG -pthread -I build-perf-base/include \
  benchmarks/hotpaths_bench.cpp -o build-perf-base/hotpaths
c++ -std=c++20 -O3 -DNDEBUG -pthread -I include \
  benchmarks/hotpaths_bench.cpp -o build-perf/hotpaths
python3 tools/hotpaths_compare.py build-perf-base/hotpaths build-perf/hotpaths \
  --iterations 500000 --repeats 5 --output build-perf/comparison.csv

build-perf/chlog_render_bench 2000
build-perf-fmt/chlog_render_bench 2000
```

fmt 对照在两次编译中同时增加 `-DCHLOG_USE_FMT=1` 及同一份 fmt 的 include/library 参数；
对齐对照在两侧同时增加 `-falign-functions=64`。这些参数只用于实验，不写入库的接口编译选项。
