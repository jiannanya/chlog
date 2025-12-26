#!/usr/bin/env python3

import argparse
import os
import platform
import re
import subprocess
import sys
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
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


def run_exe(exe: Path, env: Dict[str, str]) -> List[Result]:
    p = subprocess.run(
        [str(exe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        text=True,
        check=False,
    )

    results: List[Result] = []
    for raw in p.stdout.splitlines():
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

    return results


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
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    build_dir = (root / args.build_dir).resolve()

    exe = find_exe(build_dir, "chlog_bench_loggers")
    if exe is None:
        raise RuntimeError(f"chlog_bench_loggers not found in {build_dir}")

    env = dict(os.environ)
    env["CHLOG_BENCH_ITERS"] = str(args.iters)

    results = run_exe(exe, env)

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

    versions = vcpkg_versions(["spdlog", "fmt"])
    chlog_ver = read_chlog_version(root)

    runners = sorted({r.runner for r in results})

    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# chlog vs spdlog benchmark report\n\n")
        f.write(f"- Executable: `{exe}`\n")
        f.write(f"- Host: `{host}`\n")
        f.write(f"- Python: `{py}`\n")
        f.write(f"- Iterations: `{args.iters}`\n\n")

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
                f.write(f"- {k}: `{versions[k]}` (vcpkg)\n")
        if not chlog_ver and not versions:
            f.write("- (unavailable)\n")
        f.write("\n")

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
