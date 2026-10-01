# One-worker benchmark baseline

`benchmarks/run_baseline.py` implements the first runnable part of the
[benchmark contract](benchmarks.md): one worker, 64 `prime_count(10000000)` jobs,
one warmup, and five measured samples. There are no injected failures or
permitted retries. Every result must be exactly `664579`.

This is a **one-worker development series**, not the complete ordered scaling
campaign. Its summaries set `reportable_scaling_campaign=false`. It validates the
runner and provides an initial reference. The later 1/2/4/8-worker campaign must
collect all configurations together in the specified order, including fresh
one-worker samples; these samples cannot substitute for that campaign's column.

## Results: 2026-10-01

**The complete one-worker baseline passed.** The median batch duration was
**59.395 seconds** for 64 jobs, with median throughput **1.078 jobs/second**.
All **384 jobs** (64 warmup, 320 measured) returned exactly `664579`, on attempt 1,
with zero retries or terminal failures. Every run passed cleanup and its deadline.

The [reviewed result file](../benchmarks/results/one-worker-20261001.json) retains
unrounded metrics, per-sample machine/power snapshots, timing boundaries,
accounting totals, build/source identity, and evidence hashes. Measurements below
are rounded for display; aggregates exclude the warmup.

| Measured sample | Batch seconds | Jobs/second | Mean accepted latency, seconds | p95 accepted latency, seconds |
| --- | ---: | ---: | ---: | ---: |
| 1 | 57.804 | 1.107 | 28.183 | 53.319 |
| 2 | 58.978 | 1.085 | 28.901 | 54.475 |
| 3 | 59.426 | 1.077 | 29.277 | 54.972 |
| 4 | 60.086 | 1.065 | 29.539 | 55.556 |
| 5 | 59.395 | 1.078 | 29.168 | 54.811 |
| Median of five samples | **59.395** | **1.078** | **29.168** | **54.811** |

Batch times ranged from 57.804 to 60.086 seconds. The separately verified warmup
took 58.014 seconds. The full invocation took 734.869 seconds, including building,
correctness tests, setup, cooldowns, verification, and cleanup. That wall time is
not the batch metric. Accepted latency includes FIFO waiting; its p95 describes
the 61st sorted latency among each run's 64 jobs. The table reports the median of
the five per-run p95 values, not a percentile pooled across all runs.

This series used clean source revision
`7076a9b6b5f4085ea85fd90e0e1beab06360e113`, Clang `-O2 -g -Werror`, and
`SANITIZE=0`. Source and binary hashes stayed unchanged throughout the series.
Refreshed machine values were stable across all twelve before/after snapshots:

| Property | Value for this series |
| --- | --- |
| CPU/model | Apple M4, `Mac16,12`, ARM64 |
| Cores | 10 physical/logical: 4 Performance + 6 Efficiency |
| Memory | 16 GiB |
| OS | macOS 27.0.1, build 26A434; Darwin 27.0.0 |
| Compiler | Apple Clang 21.0.0, `clang-2100.3.34.2` |
| WAL storage | Internal APFS SSD |
| Power | AC at every checked boundary; low-power mode off |
| Thermal telemetry | Unavailable; absence of throttling is not established |

OS/compiler values differ from the original contract inventory. These refreshed
values describe this run. Background load varied: the recorded one-minute load
reached 9.694 before sample 3. Its cause was not measured, and these observations
do not isolate thermal, scheduling, or background-work effects. All five samples
are retained. This remains a single-host development baseline; collect fresh
one-worker samples within the later ordered matrix before making speedup claims.

Raw evidence is retained in Git-ignored
`build/benchmarks/one-worker-379xz48p/`. A separate audit recomputed batch/latency
aggregates from timestamps and coordinator logs, checked every acknowledged ID
and exact result, reconciled counters, verified source/binary hashes and process
cleanup, and produced `independent-audit.json` with `AUDIT_PASS`. Its script is
retained there as `audit_one_worker.py`. A post-run process-group check confirmed
all 4,671 directly tracked children/helpers were reaped and their groups absent.

Validation evidence:

| Check | Result |
| --- | --- |
| Optimized C correctness tests, rerun before this series | 131 groups passed |
| Process integration tests, rerun before this series | 141 passed; two default-endpoint checks skipped |
| Benchmark verifier regressions, prior implementation validation | 18 passed |
| Existing batch verifier regressions, prior implementation validation | 21 passed |
| Existing chaos verifier regressions, prior implementation validation | 35 passed |
| Full one-worker series and independent audit | Both passed |

Regression test timings are not benchmark samples. No new sanitizer experiment
was run for these optimized timings.

## Earlier attempts

The earlier `one-worker-20261001-1840` invocation exposed an absolute build-path
issue before any benchmark jobs ran: existing Make test recipes prefix executable
paths with `./`. The driver now passes a path relative to its explicit Make
working directory, and a regression exercises both compilation and execution
with an absolute output directory. The next `one-worker-20261001-1850` invocation
passed build/correctness checks, then stopped at the warmup power gate on battery
power with zero submissions, no accepted timing, and successful cleanup. Both
unsuccessful invocations remain under Git-ignored `build/benchmarks/`. After AC
power was supplied, the successful series ran afresh from the committed code in
a new directory. No sample within a series was silently replaced or averaged away.

## Run it

On macOS, connected to AC power with low-power mode off:

```sh
make benchmark-baseline
# Or select a new directory:
make benchmark-baseline BENCHMARK_ARGS='--output-dir build/benchmarks/my-baseline'
```

The output path must not exist. The default is a unique directory beneath
`build/benchmarks/`. The driver creates fresh optimized binaries inside it,
using Clang, `-O2 -g -Werror`, and `SANITIZE=0`, then runs `make test` against
those binaries before collecting samples. Actual commands and both output streams
are retained. Inherited Make/compiler flag overrides are removed from the build
child's environment. Core integration tests use their automatic ports, so
default-port-only checks may skip without claiming an existing port 9000.

No existing build, WAL, coordinator, or worker is reused. Workload, worker count,
repetitions, flags, and deadlines cannot be overridden on the command line.
Machine/power collection currently uses macOS tools; the driver rejects Linux
before starting children. The verifier's regression fixtures work on macOS/Linux.

## Execution and accounting

1. Save the Git revision/status/patch, copy and hash the relevant C, Python, Make,
   and JSON inputs, and retain the contract/profile/ordered plan. Copied source
   also captures uncommitted inputs; when present, the base commit alone does
   not describe the experiment.
2. Build and test outside sample timing. Failure stops the series.
3. Start an owned idle-sleep inhibitor and refresh hardware, OS, storage, power,
   thermal/load information, and free space for the sample.
4. Start an owned coordinator on a fresh loopback port with a new WAL. Correlate
   its listening record with its PID, verify PING/PONG, and register one idle worker.
5. Submit all 64 jobs serially, saving every request and unique positive ACK ID.
   Uncertain admission stops the run; no submission is automatically retried.
6. Poll the exact job ID set until DONE, at a nominal 200 ms cadence without
   catch-up bursts. Record each jobs-query start/end timestamp.
7. Read every full status twice, repeat the terminal listing, verify exact bytes,
   reconcile stats and coordinator log histories, and refresh machine/power data.
8. Stop and reap all owned processes before accepting the sample. Cool down for
   30 seconds before a fresh run. The warmup must take at least ten seconds.

The series runs 384 jobs: 64 warmup jobs and 320 measured jobs. Every job must
finish on attempt 1 with zero retries/failures; the worker must be ALIVE and idle
at verification. A failed sample stops the series and is never replaced.

Power and thermal/load conditions are sampled at run boundaries, not continuously.
Unknown thermal telemetry is explicit. Boundary checks cannot detect a brief
unplug/replug or thermal event between snapshots; known interruptions must still
be recorded. An observed AC loss, low-power mode, warning, or insufficient disk
space invalidates the run. Source and binary hashes must stay unchanged.

## Timing and evidence

`timings.json` retains monotonic nanoseconds before the first submission CLI,
after the last parsed ACK, after the first parsed listing proving every job DONE,
and after final verification. Batch time is the first-to-DONE interval; throughput
is 64 divided by that duration in seconds. Startup, verification, and cleanup
are reported separately. Polling/query delay is part of observed batch time.

Each coordinator job must have exactly one submission, assignment, STARTED, and
completion record, in order, from the owned PID, with correct owner, attempt,
result, and increasing WAL sequences. Per-job latency uses that coordinator's
completion-minus-submission log timestamps. Mean and nearest-rank p95 are saved.
No clocks from different processes are subtracted. Existing session statistics
are retained as diagnostics, not substituted for these bounded metrics.

A duration is provisional until accounting and cleanup pass. Failed runs have
`timing_valid=false` even when their jobs completed. Only exactly five successful
measured samples can produce aggregates: all five raw values plus median/min/max
batch time, throughput, mean accepted latency, and p95 accepted latency. Speedup
requires the later multi-worker matrix.

The series retains `profile.json`, `contract.md`, `plan.json`, `provenance.json`,
`source-inputs/`, `build.json`, helper/build/test/outer logs, and binary hashes.
Each `00-warmup/` and `01-measured/` through `05-measured/` subdirectory retains
manifest, machine snapshots, ACK ledger, trace, query timings, per-job latencies,
accounting, statuses/listings/stats, WAL, process logs, and cleanup/summary records.
The parent `summary.json` lists attempted runs and aggregates only on success.
Raw evidence is Git-ignored under `build/`; `make clean` removes it. Reviewed
reports can be retained separately under `benchmarks/results/`.

## Deadlines and process ownership

Runtime ownership and cleanup reuse `BatchRun`. Its listing verifier has a
task-name hook, defaulting to `sleep` for existing chaos runs; the benchmark
provides `prime_count`. Original task names remain in observations and snapshots.
No C runtime, protocol, or WAL behavior changes are required.

Each sample has 600 seconds including a ten-second cleanup reserve. The parent
adds a stricter two-hour limit on the entire development invocation, including
source collection, build/tests, cooldowns, and teardown. Build and correctness
commands have 120- and 600-second caps; metadata/CLI commands have eight-second
caps. Admission and final verification each have 60-second caps, always bounded
by the remaining sample budget. SIGINT/SIGTERM enter the same bounded cleanup.
Every runtime process, helper, and idle-sleep inhibitor has a tracked handle and
owned process group. A cleanup failure invalidates otherwise completed work.

## Regression checks

```sh
make test-benchmark-harness
```

The suite now has 25 checks, including the [scaling extensions](benchmark-scaling.md).
Checks cover isolated builds with absolute output paths, exact real results,
timing boundaries, log identity/clock/WAL
ordering, nearest-rank p95 and sample aggregation, unavailable/bad power conditions,
missing IDs hidden by equal totals, wrong same-length results, duplicate ACKs
without resubmission, unexpected retries, undersized warmups, corrupted timing,
cleanup failure after completion, deadline expiry, and SIGTERM during execution.
Process fixtures verify retained failure evidence, reaped children, and empty
owned process groups. Their small workloads are marked as regression fixtures.
