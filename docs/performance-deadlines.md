# Deadline and lifetime completion — performance evidence

Baseline: merged [`7de9747`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/7de9747589a6f7a0a7765250d2fe29ba72a567d2).
Measured final code: [`24866da`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/24866daf8a0f02753487afd50606234b996de7fc).

Same unchanged benchmark harness, host and compiler for both revisions: GCC 13.3,
CMake Release (`-O3 -DNDEBUG -std=gnu++23`), AMD EPYC 9V74, Linux 6.18.44,
glibc 2.39, nine available logical CPUs. No LTO or sanitizers. Five alternating
process samples per revision, no concurrent local builds/benchmark processes.
CPU affinity/frequency are not pinned; this shared host is not a real-time target.

Values are nanoseconds per complete operation: median of five process medians,
with their observed min/max range. These ranges are not confidence intervals or
worst-case bounds. The capture uses `--features`; native timeout helper samples
are deliberately separate from inexpensive sequential cases. The same method and
limitations as [the earlier report](performance.md) apply.

## Final results

| Operation | Baseline ns [min–max] | Current ns [min–max] | Median change |
|---|---:|---:|---:|
| construct 10-job linear chain | 983.1 [954.9–1,058.8] | 1,005.2 [964.4–1,091.8] | +2.2% |
| construct 10-job fan-out (1 root + 9 leaves) | 1,148.2 [1,087.5–1,221.7] | 1,124.5 [1,113.1–1,212.4] | -2.1% |
| 10-job linear chain | 368.6 [348.6–417.8] | 357.2 [344.3–367.3] | -3.1% |
| 10-job fan-out (1 root + 9 leaves) | 325.1 [314.4–344.2] | 303.8 [302.7–312.9] | -6.6% |
| 10-job fan-in (9 roots + 1 sink) | 321.3 [313.5–357.4] | 305.8 [302.1–309.3] | -4.8% |
| 4-job diamond | 141.6 [138.7–160.5] | 133.8 [130.4–137.3] | -5.5% |
| validate 20-job chain | 106.3 [100.8–108.8] | 104.3 [103.3–104.8] | -1.9% |
| external stoppable token, no request | 603.0 [562.7–613.8] | 579.2 [552.9–596.4] | -3.9% |
| observer callbacks, no external token | 394.6 [378.4–396.3] | 378.4 [362.4–388.1] | -4.1% |
| cooperative timeout configured | 28,544.3 [26,483.3–30,493.7] | 27,007.0 [23,910.9–31,250.6] | -5.4% |
| plain timeout configured plus join | 25,591.8 [24,340.7–29,184.5] | 26,130.8 [24,248.1–36,125.7] | +2.1% |

[Final raw JSON/logs and summary](benchmarks/2026-09-24-deadlines-final/) retain all
samples. Summary medians and sample arrays were checked against the raw JSON.
Overlapping ranges and native-thread startup variance limit causal claims. This
is not evidence of universally optimal scheduling or hardware latency bounds.

## Regression investigation

The [first candidate capture](benchmarks/2026-09-24-deadlines/) at
[`1a09718`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/1a09718a313603f2b7dba899b5ec1ad03907d641)
showed roughly 9–16% slower median times in several default DAG cases. Adding
optional machinery enlarged the invocation function. Separating the untimed
invocation from timeout handling reduced the common function's generated code
size and restored fan-in, fan-out and diamond timings near baseline in the
[intermediate capture](benchmarks/2026-09-24-deadlines-refined/) at
[`3c30e6d`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/3c30e6d87cddf2f5b023e85af76c084bd7fd36f5),
although the chain still regressed. Inspection of generated code then identified
an unnecessary stop-token reference-count pair. Moving that token into the
untimed callable removed this cost while preserving its ownership and stop gate.
The final repeated capture above includes both changes. The experiment supports
the final implementation; it does not isolate every compiler/layout effect.

## Optional-feature and allocation boundaries

Untimed jobs do not consult the deadline service or create native timeout helpers.
Injected cooperative deadlines use caller-owned registration storage and avoid a
native watchdog thread. Service implementations may still allocate and incur
platform-dependent timer costs; these are not measured by the native-helper
cases above. Plain timed bodies still require a native worker and owned shared
completion state. RunScope adds one run thread and a stop state; it is explicitly
opt-in. No new universal allocation-free or constant-latency claim is made.

The benchmark does not measure allocation counts, stack depth, power, actual I/O,
Qt/Zephyr throughput, or RunScope/platform-timer latency. Fixed queue capacity and
caller-owned registration are structural bounds, not proof of a heap-free system.
Correctness is separately checked by deterministic clock tests, synchronized
lifetime tests, sanitizers and actual platform example runs.
