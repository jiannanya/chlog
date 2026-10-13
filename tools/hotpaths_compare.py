#!/usr/bin/env python3
"""Compare the same hotpaths driver built against two chlog revisions."""

import argparse
import csv
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--iterations", type=int, default=1_000_000)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.iterations <= 0 or args.repeats <= 0:
        parser.error("iterations and repeats must be positive")

    rows = []
    for run in range(args.repeats):
        order = ("before", "after") if run % 2 == 0 else ("after", "before")
        for revision in order:
            result = subprocess.run(
                [str(getattr(args, revision).resolve()), str(args.iterations)],
                check=False, capture_output=True, text=True, timeout=180,
            )
            if result.returncode:
                raise RuntimeError(f"{revision} exited with {result.returncode}:\n{result.stdout}\n{result.stderr}")
            samples = []
            for line in result.stdout.splitlines():
                if not line.startswith("HOTPATH "):
                    continue
                fields = dict(item.split("=", 1) for item in line.split()[1:])
                samples.append(dict(revision=revision, run=run + 1, **fields))
            if not samples:
                raise RuntimeError(f"no benchmark results from {revision}")
            rows.extend(samples)
            print(f"Completed {revision} run {run + 1}/{args.repeats}", flush=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

    print("| Case | Before (s) | After (s) | Throughput change | Paired wins |")
    print("|---|---:|---:|---:|---:|")
    for case in dict.fromkeys(row["case"] for row in rows):
        samples = {
            revision: [float(row["seconds"]) for row in rows
                       if row["case"] == case and row["revision"] == revision]
            for revision in ("before", "after")
        }
        if any(len(values) != args.repeats for values in samples.values()):
            raise RuntimeError(f"incomplete samples for {case}")
        before, after = (statistics.median(samples[key]) for key in ("before", "after"))
        wins = sum(a < b for b, a in zip(samples["before"], samples["after"]))
        print(f"| {case} | {before:.6f} | {after:.6f} | {before / after - 1:+.1%} | {wins}/{args.repeats} |")


if __name__ == "__main__":
    main()
