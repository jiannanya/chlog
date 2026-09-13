#!/usr/bin/env python3
"""Run and validate matched rotating-file workloads, including async draining."""
import argparse
import ctypes
import hashlib
import json
import math
import platform
import statistics
import subprocess
from datetime import datetime
from pathlib import Path

from logbench_report import find_exe, parse_kv, windows_cpu_name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-comparison"))
    parser.add_argument("--out", type=Path, default=Path("docs/logbench_output.md"))
    parser.add_argument("--iters", type=int, default=100000)
    parser.add_argument("--short-iters", type=int, help="Optional longer runs for 128-byte records")
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--affinity", type=lambda value: int(value, 0))
    args = parser.parse_args()
    if args.iters < 1 or args.repeats < 1:
        parser.error("iterations and repeats must be positive")
    if args.short_iters is not None and args.short_iters < 1:
        parser.error("short iterations must be positive")
    if args.affinity is not None:
        if platform.system() != "Windows":
            parser.error("--affinity currently requires Windows")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), args.affinity):
            raise ctypes.WinError(ctypes.get_last_error())
    exe = find_exe(args.build_dir.resolve(), "chlog_bench_output")
    if exe is None:
        raise RuntimeError("build chlog_bench_output first")
    runs, groups, metadata = [], {}, None
    expected_cases = {f"{mode}_{pattern}_{size}" for mode in ("sync_st", "sync_mt", "async_mt")
                      for pattern in ("message", "pattern") for size in (128, 1024)}
    expected_keys = {(library, case) for library in ("chlog", "spdlog") for case in expected_cases}
    for repeat in range(args.repeats):
        command = [str(exe), str(args.iters)] + (["--spdlog-first"] if repeat % 2 else [])
        if args.short_iters is not None:
            command += ["--short-iters", str(args.short_iters)]
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=180, check=True)
        records, current_metadata = [], {}
        for line in result.stdout.splitlines():
            if line.startswith("META "):
                current_metadata.update(parse_kv(line[5:]))
            if not line.startswith("OUTPUT "):
                continue
            record = parse_kv(line[7:])
            for key in ("calls", "bytes", "processed"):
                record[key] = int(record[key])
            for key in ("seconds", "cps"):
                record[key] = float(record[key])
                if not math.isfinite(record[key]) or record[key] <= 0:
                    raise RuntimeError(f"invalid timing: {record}")
            size = int(record["case"].rsplit("_", 1)[1])
            calls = args.short_iters if size == 128 and args.short_iters is not None else args.iters
            expected_bytes = (size + 1 + (34 if "_pattern_" in record["case"] else 0)) * calls
            if record["calls"] != calls or record["processed"] != calls or record["bytes"] != expected_bytes:
                raise RuntimeError(f"incomplete file output: {record}")
            records.append(record)
        keys = {(record["runner"], record["case"]) for record in records}
        if keys != expected_keys or len(records) != len(expected_keys):
            raise RuntimeError("expected 12 matched workloads from both libraries")
        if metadata is not None and metadata != current_metadata:
            raise RuntimeError("benchmark metadata changed between runs")
        metadata = current_metadata
        for record in records:
            groups.setdefault((record["runner"], record["case"]), []).append(record)
        runs.append({"command": command, "stdout": result.stdout, "results": records})
        print(f"Validated file-output run {repeat + 1}/{args.repeats}", flush=True)
    medians = [{"runner": library, "case": case,
                "cps": statistics.median(record["cps"] for record in records),
                "seconds": statistics.median(record["seconds"] for record in records),
                "bytes": records[0]["bytes"]} for (library, case), records in sorted(groups.items())]
    root = Path(__file__).resolve().parents[1]
    data = {"schema": "chlog-vs-spdlog-output/v1", "timestamp": datetime.now().astimezone().isoformat(),
            "host": platform.platform(), "cpu": windows_cpu_name() or platform.processor(),
            "iterations": args.iters, "short_iterations": args.short_iters or args.iters,
            "repeats": args.repeats, "metadata": metadata,
            "affinity_mask": hex(args.affinity) if args.affinity is not None else None,
            "source_sha256": {path: hashlib.sha256((root / path).read_bytes()).hexdigest()
                              for path in ("include/chlog/chlog.hpp", "benchmarks/output_bench.cpp")},
            "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "medians": medians, "runs": runs}
    lookup = {(record["runner"], record["case"]): record["cps"] for record in medians}
    lines = ["# File-output comparison", "", f"Recorded: {data['timestamp']}", "",
             f"CPU: {data['cpu']}. Host: {data['host']}. Affinity: {data['affinity_mask']}.", "",
             f"Median of {args.repeats} alternating-order runs; "
             f"{args.short_iters or args.iters:,} records per 128-byte case and {args.iters:,} per 1,024-byte case, for each library.",
             f"Backend: {metadata.get('backend')}; fmt: {metadata.get('fmt_version')}; spdlog: {metadata.get('spdlog_version')}.", "",
             f"Rotating-file sink object sizes: chlog {metadata.get('chlog_file_sink_bytes')} B; "
             f"spdlog ST {metadata.get('spdlog_file_sink_st_bytes')} B and MT {metadata.get('spdlog_file_sink_mt_bytes')} B. "
             "These sizeof values exclude the logger, allocator, file buffers and heap-owned state.", "",
             "Both libraries use their built-in rotating-file sink with rotation disabled, identical logger names, "
             "prebuilt payloads formatted with `{}`, and binary LF newlines. The full pattern contains the local "
             "date, time, milliseconds, logger name and payload. Both async queues are blocking FIFO queues with "
             "65,536 slots and one worker; chlog uses batches of 256 and no periodic flush. "
             "ST uses a single-thread sink; MT uses a thread-safe sink with one calling thread.", "",
             "Timing includes logging and flushing library buffers; async also includes draining and joining the worker. "
             "Each library and case first performs a separate, untimed 10,000-record warmup with full output validation. "
             "Construction, directory creation and validation are outside the timed interval. Every record and output "
             "byte count is checked. Files use the system temporary directory and the normal filesystem cache; "
             "there is no fsync/durable-storage barrier. Results depend on the filesystem, cache and competing I/O.", "",
             "| Case | chlog calls/s | spdlog calls/s | chlog / spdlog |", "|---|---:|---:|---:|"]
    for case in sorted(expected_cases):
        ch, spd = lookup["chlog", case], lookup["spdlog", case]
        lines.append(f"| {case} | {ch:,.0f} | {spd:,.0f} | {ch / spd:.2f}x |")
    lines.extend(["", f"Source SHA-256: `{data['source_sha256']['include/chlog/chlog.hpp']}`", "",
                  "[Raw measurements](logbench_output.json) | [Counter-sink throughput](logbench_results.md) | [Memory](logbench_memory.md)", ""])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines), encoding="utf-8")
    args.out.with_suffix(".json").write_text(json.dumps(data, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
