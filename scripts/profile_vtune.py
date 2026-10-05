#!/usr/bin/env python3
"""Profile benchmark cases under Intel VTune and summarise where time goes (stdlib only).

Each case runs alone in its own process, looping for a fixed wall time
(`Sub0Pipeline_Bench --exact --case NAME --profile-seconds N`), so every sample
belongs to that workload. Build the benchmark with the `perf-msvc` / `perf-unix`
preset first: without debug symbols VTune reports addresses, not functions.

`hotspots` uses user-mode sampling and `threading` uses user-mode tracing, so
neither needs elevation. Profiled runs are for attribution only; take timings
from `capture_benchmarks.py`, never from a run under the profiler.
"""
import argparse
import csv
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# Metric that ranks functions for each analysis, and extra collection options.
ANALYSES = {
    'hotspots': {'rank': 'CPU Time', 'knobs': ['-knob', 'sampling-mode=sw']},
    'threading': {'rank': 'Wait Time', 'knobs': []},
}


def find_vtune(explicit):
    """Locate the VTune CLI: --vtune, PATH, VTUNE_PROFILER_DIR, then oneAPI defaults."""
    candidates = [explicit] if explicit else []
    candidates.append(shutil.which('vtune'))
    exe = 'vtune.exe' if os.name == 'nt' else 'vtune'
    roots = [os.environ.get('VTUNE_PROFILER_DIR'),
             r'C:\Program Files (x86)\Intel\oneAPI\vtune\latest',
             '/opt/intel/oneapi/vtune/latest']
    candidates += [str(Path(root) / 'bin64' / exe) for root in roots if root]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    sys.exit('VTune not found: pass --vtune, add it to PATH or set VTUNE_PROFILER_DIR')


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def slug(name):
    return re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')


def number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return 0.0


def profile_case(vtune, bench, case, analysis, seconds, features, output, keep):
    """Collect one case and return its ranked function and module totals."""
    spec = ANALYSES[analysis]
    case_dir = output / slug(case)
    case_dir.mkdir(parents=True, exist_ok=True)
    result_dir = case_dir / 'vtune-result'
    if result_dir.exists():
        shutil.rmtree(result_dir)

    target = [str(bench), '--exact', '--case', case, '--profile-seconds', str(seconds)]
    if features:
        target.append('--features')
    collected = run([vtune, '-collect', analysis, *spec['knobs'],
                     '-result-dir', str(result_dir), '-quiet', '--', *target])
    (case_dir / 'collect.txt').write_text(collected.stdout + collected.stderr)
    if collected.returncode != 0:
        sys.exit(f'VTune collection failed for "{case}"; see {case_dir / "collect.txt"}')
    # The benchmark prints: profile,"<name>",<iterations>,<seconds>,<ns per op>
    loop = re.search(r'^profile,"[^"]*",(\d+),([\d.]+),([\d.]+)$', collected.stdout, re.M)

    # Two views of the same samples: inlined callees folded into the function
    # that contains them (what to change), and each inlined frame on its own
    # (which operation inside it costs the time).
    csv_format = ['-format', 'csv', '-csv-delimiter', 'comma']
    reports = (
        ('hotspots', case_dir / 'functions.csv', ['-inline-mode', 'off', *csv_format]),
        ('hotspots', case_dir / 'inline-frames.csv', ['-inline-mode', 'on', *csv_format]),
        ('summary', case_dir / 'summary.txt', []),
    )
    for report, destination, options in reports:
        reported = run([vtune, '-report', report, '-r', str(result_dir), *options,
                        '-report-output', str(destination), '-quiet'])
        if reported.returncode != 0:
            sys.exit(f'VTune report "{report}" failed for "{case}": {reported.stderr.strip()}')
    if not keep:
        shutil.rmtree(result_dir, ignore_errors=True)
    # Evidence gets committed: keep the host description, drop what names the machine.
    (case_dir / 'collect.txt').unlink()
    summary_path = case_dir / 'summary.txt'
    private = ('Computer Name:', 'User Name:', 'Application Command Line:')
    summary_path.write_text(''.join(
        line for line in summary_path.read_text(errors='replace').splitlines(keepends=True)
        if not any(marker in line for marker in private)))

    rank = spec['rank']

    def ranked(path):
        with path.open(newline='', encoding='utf-8', errors='replace') as handle:
            rows = list(csv.DictReader(handle))
        if rows and rank not in rows[0]:
            sys.exit(f'"{rank}" column missing from {path}; columns: {list(rows[0])}')
        total = sum(number(row[rank]) for row in rows)
        share = (lambda value: round(100.0 * value / total, 2)) if total else (lambda value: 0.0)
        modules = {}
        for row in rows:
            module = row.get('Module', '?')
            modules[module] = modules.get(module, 0.0) + number(row[rank])
        entries = [{'function': row['Function'], 'module': row.get('Module', '?'),
                    'source': row.get('Source File', ''),
                    'seconds': round(number(row[rank]), 3), 'percent': share(number(row[rank]))}
                   for row in sorted(rows, key=lambda row: number(row[rank]), reverse=True)
                   if number(row[rank]) > 0.0]
        module_list = [{'module': name, 'seconds': round(value, 3), 'percent': share(value)}
                       for name, value in sorted(modules.items(), key=lambda item: -item[1])]
        return total, module_list, entries

    total, modules, functions = ranked(case_dir / 'functions.csv')
    _, _, inline_frames = ranked(case_dir / 'inline-frames.csv')
    return {
        'case': case,
        'metric': rank,
        'total_seconds': round(total, 3),
        'iterations': int(loop.group(1)) if loop else None,
        'ns_per_op_under_profiler': float(loop.group(3)) if loop else None,
        'modules': modules,
        'functions': functions,
        'inline_frames': inline_frames,
    }


def render(summary, top):
    """Markdown view of the summary: per case, module split then the top functions."""
    lines = [f"# VTune {summary['analysis']} profile", '',
             f"- Captured: {summary['captured_utc']}",
             f"- Host: {summary['platform']}; {summary['vtune']}",
             f"- Benchmark: `{summary['bench']}` at `{summary['ref']}`",
             f"- {summary['seconds']} s per case; ranked by {ANALYSES[summary['analysis']]['rank']}"
             ' (self time, summed over threads)', '']
    for case in summary['cases']:
        lines += [f"## {case['case']}", '']
        if case['iterations']:
            lines += [f"{case['iterations']:,} iterations, {case['ns_per_op_under_profiler']:,.0f} ns/op"
                      ' under the profiler (attribution only, not a timing).', '']
        lines += ['| Module | Seconds | % |', '|---|---:|---:|']
        lines += [f"| `{m['module']}` | {m['seconds']:.3f} | {m['percent']:.1f} |"
                  for m in case['modules'][:6]]
        for title, key in (('Function (inlined callees included)', 'functions'),
                           ('Inlined frame', 'inline_frames')):
            lines += ['', f'| {title} | Module | Seconds | % |', '|---|---|---:|---:|']
            for f in case[key][:top]:
                name = f['function'].replace('|', '\\|').replace('`', "'")
                lines.append(f"| `{name}` | `{f['module']}` | {f['seconds']:.3f} | {f['percent']:.1f} |")
        lines.append('')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bench', required=True, type=Path, help='Sub0Pipeline_Bench built with symbols')
    parser.add_argument('--ref', required=True, help='source revision of the benchmark binary')
    parser.add_argument('--case', action='append', required=True, dest='cases',
                        help='exact case name (see --list on the benchmark); repeatable')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--analysis', choices=sorted(ANALYSES), default='hotspots')
    parser.add_argument('--seconds', type=float, default=10.0, help='loop time per case')
    parser.add_argument('--top', type=int, default=20, help='functions per case in profile.md')
    parser.add_argument('--features', action='store_true', help='also register the opt-in cases')
    parser.add_argument('--keep-results', action='store_true',
                        help='keep raw VTune result directories (large) for the GUI')
    parser.add_argument('--vtune', help='path to the vtune executable')
    args = parser.parse_args()

    vtune = find_vtune(args.vtune)
    bench = args.bench.resolve()
    listing = [bench, '--list'] + (['--features'] if args.features else [])
    known = run(listing, check=True).stdout.splitlines()
    unknown = [case for case in args.cases if case not in known]
    if unknown:
        sys.exit(f'unknown case(s) {unknown}; available: {known}')
    args.output.mkdir(parents=True, exist_ok=True)

    version = run([vtune, '--version']).stdout.splitlines()[0].strip()
    summary = {
        'captured_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.platform(),
        'vtune': version,
        'analysis': args.analysis,
        'bench': bench.name,
        'ref': args.ref,
        'seconds': args.seconds,
        'cases': [profile_case(vtune, bench, case, args.analysis, args.seconds, args.features,
                               args.output.resolve(), args.keep_results)
                  for case in args.cases],
    }
    (args.output / 'profile.json').write_text(json.dumps(summary, indent=2) + '\n')
    report = render(summary, args.top)
    (args.output / 'profile.md').write_text(report + '\n', encoding='utf-8')
    print(report)


if __name__ == '__main__':
    main()
