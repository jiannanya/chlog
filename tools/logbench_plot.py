#!/usr/bin/env python3
"""Generate a bar chart from docs/logbench_results.md.

- Input: markdown produced by tools/logbench_report.py
- Output: a standalone SVG (no external dependencies)

The chart uses log10(calls/s) scaling so very fast cases (e.g. filtered_out)
can still be shown alongside slower cases.
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class SummaryTable:
    runners: List[str]
    cases: List[str]
    cps: Dict[Tuple[str, str], float]  # (case, runner) -> calls/s


def _parse_float_cell(cell: str) -> Optional[float]:
    s = cell.strip()
    if not s or s == "-":
        return None
    # Accept forms like 5.026e+06
    try:
        return float(s)
    except ValueError:
        return None


def parse_summary_table(md_text: str) -> SummaryTable:
    lines = md_text.splitlines()

    # Find Summary section
    start = None
    for i, line in enumerate(lines):
        if line.strip().startswith("## Summary"):
            start = i
            break
    if start is None:
        raise SystemExit("Could not find '## Summary' section in markdown")

    # Find header row: | Case | chlog | spdlog |
    header_i = None
    for i in range(start, min(start + 80, len(lines))):
        if lines[i].strip().startswith("| Case "):
            header_i = i
            break
    if header_i is None:
        raise SystemExit("Could not find summary table header row")

    header_cells = [c.strip() for c in lines[header_i].strip().strip("|").split("|")]
    if len(header_cells) < 2 or header_cells[0] != "Case":
        raise SystemExit("Unexpected summary table header format")

    runners = header_cells[1:]

    cps: Dict[Tuple[str, str], float] = {}
    cases: List[str] = []

    # Data rows start after separator row
    row_i = header_i + 2
    while row_i < len(lines):
        row = lines[row_i].strip()
        if not row:
            break
        if not row.startswith("|"):
            break
        cells = [c.strip() for c in row.strip().strip("|").split("|")]
        if len(cells) != 1 + len(runners):
            break
        case = cells[0]
        cases.append(case)
        for j, runner in enumerate(runners):
            v = _parse_float_cell(cells[1 + j])
            if v is not None:
                cps[(case, runner)] = v
        row_i += 1

    if not cases:
        raise SystemExit("Summary table has no data rows")

    return SummaryTable(runners=runners, cases=cases, cps=cps)


def _fmt_sci(v: float) -> str:
    # Match the report's style like 5.026e+06
    return f"{v:.3e}"


def render_svg_bar_chart(
    table: SummaryTable,
    title: str,
    width: int = 980,
    height: int = 480,
) -> str:
    # Layout
    margin_l = 90
    margin_r = 30
    margin_t = 60
    margin_b = 85

    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    # Determine log10 scale range
    values = [v for v in table.cps.values() if v > 0]
    if not values:
        raise SystemExit("No positive calls/s values found")

    log_vals = [math.log10(v) for v in values]
    y_min = math.floor(min(log_vals))
    y_max = math.ceil(max(log_vals))
    if y_max == y_min:
        y_max += 1

    def y_to_px(log10_v: float) -> float:
        t = (log10_v - y_min) / (y_max - y_min)
        return margin_t + (1.0 - t) * plot_h

    # Simple palette (svg-friendly)
    palette = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd"]

    n_cases = len(table.cases)
    n_runners = len(table.runners)

    group_w = plot_w / n_cases
    # Inner spacing: left padding + bars + right padding
    inner_pad = max(8.0, group_w * 0.10)
    bars_w_total = max(1.0, group_w - 2 * inner_pad)
    bar_gap = max(6.0, bars_w_total * 0.06)
    bar_w = (bars_w_total - bar_gap * (n_runners - 1)) / max(1, n_runners)

    # Build SVG
    out: List[str] = []
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">')
    out.append('<rect x="0" y="0" width="100%" height="100%" fill="white" />')

    # Title
    out.append(f'<text x="{margin_l}" y="{margin_t - 25}" font-family="Segoe UI, Arial" font-size="18" fill="#111">{_xml_escape(title)}</text>')

    # Axes + grid
    axis_x0 = margin_l
    axis_y0 = margin_t + plot_h
    axis_x1 = margin_l + plot_w
    axis_y1 = margin_t

    # Horizontal grid + y labels (log10)
    for tick in range(int(y_min), int(y_max) + 1):
        y = y_to_px(float(tick))
        out.append(f'<line x1="{axis_x0}" y1="{y:.2f}" x2="{axis_x1}" y2="{y:.2f}" stroke="#e5e5e5" stroke-width="1" />')
        out.append(
            f'<text x="{axis_x0 - 10}" y="{y + 4:.2f}" text-anchor="end" '
            f'font-family="Segoe UI, Arial" font-size="12" fill="#333">10^{tick}</text>'
        )

    # Axis lines
    out.append(f'<line x1="{axis_x0}" y1="{axis_y0}" x2="{axis_x1}" y2="{axis_y0}" stroke="#333" stroke-width="1.2" />')
    out.append(f'<line x1="{axis_x0}" y1="{axis_y0}" x2="{axis_x0}" y2="{axis_y1}" stroke="#333" stroke-width="1.2" />')

    # Y axis label
    out.append(
        f'<text x="{25}" y="{margin_t + plot_h/2:.2f}" transform="rotate(-90 25,{margin_t + plot_h/2:.2f})" '
        f'font-family="Segoe UI, Arial" font-size="12" fill="#333">calls/s (log10 scale)</text>'
    )

    # Bars
    for ci, case in enumerate(table.cases):
        gx = margin_l + ci * group_w
        base_x = gx + inner_pad

        # Case label
        out.append(
            f'<text x="{gx + group_w/2:.2f}" y="{axis_y0 + 38}" text-anchor="middle" '
            f'font-family="Segoe UI, Arial" font-size="12" fill="#111">{_xml_escape(case)}</text>'
        )

        for ri, runner in enumerate(table.runners):
            v = table.cps.get((case, runner))
            if v is None or v <= 0:
                continue
            logv = math.log10(v)
            y_top = y_to_px(logv)
            x = base_x + ri * (bar_w + bar_gap)
            y = y_top
            h = axis_y0 - y_top

            color = palette[ri % len(palette)]
            out.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_w:.2f}" height="{h:.2f}" fill="{color}" />')
            # Value label
            out.append(
                f'<text x="{x + bar_w/2:.2f}" y="{y - 6:.2f}" text-anchor="middle" '
                f'font-family="Consolas, Menlo, monospace" font-size="11" fill="#111">{_xml_escape(_fmt_sci(v))}</text>'
            )

    # Legend
    leg_x = axis_x1 - 220
    leg_y = margin_t - 42
    out.append(f'<rect x="{leg_x}" y="{leg_y}" width="210" height="{18 + 18*n_runners}" fill="#fff" stroke="#ddd" />')
    out.append(f'<text x="{leg_x + 10}" y="{leg_y + 18}" font-family="Segoe UI, Arial" font-size="12" fill="#111">Runners</text>')
    for ri, runner in enumerate(table.runners):
        y = leg_y + 18 + (ri + 1) * 18
        color = palette[ri % len(palette)]
        out.append(f'<rect x="{leg_x + 10}" y="{y - 11}" width="12" height="12" fill="{color}" />')
        out.append(
            f'<text x="{leg_x + 30}" y="{y - 1}" font-family="Segoe UI, Arial" font-size="12" fill="#111">{_xml_escape(runner)}</text>'
        )

    out.append("</svg>")
    return "\n".join(out)


def _xml_escape(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&apos;")
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate an SVG bar chart from docs/logbench_results.md")
    ap.add_argument("--in", dest="in_path", default="docs/logbench_results.md", help="Input markdown path")
    ap.add_argument("--out", dest="out_path", default="docs/logbench_summary.svg", help="Output SVG path")
    ap.add_argument("--title", dest="title", default="chlog vs spdlog (calls/s, log scale)", help="Chart title")
    args = ap.parse_args()

    in_path = Path(args.in_path)
    out_path = Path(args.out_path)

    md = in_path.read_text(encoding="utf-8")
    table = parse_summary_table(md)

    svg = render_svg_bar_chart(table=table, title=args.title)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(svg, encoding="utf-8")

    print(f"Wrote: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
