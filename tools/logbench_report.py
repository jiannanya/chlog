#!/usr/bin/env python3

import argparse
import os
import platform
import re
import subprocess
import sys
import ctypes
import json
import hashlib
import statistics
from datetime import datetime
from ctypes import wintypes
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional


RESULT_RE = re.compile(r"^RESULT\s+(.*)$")


@dataclass(frozen=True)
class Result:
    runner: str
    case: str
    calls: int
    seconds: float
    processed: int
    dropped: int
    cps: float


def parse_kv(s: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for token in s.strip().split():
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        out[k] = v
    return out


def find_exe(build_dir: Path, name: str) -> Optional[Path]:
    candidates = [
        build_dir / name,
        build_dir / f"{name}.exe",
        build_dir / "Release" / f"{name}.exe",
        build_dir / "Debug" / f"{name}.exe",
        build_dir / "RelWithDebInfo" / f"{name}.exe",
        build_dir / "MinSizeRel" / f"{name}.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def run_exe(exe: Path, env: Dict[str, str], spdlog_first: bool = False):
    p = subprocess.run(
        [str(exe)] + (["--spdlog-first"] if spdlog_first else []),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        text=True,
        check=False,
        timeout=180,
    )
    if p.returncode:
        raise RuntimeError(f"Benchmark exited with {p.returncode}:\n{p.stdout}")

    results: List[Result] = []
    metadata = {}
    for raw in p.stdout.splitlines():
        if raw.startswith("META "):
            metadata.update(parse_kv(raw[5:]))
        m = RESULT_RE.match(raw.strip())
        if not m:
            continue
        kv = parse_kv(m.group(1))
        results.append(
            Result(
                runner=kv["runner"],
                case=kv["case"],
                calls=int(kv["calls"]),
                seconds=float(kv["seconds"]),
                processed=int(kv.get("processed", "0")),
                dropped=int(kv.get("dropped", "0")),
                cps=float(kv["cps"]),
            )
        )

    if not results:
        raise RuntimeError(f"No RESULT lines parsed from {exe}. Output:\n{p.stdout}")

    for r in results:
        expected = 0 if r.case == "filtered_out" else r.calls
        if r.processed != expected or r.dropped != 0:
            raise RuntimeError(f"Incomplete benchmark: {r}")
    return results, metadata, p.stdout


def fmt_num(x: Optional[float]) -> str:
    if x is None:
        return "-"
    if x >= 1e6:
        return f"{x:.3e}"
    if x >= 1e3:
        return f"{x:.1f}"
    return f"{x:.3f}"


def windows_cpu_name() -> Optional[str]:
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
        ) as k:
            v, _ = winreg.QueryValueEx(k, "ProcessorNameString")
            return str(v).strip()
    except Exception:
        return None


def windows_total_phys_mem_bytes() -> Optional[int]:
    try:
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [
                ("dwLength", wintypes.DWORD),
                ("dwMemoryLoad", wintypes.DWORD),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]

        ms = MEMORYSTATUSEX()
        ms.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
            return None
        return int(ms.ullTotalPhys)
    except Exception:
        return None


def fmt_bytes(n: Optional[int]) -> str:
    if n is None:
        return "-"
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    x = float(n)
    for u in units:
        if x < 1024.0 or u == units[-1]:
            if u == "B":
                return f"{int(x)} {u}"
            return f"{x:.2f} {u}"
        x /= 1024.0
    return f"{n} B"


def try_get_vcpkg_exe() -> Optional[str]:
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    if vcpkg_root:
        cand = os.path.join(vcpkg_root, "vcpkg.exe")
        if os.path.exists(cand):
            return cand
    # fallback: rely on PATH
    return "vcpkg"


def vcpkg_versions(want: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    exe = try_get_vcpkg_exe()
    if not exe:
        return out
    try:
        p = subprocess.run([exe, "list"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
        lines = p.stdout.splitlines()
        for line in lines:
            # example: spdlog:x64-windows  1.15.3  ...
            parts = line.strip().split()
            if len(parts) < 2:
                continue
            name_triplet = parts[0]
            version = parts[1]
            pkg = name_triplet.split(":", 1)[0]
            if pkg in want:
                out[pkg] = version
    except Exception:
        return out
    return out


def read_chlog_version(root: Path) -> Optional[str]:
    try:
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8", errors="ignore")
        m = re.search(r"project\(\s*chlog\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", cmake, re.IGNORECASE)
        if m:
            return m.group(1)
    except Exception:
        pass
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Run chlog vs spdlog benchmark and emit Markdown report.")
    ap.add_argument("--build-dir", default="build-clang", help="CMake build directory containing executables")
    ap.add_argument("--out", default="docs/logbench_results.md", help="Markdown output path")
    ap.add_argument("--iters", type=int, default=1_000_000, help="Iterations (CHLOG_BENCH_ITERS)")
    ap.add_argument("--repeats", type=int, default=5, help="Runs to aggregate by median; alternate library order")
    ap.add_argument("--affinity", type=lambda s: int(s, 0), help="Optional Windows CPU mask, e.g. 0x5555")
    args = ap.parse_args()
    if args.iters <= 0:
        ap.error("--iters must be positive")
    if args.repeats <= 0:
        ap.error("--repeats must be positive")
    if args.affinity is not None:
        if os.name != "nt" or args.affinity <= 0:
            ap.error("--affinity requires Windows and a positive CPU mask")
        if not ctypes.windll.kernel32.SetProcessAffinityMask(ctypes.c_void_p(-1), ctypes.c_size_t(args.affinity)):
            raise ctypes.WinError()

    root = Path(__file__).resolve().parent.parent
    build_dir = (root / args.build_dir).resolve()

    exe = find_exe(build_dir, "chlog_bench_loggers")
    if exe is None:
        raise RuntimeError(f"chlog_bench_loggers not found in {build_dir}")

    env = dict(os.environ)
    env["CHLOG_BENCH_ITERS"] = str(args.iters)

    samples = {}
    runs = []
    metadata = None
    expected_cases = None
    for index in range(args.repeats):
        results, run_metadata, stdout = run_exe(exe, env, index % 2 == 1)
        keys = {(r.case, r.runner) for r in results}
        if expected_cases is not None and keys != expected_cases:
            raise RuntimeError("Benchmark cases changed between runs")
        if metadata is not None and metadata != run_metadata:
            raise RuntimeError("Benchmark configuration changed between runs")
        expected_cases, metadata = keys, run_metadata
        for r in results:
            samples.setdefault((r.case, r.runner), []).append(r)
        runs.append({"run": index + 1, "first": "spdlog" if index % 2 else "chlog",
                     "results": [asdict(r) for r in results], "stdout": stdout})
        print(f"Completed run {index + 1}/{args.repeats}", flush=True)
    results = [Result(runner=runner, case=case, calls=group[0].calls,
                      seconds=statistics.median(r.seconds for r in group),
                      cps=statistics.median(r.cps for r in group),
                      processed=group[0].processed, dropped=0)
               for (case, runner), group in samples.items()]

    # index by case -> runner
    by_case: Dict[str, Dict[str, Result]] = {}
    for r in results:
        by_case.setdefault(r.case, {})[r.runner] = r

    out_path = (root / args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    host = platform.platform()
    py = sys.version.split()[0]

    cpu = windows_cpu_name() if os.name == "nt" else platform.processor()
    cpu_count = os.cpu_count()
    mem_total = windows_total_phys_mem_bytes() if os.name == "nt" else None

    versions = {name: metadata[name] for name in ("spdlog", "fmt") if name in metadata}
    chlog_ver = read_chlog_version(root)
    if chlog_ver:
        versions["chlog"] = chlog_ver
    timestamp = datetime.now().astimezone().isoformat(timespec="seconds")
    build_flags = ""
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        match = re.search(r"^CMAKE_CXX_FLAGS_RELEASE:STRING=(.*)$", cache.read_text(encoding="utf8"), re.MULTILINE)
        if match:
            build_flags = match.group(1).strip()

    raw_path = out_path.with_suffix(".json")
    raw_path.write_text(json.dumps({
        "schema": "chlog-vs-spdlog/v1", "date": timestamp[:10], "timestamp": timestamp,
        "iterations": args.iters, "repeats": args.repeats, "host": host, "cpu": cpu,
        "logical_cpus": cpu_count, "memory_bytes": mem_total, "versions": versions,
        "metadata": metadata, "build_flags": build_flags,
        "affinity_mask": hex(args.affinity) if args.affinity is not None else None,
        "source_sha256": {str(p.relative_to(root)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (root / "include/chlog/chlog.hpp", root / "benchmarks/loggers_bench.cpp")},
        "medians": [asdict(r) for r in results], "runs": runs,
    }, indent=2, ensure_ascii=False) + "\n", encoding="utf8")

    runners = sorted({r.runner for r in results})

    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# chlog vs spdlog benchmark report\n\n")
        f.write(f"- Executable: `{exe}`\n")
        f.write(f"- Host: `{host}`\n")
        f.write(f"- Python: `{py}`\n")
        f.write(f"- Iterations: `{args.iters}`\n\n")
        f.write(f"- Measured: `{timestamp}`\n")
        f.write(f"- Runs: `{args.repeats}`; medians; library order alternates\n")
        f.write(f"- Compiler: `{metadata.get('compiler', 'unavailable')}`; Release flags: `{build_flags}`\n")
        f.write(f"- CPU affinity: `{hex(args.affinity) if args.affinity is not None else 'OS default'}`\n")
        f.write(f"- Raw measurements: [{raw_path.name}]({raw_path.name})\n\n")

        f.write("## System\n\n")
        if cpu:
            f.write(f"- CPU: `{cpu}`\n")
        if cpu_count:
            f.write(f"- CPU cores (logical): `{cpu_count}`\n")
        if mem_total is not None:
            f.write(f"- Memory (total): `{fmt_bytes(mem_total)}`\n")
        f.write("\n")

        f.write("## Library versions\n\n")
        if chlog_ver:
            f.write(f"- chlog: `{chlog_ver}`\n")
        for k in ["spdlog", "fmt"]:
            if k in versions:
                f.write(f"- {k}: `{versions[k]}` (reported by the benchmark executable)\n")
        if not chlog_ver and not versions:
            f.write("- (unavailable)\n")
        f.write("\n")

        f.write("## Workload\n\n")
        f.write("Both libraries use the same fmt backend and a sink that performs one relaxed atomic increment per event. "
                "There is no sink-level formatting, extra sink mutex, console output, or disk I/O. "
                "Formatted cases log `v {}` with an integer; literal cases log `message ready`. "
                "Cases ending in `_payload128` or `_payload1024` format a prebuilt string of that byte length with `{}`. "
                "chlog uses `view_sink`; spdlog's sink receives its native borrowed log message. "
                "Existing chlog sinks using owning `log_event` remain supported and may require string copies.\n\n")
        f.write("Async cases use one worker, a 65,536-slot FIFO queue, blocking overflow, and include final draining "
                "and worker shutdown. chlog uses `weighted_queue=false`. All recorded calls are verified as processed "
                "with zero drops; filtered calls correctly process zero events.\n\n")
        f.write("The sync_st and sync_mt cases each have one producer; chlog selects its corresponding logger mode. "
                "The spdlog logger and atomic counter sink are the same in both. Library metadata policies remain native: "
                "chlog's `{msg}` mode omits unused fields, while spdlog still captures its normal event metadata. "
                "These are logging API and dispatch measurements, not end-to-end file throughput. "
                "Filter-only runs are very short and sensitive to timing noise.\n\n")

        f.write("## Summary (calls/s, higher is better)\n\n")
        f.write("| Case | " + " | ".join(runners) + " |\n")
        f.write("|---|" + "|".join(["---:"] * len(runners)) + "|\n")
        for case in sorted(by_case.keys()):
            row = by_case[case]
            f.write("| " + case + " | " + " | ".join(fmt_num(row.get(rn).cps if rn in row else None) for rn in runners) + " |\n")
        f.write("\n")

        f.write("## Details\n\n")
        for case in sorted(by_case.keys()):
            f.write(f"### {case}\n\n")
            f.write("| Runner | calls | seconds | calls/s | processed | dropped |\n")
            f.write("|---|---:|---:|---:|---:|---:|\n")
            for rn in runners:
                r = by_case[case].get(rn)
                if r is None:
                    continue
                f.write(
                    f"| {rn} | {r.calls} | {r.seconds:.6f} | {fmt_num(r.cps)} | {r.processed} | {r.dropped} |\n"
                )
            f.write("\n")

    print(f"Wrote: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
