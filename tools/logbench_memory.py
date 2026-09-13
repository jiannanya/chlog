#!/usr/bin/env python3
"""Fresh-process Windows memory comparison, without replacing the CRT allocator."""

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
from datetime import datetime
from pathlib import Path

from logbench_report import find_exe, parse_kv, windows_cpu_name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-comparison")
    parser.add_argument("--out", default="docs/logbench_memory.md")
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Process memory collection currently requires Windows GetProcessMemoryInfo")
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    root = Path(__file__).resolve().parent.parent
    build = (root / args.build_dir).resolve()
    executables = {kind: find_exe(build, name) for kind, name in
                   (("process", "chlog_bench_memory"), ("cpp_new", "chlog_bench_allocations"))}
    if not all(executables.values()):
        parser.error("Build chlog_bench_memory and chlog_bench_allocations with spdlog available")
    runs, grouped, object_sizes = [], {}, None
    workloads = [(mode, (13, 128, 1024)) for mode in ("sync_st", "sync_mt", "async", "backlog")]
    workloads += [(mode, (1000, 10000)) for mode in ("objects_st", "objects_mt")]
    workloads += [(mode, (32, 128)) for mode in ("file_st", "file_mt")]
    for repeat in range(args.repeats):
        libraries = ("chlog", "spdlog") if repeat % 2 == 0 else ("spdlog", "chlog")
        for mode, parameters in workloads:
            for size in parameters:
                for kind, exe in executables.items():
                    # CRT and library buffering use different allocators. Compare
                    # file instances using process totals, including both.
                    if mode.startswith("file_") and kind != "process":
                        continue
                    for library in libraries:
                        command = [str(exe), library, mode, str(size)]
                        proc = subprocess.run(command, capture_output=True, text=True, timeout=60, check=True)
                        sizes = [parse_kv(line[6:]) for line in proc.stdout.splitlines() if line.startswith("SIZES ")]
                        if len(sizes) != 1 or (object_sizes is not None and sizes[0] != object_sizes):
                            raise RuntimeError("Object size metadata missing or inconsistent")
                        object_sizes = sizes[0]
                        lines = [line[7:] for line in proc.stdout.splitlines() if line.startswith("MEMORY ")]
                        if len(lines) != 1:
                            raise RuntimeError(f"Unexpected benchmark output: {proc.stdout}")
                        fields = parse_kv(lines[0])
                        numeric = {key: int(value) for key, value in fields.items() if key not in ("runner", "case")}
                        if numeric["calls"] != numeric["processed"] or numeric["retained_bytes"] != 0:
                            raise RuntimeError(f"Incomplete benchmark or retained measured C++ allocations: {fields}")
                        if kind == "process" and not numeric["peak_private_bytes"]:
                            raise RuntimeError("Process memory is unavailable")
                        label = f"{mode}_{size}" if mode.startswith(("objects_", "file_")) else mode
                        row = {"run": repeat + 1, "kind": kind, "runner": library, "case": label, **numeric}
                        runs.append(row)
                        grouped.setdefault((kind, library, label, size), []).append(numeric)
        print(f"Completed memory run {repeat + 1}/{args.repeats}", flush=True)
    if any(len(rows) != args.repeats for rows in grouped.values()):
        raise RuntimeError("Each memory case must contain exactly one sample per run")
    medians = [{"kind": kind, "runner": library, "case": mode,
                **{key: statistics.median(row[key] for row in rows) for key in rows[0]}}
               for (kind, library, mode, _), rows in grouped.items()]
    timestamp = datetime.now().astimezone().isoformat(timespec="seconds")
    out = (root / args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    data = {"schema": "chlog-vs-spdlog-memory/v1", "timestamp": timestamp, "repeats": args.repeats,
            "host": platform.platform(), "cpu": windows_cpu_name(), "queue_capacity": 65536,
            "source_sha256": {str(p.relative_to(root)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in (root / "include/chlog/chlog.hpp", root / "benchmarks/memory_bench.cpp")},
            "binary_sha256": {kind: hashlib.sha256(exe.read_bytes()).hexdigest() for kind, exe in executables.items()},
            "object_sizes": {key: int(value) for key, value in object_sizes.items()}, "medians": medians, "runs": runs}
    out.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf8")
    indexed = {(row["kind"], row["runner"], row["case"], row["payload_bytes"]): row for row in medians}
    lines = ["# chlog vs spdlog memory report", "", f"Measured: {timestamp}; {args.repeats} runs per case.", "",
             f"Host: {data['host']}; CPU: {data['cpu']}.", "",
             "Each library, payload and case runs in a fresh process. Both use the same fmt backend, a borrowed-event "
             "counter sink, and one consumer with a 65,536-slot FIFO queue and blocking overflow. Counter-sink synchronous cases "
             "run 10,000 calls; async streaming also runs 10,000 calls. Backlog cases block the first sink callback, "
             "then fill all 65,536 queue slots before releasing the consumer. Every record must be processed.", "",
             "## Process memory", "",
             "These measurements use the original CRT allocator. Peak private committed bytes and peak working set "
             "come from GetProcessMemoryInfo, and include the executable, runtime, thread stacks, allocator overhead "
             "and malloc allocations made by fmt. They are absolute process totals, not logger-only heap sizes. "
             "Small differences near the runtime baseline are noise; streaming peaks also depend on scheduling.", "",
             "| Case | Payload (B) | chlog peak private (MiB) | spdlog peak private (MiB) | chlog peak working set (MiB) | spdlog peak working set (MiB) |",
             "|---|---:|---:|---:|---:|---:|"]
    for mode in ("sync_st", "sync_mt", "async", "backlog"):
        for size in (13, 128, 1024):
            ch, sp = (indexed["process", runner, mode, size] for runner in ("chlog", "spdlog"))
            values = [ch["peak_private_bytes"], sp["peak_private_bytes"],
                      ch["peak_working_set_bytes"], sp["peak_working_set_bytes"]]
            lines.append(f"| {mode} | {size} | " + " | ".join(f"{v / 2**20:.3f}" for v in values) + " |")
    lines += ["", "## Many synchronous instances", "",
              "Each instance owns its own counter sink. The common harness retains a vector of shared pointers "
              "for both libraries, and each logger processes one 13 B record. This also exposes allocator and "
              "process-memory effects that are too small to resolve with one instance.", "",
              "| Mode | Instances | chlog peak private (MiB) | spdlog peak private (MiB) | chlog resident new bytes | spdlog resident new bytes |",
              "|---|---:|---:|---:|---:|---:|"]
    for mode in ("objects_st", "objects_mt"):
        for count in (1000, 10000):
            label = f"{mode}_{count}"
            ch, sp = (indexed["process", runner, label, 13] for runner in ("chlog", "spdlog"))
            cn, sn = (indexed["cpp_new", runner, label, 13] for runner in ("chlog", "spdlog"))
            lines.append(f"| {mode} | {count:,} | {ch['peak_private_bytes'] / 2**20:.3f} | {sp['peak_private_bytes'] / 2**20:.3f} | "
                         f"{cn['resident_bytes']:,.0f} | {sn['resident_bytes']:,.0f} |")
    lines += ["", "## File instances", "",
              "Each logger owns a distinct built-in rotating-file sink with a message-only pattern. "
              "All files are open simultaneously and each receives one 13 B record. Snapshots include live "
              "file buffers before and after flushing. Both libraries use LF newlines, and every file is "
              "checked after destruction. These process totals include C++ allocations and CRT malloc buffers.", "",
              "| Mode | Instances | chlog peak private (MiB) | spdlog peak private (MiB) | chlog peak working set (MiB) | spdlog peak working set (MiB) |",
              "|---|---:|---:|---:|---:|---:|"]
    for mode in ("file_st", "file_mt"):
        for count in (32, 128):
            ch, sp = (indexed["process", runner, f"{mode}_{count}", 13] for runner in ("chlog", "spdlog"))
            values = [ch["peak_private_bytes"], sp["peak_private_bytes"], ch["peak_working_set_bytes"], sp["peak_working_set_bytes"]]
            lines.append(f"| {mode} | {count} | " + " | ".join(f"{v / 2**20:.3f}" for v in values) + " |")
    lines += ["", "## C++ new diagnostics", "",
              "A separate instrumented executable tracks requested C++ new bytes; it is not used for the process "
              "memory table above. It excludes malloc/free and fmt's dynamic memory buffers, so zero allocations "
              "here does not establish zero total heap allocation. Resident bytes include logger, sink, queue and "
              "owned configuration immediately before logging. The workload column counts C++ allocations during "
              "logging and shutdown. All tracked bytes must be released after destruction.", "",
              "| Case | Payload (B) | chlog resident new bytes | spdlog resident new bytes | chlog construction peak (B) | spdlog construction peak (B) | chlog workload new calls | spdlog workload new calls |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for mode in ("sync_st", "sync_mt", "async", "backlog"):
        for size in (13, 128, 1024):
            ch, sp = (indexed["cpp_new", runner, mode, size] for runner in ("chlog", "spdlog"))
            lines.append(f"| {mode} | {size} | {ch['resident_bytes']:,.0f} | {sp['resident_bytes']:,.0f} | "
                         f"{ch['construction_peak_bytes']:,.0f} | {sp['construction_peak_bytes']:,.0f} | "
                         f"{ch['allocations']:,.0f} | {sp['allocations']:,.0f} |")
    ch, sp = (indexed["cpp_new", runner, "sync_mt", 13] for runner in ("chlog", "spdlog"))
    lines += ["", f"The synchronous logger plus one counter sink retains {ch['resident_bytes']:,.0f} B for chlog and "
              f"{sp['resident_bytes']:,.0f} B for spdlog. Construction peaks are {ch['construction_peak_bytes']:,.0f} B "
              f"and {sp['construction_peak_bytes']:,.0f} B respectively. `sizeof(logger)` is "
              f"{object_sizes['chlog_logger']} B for chlog and {object_sizes['spdlog_logger']} B for spdlog.", "",
              "These measurements cover the stated configuration. Existing "
              "owning-event chlog sinks can allocate when copying long messages; view_sink avoids that copy "
              "for inline dispatch. Arbitrary user sinks and every platform are outside this comparison.", "",
              f"Raw runs and source hashes: [{out.with_suffix('.json').name}]({out.with_suffix('.json').name}).", ""]
    out.write_text("\n".join(lines), encoding="utf8")
    print(f"Wrote: {out}")


if __name__ == "__main__":
    main()
