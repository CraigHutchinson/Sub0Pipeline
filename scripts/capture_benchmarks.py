#!/usr/bin/env python3
"""Capture alternating benchmark samples and retain raw evidence (stdlib only)."""
import argparse
import json
import platform
import statistics
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--current', required=True, type=Path)
    parser.add_argument('--current-ref', required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--features', action='store_true')
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error('use at least three independent process samples')
    if bool(args.baseline) != bool(args.baseline_ref):
        parser.error('--baseline and --baseline-ref must be supplied together')
    args.output.mkdir(parents=True, exist_ok=True)
    versions = {'current': (args.current, args.current_ref)}
    if args.baseline:
        versions['baseline'] = (args.baseline, args.baseline_ref)
    samples = {name: {} for name in versions}
    for repetition in range(args.repeats):
        order = list(versions)
        if repetition % 2:
            order.reverse()
        for name in order:
            executable, _ = versions[name]
            raw = (args.output / f'{name}-{repetition + 1}.json').resolve()
            command = [str(executable.resolve()), '--json', str(raw)]
            if args.features:
                command.append('--features')
            result = subprocess.run(command, text=True, capture_output=True, check=True)
            raw.with_suffix('.txt').write_text(result.stdout + result.stderr)
            for row in json.loads(raw.read_text())['results']:
                samples[name].setdefault(row['name'], []).append(row['median(elapsed)'] * 1e9)
    for name, rows in samples.items():
        if not rows or any(len(values) != args.repeats for values in rows.values()):
            raise ValueError(f'inconsistent benchmark cases across {name} samples')
    # A baseline built from an older harness may lack newer cases. Compare the
    # shared ones and name the rest, so a missing case is never read as a result.
    unmatched = {}
    if args.baseline:
        shared = samples['current'].keys() & samples['baseline'].keys()
        if not shared:
            raise ValueError('baseline and current share no benchmark cases')
        for name, rows in samples.items():
            unmatched[name] = sorted(rows.keys() - shared)
            samples[name] = {case: values for case, values in rows.items() if case in shared}
    summary = {
        'captured_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.platform(),
        'repeats': args.repeats,
        'features': args.features,
        'refs': {name: ref for name, (_, ref) in versions.items()},
        'units': 'ns per complete operation; median of per-process medians',
        'cases_without_counterpart': unmatched,
        'results': {},
    }
    for name, rows in samples.items():
        summary['results'][name] = {
            case: {'median_ns': statistics.median(values),
                   'min_ns': min(values), 'max_ns': max(values), 'samples_ns': values}
            for case, values in rows.items()
        }
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
