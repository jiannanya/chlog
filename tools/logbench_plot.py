#!/usr/bin/env python3
"""Render a chlog vs spdlog SVG from logbench_report.py output (stdlib only)."""

from __future__ import annotations

import argparse
import json
import math
from html import escape
from pathlib import Path

CASES = [
    ("sync_st", "Single-thread format"),
    ("sync_mt", "Thread-safe sync format"),
    ("async_mt", "Async / 1 producer"),
    ("async_4p", "Async / 4 producers"),
    ("sync_st_literal", "Single-thread plain text"),
    ("sync_mt_literal", "Thread-safe sync plain text"),
    ("async_mt_literal", "Async plain text / 1 producer"),
    ("sync_st_payload128", "Single-thread / 128 B"),
    ("sync_mt_payload128", "Thread-safe sync / 128 B"),
    ("async_mt_payload128", "Async / 128 B"),
    ("sync_st_payload1024", "Single-thread / 1 KiB"),
    ("sync_mt_payload1024", "Thread-safe sync / 1 KiB"),
    ("async_mt_payload1024", "Async / 1 KiB"),
]


def load_report(path: Path) -> dict:
    if path.suffix.lower() == ".json":
        data = json.loads(path.read_text(encoding="utf8"))
        if data.get("schema") != "chlog-vs-spdlog/v1":
            raise ValueError("Expected chlog vs spdlog measurements from logbench_report.py")
        return data

    # Retain support for the Markdown reports used by the original CLI.
    lines = path.read_text(encoding="utf8").splitlines()
    in_summary, runners, records = False, [], []
    for line in lines:
        if line.startswith("## Summary"):
            in_summary = True
            continue
        if in_summary and line.startswith("## "):
            break
        if not in_summary or not line.startswith("|"):
            continue
        cells = [part.strip() for part in line.strip("|").split("|")]
        if cells[0] == "Case":
            runners = cells[1:]
        elif runners and not cells[0].startswith("---"):
            for runner, value in zip(runners, cells[1:]):
                if value != "-":
                    records.append({"case": cells[0], "runner": runner, "cps": float(value)})
    if not records:
        raise ValueError("No Summary table found in Markdown report")
    return {"medians": records, "metadata": {}, "versions": {}, "date": "", "repeats": None}


def render_comparison(data: dict, source: str, title: str = "chlog vs spdlog", memory: dict | None = None) -> str:
    values = {(r["case"], r["runner"]): float(r["cps"]) for r in data["medians"]}
    cases = [(case, label) for case, label in CASES
             if all((case, runner) in values for runner in ("chlog", "spdlog"))]
    if not cases:
        raise ValueError("No logging cases have results for both chlog and spdlog")
    if any(not math.isfinite(v) or v <= 0 for v in values.values()):
        raise ValueError("Throughput must be finite and positive")
    versions, metadata = data.get("versions", {}), data.get("metadata", {})
    ink, muted, grid = "#142b3a", "#576c7b", "#e6edf1"
    colors = {"chlog": "#078575", "spdlog": "#647dce"}
    panel_height = 185 + len(cases) * 70
    bottom = 202 + panel_height
    height = bottom + 136
    show_memory = memory is not None and bottom >= 1250
    output = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="{height}" '
        f'viewBox="0 0 1280 {height}" role="img" aria-labelledby="chart-title chart-desc">',
        f'<title id="chart-title">{escape(title)}</title>',
        '<desc id="chart-desc">Logging throughput in millions of calls per second; higher is better. '
        'All bars start at zero. Filtered calls use a separate billions scale. '
        'Ratios are chlog throughput divided by spdlog throughput. Results measure counter sinks, not disk I/O. '
        'When included, the memory panel shows full-queue process peak private memory in MiB, where lower is better.</desc>',
        '<style>text{font-family:Segoe UI,Arial,sans-serif;font-variant-numeric:tabular-nums} '
        '.code{font-family:Consolas,Menlo,monospace}</style>',
        f'<rect width="1280" height="{height}" fill="#f3f6f8"/>',
    ]

    def text(x, y, value, size=14, color=ink, weight=400, anchor="start", extra=""):
        output.append(f'<text x="{x:.2f}" y="{y:.2f}" font-size="{size}" fill="{color}" '
                      f'font-weight="{weight}" text-anchor="{anchor}" {extra}>{escape(str(value))}</text>')

    def rect(x, y, w, h, fill, radius=0, extra=""):
        output.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" '
                      f'rx="{radius}" fill="{fill}" {extra}/>')

    def line(x1, y1, x2, y2):
        output.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{grid}"/>')

    def maximum(items):
        largest = max(items)
        unit = 10 ** math.floor(math.log10(largest))
        return math.ceil(largest / unit) * unit

    def bar(x, y, span, case, runner, scale):
        value = values[case, runner]
        rect(x, y, span * value / scale, 12, colors[runner], 3,
             f'data-case="{case}" data-runner="{runner}" data-cps="{value:.12g}"')

    text(40, 43, "CHLOG / BENCHMARK", 12, colors["chlog"], 700, extra='letter-spacing="1.8"')
    date = data.get("date", "")
    aggregation = f'{data["repeats"]}-run medians' if data.get("repeats") else "Saved report"
    text(1240, 43, f"{date}  /  {aggregation}" if date else aggregation, 13, muted, anchor="end")
    text(40, 96, title, 38, weight=700)
    text(40, 131, "Logging API throughput  ·  Higher is better", 17, muted)
    for runner, x in (("chlog", 40), ("spdlog", 228)):
        rect(x, 158, 16, 12, colors[runner], 3)
        text(x + 24, 169, f'{runner} {versions.get(runner, "")}'.strip(), 14, ink, 600)
    text(1240, 169, "Ratio = chlog / spdlog", 14, muted, anchor="end")

    rect(40, 202, 858, panel_height, "#fff", 16, 'stroke="#dfe7eb"')
    text(64, 240, "Logging throughput", 22, weight=650)
    text(64, 266, "Million calls/s  ·  Shared linear scale", 14, muted)
    text(866, 294, "RATIO", 11, muted, 700, "end", 'letter-spacing="1"')
    scale = maximum([values[case, runner] for case, _ in cases for runner in colors])
    x, span = 326, 408
    for index in range(6):
        px = x + span * index / 5
        line(px, 310, px, bottom - 55)
        text(px, bottom - 30, f"{scale * index / 5 / 1e6:g}", 12, muted, anchor="middle")
    for index, (case, label) in enumerate(cases):
        y = 326 + index * 70
        text(64, y + 16, label, 14, weight=600)
        text(64, y + 36, case, 11, muted, extra='class="code"')
        for runner, offset in (("chlog", 0), ("spdlog", 23)):
            value = values[case, runner]
            bar(x, y + offset, span, case, runner, scale)
            text(x + span * value / scale + 8, y + offset + 11, f"{value / 1e6:.3f}", 12, ink, 500)
        ratio = values[case, "chlog"] / values[case, "spdlog"]
        text(866, y + 26, f"{ratio:.2f}×", 15, colors["chlog"] if ratio >= 1 else colors["spdlog"], 650, "end")
    text(326, bottom - 10, "Zero-based bars; a ratio below 1 favors spdlog.", 11, muted)

    has_filter = all(("filtered_out", runner) in values for runner in colors)
    if has_filter:
        rect(922, 202, 318, 222, "#fff", 16, 'stroke="#dfe7eb"')
        text(944, 240, "Filter-only calls", 21, weight=650)
        text(944, 265, "Billion calls/s  ·  Separate scale", 13, muted)
        filter_scale = maximum([values["filtered_out", runner] for runner in colors])
        for runner, y in (("chlog", 288), ("spdlog", 332)):
            text(944, y, runner, 12, muted)
            text(1216, y, f'{values["filtered_out", runner] / 1e9:.3f} B', 13, ink, 600, "end")
            rect(944, y + 8, 272, 12, "#f0f4f6", 3)
            bar(944, y + 8, 272, "filtered_out", runner, filter_scale)
        text(944, 378, "0", 11, muted)
        text(1216, 378, f"{filter_scale / 1e9:g} B", 11, muted, anchor="end")
        text(944, 404, "Short runtimes are sensitive to timing noise.", 12, muted)

    if panel_height >= 500 and data.get("iterations"):
        setup_bottom = 826 if show_memory else bottom
        rect(922, 442, 318, setup_bottom - 442, "#fff", 16, 'stroke="#dfe7eb"')
        text(944, 480, "Benchmark setup", 21, weight=650)
        items = [
            ("SAMPLES", f'{data["iterations"]:,} calls × {data["repeats"]} runs'),
            ("FORMAT BACKEND", f'fmt {versions.get("fmt", "unknown")} for both libraries'),
            ("ASYNC QUEUE", "65,536 slots / FIFO / blocking"),
            ("WORKER", "1 consumer; drain time included"),
            ("SINK", "One atomic increment per event"),
        ]
        for index, (label, value) in enumerate(items):
            y = 517 + index * 55
            text(944, y, label, 10, muted, 700, extra='letter-spacing="1"')
            text(944, y + 23, value, 14, ink, 500)
        text(944, setup_bottom - 40, "Borrowed-event counter sinks; no disk I/O.", 12, muted)
        text(944, setup_bottom - 18, "Native metadata policies differ by library.", 12, muted)

    if show_memory:
        measured = {(r["case"], r["runner"], r["payload_bytes"]): r for r in memory["medians"] if r["kind"] == "process"}
        sizes = (13, 128, 1024)
        mem_values = [measured["backlog", runner, size]["peak_private_bytes"] for size in sizes for runner in colors]
        if any(not math.isfinite(v) or v <= 0 for v in mem_values):
            raise ValueError("Memory measurements must be finite and positive")
        mem_scale = math.ceil(max(mem_values) / 2**20 / 10) * 10
        rect(922, 848, 318, bottom - 848, "#fff", 16, 'stroke="#dfe7eb"')
        text(944, 886, "Full-queue memory", 21, weight=650)
        text(944, 910, "Peak private MiB  ·  Lower is better", 13, muted)
        for index, size in enumerate(sizes):
            y = 940 + index * 76
            text(944, y, f"{size:,} B payload", 12, muted)
            for runner, offset in (("chlog", 10), ("spdlog", 30)):
                value = measured["backlog", runner, size]["peak_private_bytes"]
                width = 220 * (value / 2**20) / mem_scale
                rect(944, y + offset, width, 12, colors[runner], 3,
                     f'data-memory="backlog" data-payload="{size}" data-runner="{runner}" data-bytes="{value:.12g}"')
                text(944 + width + 6, y + offset + 11, f"{value / 2**20:.2f}", 11)
        text(944, 1167, "Original allocator; fresh process per sample.", 12, muted)
        text(944, 1189, "Includes runtime, stacks and allocator overhead.", 11, muted)
        sync = {r["runner"]: r for r in memory["medians"]
                if r["kind"] == "cpp_new" and r["case"] == "sync_mt" and r["payload_bytes"] == 13}
        text(944, 1220, "Sync instance / requested heap", 12, ink, 600)
        text(944, 1242, f'chlog {sync["chlog"]["resident_bytes"]:g} B  ·  spdlog {sync["spdlog"]["resident_bytes"]:g} B', 12, muted)
        text(944, 1272, "See docs/logbench_memory.md for all cases.", 11, muted)

    line(40, bottom + 24, 1240, bottom + 24)
    text(40, bottom + 53, f"Source: {source}", 13, muted)
    cpu = str(data.get("cpu") or "See the source report for measurement conditions.").replace("12th Gen ", "").replace("(R)", "").replace("(TM)", "")
    config = "  ·  ".join(str(v) for v in [cpu, metadata.get("compiler"), data.get("build_flags"),
                                          f'CPU mask {data["affinity_mask"]}' if data.get("affinity_mask") else None] if v)
    text(40, bottom + 80, config, 12, muted)
    note = "All recorded events were processed with zero drops. Host activity may affect timings."
    if data.get("repeats"):
        note = "Library order alternates between runs. " + note
    text(40, bottom + 107, note, 12, muted)
    output.append("</svg>")
    return "\n".join(output) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in", dest="input", default="docs/logbench_results.json", help="JSON or Markdown benchmark report")
    parser.add_argument("--out", default="docs/logbench_summary.svg", help="Output SVG path")
    parser.add_argument("--title", default="chlog vs spdlog", help="Chart title")
    parser.add_argument("--memory", help="Optional JSON from logbench_memory.py")
    args = parser.parse_args()
    source, destination = Path(args.input), Path(args.out)
    data = load_report(source)
    memory = json.loads(Path(args.memory).read_text(encoding="utf8")) if args.memory else None
    if memory:
        if memory.get("schema") != "chlog-vs-spdlog-memory/v1":
            raise ValueError("Expected a chlog vs spdlog memory report")
        key = "include/chlog/chlog.hpp"
        if memory["source_sha256"][key] != data.get("source_sha256", {}).get(key):
            raise ValueError("Throughput and memory reports must measure the same header")
    svg = render_comparison(data, source.as_posix(), args.title, memory)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(svg, encoding="utf8")
    print(f"Wrote: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
