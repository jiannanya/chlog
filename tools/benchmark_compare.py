#!/usr/bin/env python3
"""Run identical benchmark drivers against several revisions, in rotated order."""

import argparse
import csv
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', action='append', required=True, metavar='NAME=EXECUTABLE')
    parser.add_argument('--driver', choices=('loggers', 'hotpaths'), required=True)
    parser.add_argument('--case', action='append', default=[], dest='cases')
    parser.add_argument('--iterations', type=int, default=3_000_000)
    parser.add_argument('--repeats', type=int, default=6)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    revisions = {}
    for spec in args.revision:
        name, sep, executable = spec.partition('=')
        if not sep or not name or name in revisions:
            parser.error('each revision needs a unique NAME=EXECUTABLE')
        revisions[name] = str(Path(executable).resolve())
    if len(revisions) < 2 or args.iterations < 1 or args.repeats < 1:
        parser.error('at least two revisions and positive iterations/repeats required')

    rows, expected = [], None
    names = list(revisions)
    for run in range(args.repeats):
        offset = run % len(names)
        order = names[offset:] + names[:offset]
        if (run // len(names)) % 2:
            order.reverse()
        for position, revision in enumerate(order):
            command = [revisions[revision]]
            if args.driver == 'loggers':
                command += ['--iters', str(args.iterations)]
                for case in args.cases:
                    command += ['--case', case]
            else:
                command += [str(args.iterations), *args.cases]
            result = subprocess.run(command, capture_output=True, text=True, timeout=300)
            if result.returncode:
                raise RuntimeError(f'{revision} failed:\n{result.stdout}\n{result.stderr}')
            samples = []
            for line in result.stdout.splitlines():
                if not line.startswith(('HOTPATH ', 'RESULT ')):
                    continue
                fields = dict(item.split('=', 1) for item in line.split()[1:])
                if fields.get('runner', 'chlog') != 'chlog':
                    continue
                if args.driver == 'loggers':
                    calls = int(fields['calls'])
                    processed = 0 if fields['case'] == 'filtered_out' else calls
                    if int(fields['processed']) != processed or int(fields['dropped']):
                        raise RuntimeError(f'incomplete benchmark: {line}')
                if float(fields['seconds']) <= 0:
                    raise RuntimeError(f'invalid timing: {line}')
                samples.append(dict(revision=revision, run=run + 1, position=position + 1, **fields))
            cases = [row['case'] for row in samples]
            if not cases or len(set(cases)) != len(cases):
                raise RuntimeError(f'missing/duplicate cases from {revision}')
            if args.cases and set(cases) != set(args.cases):
                raise RuntimeError(f'requested cases not returned: {cases}')
            if expected is None:
                expected = set(cases)
            elif set(cases) != expected:
                raise RuntimeError(f'different case sets from {revision}')
            rows.extend(samples)
            print(f'Completed {revision} run {run + 1}/{args.repeats}', flush=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

    print('| Case | ' + ' | '.join(f'{name} (s)' for name in names)
          + ' | ' + ' | '.join(f'{names[-1]} vs {name}; paired wins' for name in names[:-1]) + ' |')
    print('|---|' + '---:|' * (2 * len(names) - 1))
    for case in dict.fromkeys(row['case'] for row in rows):
        samples = {name: [float(row['seconds']) for row in rows
                          if row['case'] == case and row['revision'] == name] for name in names}
        medians = {name: statistics.median(values) for name, values in samples.items()}
        changes = []
        for name in names[:-1]:
            wins = sum(new < old for old, new in zip(samples[name], samples[names[-1]]))
            changes.append(f'{medians[name] / medians[names[-1]] - 1:+.1%}; {wins}/{args.repeats}')
        print('| ' + case + ' | ' + ' | '.join(f'{medians[name]:.6f}' for name in names)
              + ' | ' + ' | '.join(changes) + ' |')


if __name__ == '__main__':
    main()
