# Controlled failure recovery benchmark

The runner implements the frozen `recovery-sleep-v1`
[profile](../benchmarks/profiles/recovery-v1.json) and
[benchmark contract](benchmarks.md). It compares three scenarios using the same
optimized binaries, fresh durable coordinator state, four workers, and 32 jobs:

| Scenario | Intervention | Expected final accounting |
| --- | --- | --- |
| `no_fault` | Observe the target and scheduled fault point, without signaling | 32 DONE, 32 attempts, no retries or terminal failures |
| `sigkill` | Kill the first acknowledged job's attempt-1 worker | 32 DONE, 33 attempts, one retry, no terminal failures |
| `heartbeat_expiry` | Stop that worker for ten seconds, then resume it | Same accounting as SIGKILL, with heartbeat-based detection |

Every job is `sleep --args 10000 --max-retries 1`, with the exact expected result
`slept_ms=10000`. Hard crashes and heartbeat expiry have separate samples and
aggregates. Waiting tasks measure recovery behavior; these numbers are not
CPU throughput or a replacement for the [scaling experiment](benchmark-scaling.md).

## Results: 2026-10-02

**The complete campaign and independent artifact audit passed.** All 576 jobs
(96 warmup, 480 measured) returned exactly `slept_ms=10000`. Each of the twelve
fault runs consumed exactly one retry and completed the interrupted job under a
different worker. All six controls had zero retries. There were zero terminal
failures, missed IDs, incorrect results, deadline failures, or cleanup failures.

The [reviewed JSON](../benchmarks/results/recovery-20261002.json) retains all
unrounded measurements, matched pairs, provenance, and audit/evidence hashes.
The [CSV](../benchmarks/results/recovery-20261002.csv) includes all 18 runs and
explicitly labels warmups. The tables below exclude warmups. Each median and
range uses five measured runs; paired overhead uses the same-round control.

| Scenario | Median batch, s | Batch range, s | Median jobs/s | Median per-run mean latency, s | Median paired additional time, s | Median paired overhead |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| No fault | 80.264 | 80.186–80.358 | 0.399 | 44.852 | — | — |
| SIGKILL | 80.689 | 80.604–80.805 | 0.397 | 44.966 | 0.418 | 0.521% |
| Heartbeat expiry | 85.939 | 85.777–86.049 | 0.372 | 46.261 | 5.691 | 7.082% |

All measured batch times, in round order (seconds):

| Scenario | Round 1 | Round 2 | Round 3 | Round 4 | Round 5 |
| --- | ---: | ---: | ---: | ---: | ---: |
| No fault | 80.297 | 80.186 | 80.264 | 80.358 | 80.233 |
| SIGKILL | 80.689 | 80.604 | 80.805 | 80.686 | 80.710 |
| Heartbeat expiry | 85.777 | 85.788 | 85.962 | 86.049 | 85.939 |

Recovery delays remain separate from whole-batch cost:

| Scenario | Detection median [range], ms | Loss → reassignment median [range], s | Signal → recovered completion median [range], s | Attempts / retries / terminal failures per run |
| --- | --- | --- | --- | --- |
| SIGKILL | 11.119 [0.688–12.922] | 70.046 [70.042–70.099] | 80.066 [80.058–80.105] | 33 / 1 / 0 |
| Heartbeat expiry | 5195.880 [5189.583–5204.533] | 70.044 [70.042–70.148] | 85.260 [85.246–85.332] | 33 / 1 / 0 |

![All five samples and their medians](../benchmarks/results/recovery-20261002.svg)

The roughly 70-second reassignment delay is explained by the FIFO workload:
32 ten-second jobs fill four workers for roughly eight waves. The interrupted
first job rejoins the queue behind the remaining jobs, so its next attempt is
near the last wave. The replacement begins other queued work shortly after
registration. This waiting time is not a 70-second failure-detection delay.

Heartbeat detection is measured from SIGSTOP, while expiry is based on the
last accepted heartbeat. Some of the six-second silence window has already
elapsed when the signal is sent. The audit checks the coordinator actually
reported at least 6,000 ms of silence before revoking the connection.

A recovered job restarting its ten-second task does not necessarily add ten
seconds to the batch: other workers execute jobs concurrently. The paired batch
difference captures the overall completion cost for this fixed intervention.

## Machine, validation, and excluded campaigns

The accepted campaign ran from clean revision `5f945ed1a54d5b28f557be92748f2da7fdd4bbfb`.
Source/binary hashes and machine identity stayed unchanged. The recorded machine
was an Apple M4 (`Mac16,12`, ARM64), with 10 cores (4 Performance + 6 Efficiency),
16 GiB RAM, and an internal APFS SSD. It ran macOS 27.0.1 (26A434), Darwin 27.0.0,
Apple Clang 21.0.0, and Python 3.9.6. The build used `-O2 -g -Werror`,
`SANITIZE=0`, normal logging, and normal durable WAL synchronization.

All 36 before/after sample boundaries reported AC power with low-power mode off.
Thermal telemetry was unavailable; absence of throttling is not established.
Power and load observations are boundary checks, not continuous monitoring.

The entire invocation took 2276.388 seconds
(including build, tests, warmups, cooldowns, verification, and cleanup). Every
30-second cooldown was checked independently against raw monotonic event times.

- The fresh optimized build passed 131 C test groups and 141 integration tests;
  two default-endpoint checks skipped because the suite used automatic ports.
- All 15 recovery-harness checks passed in normal, ASan/UBSan, and fresh
  optimized builds. Sanitizer fixture times are excluded from the measurements.
- The existing benchmark, batch, and chaos regression suites passed 25, 22,
  and 35 checks during implementation.
- The independent audit verified all 9,450 directly tracked children/helpers
  were reaped with retired process groups. It reconstructed every job history,
  exact result, retry count, metric, paired comparison, and aggregate from
  retained artifacts, without importing the runner’s metric code.

Three earlier campaigns stopped because a final power check reported battery
power: `recovery-20261002/` at round 4’s no-fault sample,
`recovery-20261002-ac/` at round 3’s heartbeat sample, and
`recovery-20261002-stable/` at round 2’s SIGKILL sample. All 1,024 jobs across
32 samples returned correct results and cleanup passed, but each campaign was
invalidated by its power check. All three are **excluded from every published
performance aggregate**. The
[interrupted-attempt record](../benchmarks/results/recovery-attempts-20261002.json)
retains their per-sample accounting, failure reasons, and hashes. The accepted
campaign was run manually from Terminal into
`build/benchmarks/recovery-7yblm_u2/`. No sample was replaced or combined
across campaigns.

The accepted revision includes a verifier fix: checking the same crash diagnostic
more than once no longer consumes it twice and causes a false failure. An extra
unattributed warning still fails validation; the regression suite covers both.
This did not change coordinator or worker execution behavior.

All four raw campaign directories are local, Git-ignored evidence, including WALs,
logs, traces, and summaries. `make clean` removes them. The compact JSON, CSV,
and chart under `benchmarks/results/` are the durable repository report.

## Running the campaign

```sh
make benchmark-recovery
# Or choose a new evidence directory:
make benchmark-recovery BENCHMARK_ARGS='--output-dir build/benchmarks/my-recovery-run'
```

Automatic machine and power checks currently support macOS. Use a clean committed
checkout, AC power, low-power mode off, and an internal APFS SSD with at least
1 GiB free. The runner records machine/compiler/storage identity, power, available
thermal telemetry and load before and after each run. Unknown thermal telemetry
stays unknown. It owns an idle-sleep inhibitor during each sample.

The shared campaign driver creates a fresh Clang `-O2 -g -Werror`, non-sanitized
build, runs the core correctness suites and recovery harness regressions, and
records source and binary hashes. Builds/tests finish before measurements begin.
It runs three unreported warmups followed by five rounds in the profile's exact
order. Each round contains all three scenarios and its own no-fault control.
Thirty-second cooldowns separate runs. The complete campaign has 18 runs and
576 jobs, including 15 measured runs and 480 measured jobs. Expect roughly
35–40 minutes on the current machine, including build/tests and cooldowns.

Each run has a 600-second total deadline, with ten seconds reserved for cleanup;
the entire campaign has a two-hour deadline. Helpers, admission, registration,
and final verification also have bounded phase deadlines. Any failed run stops
the campaign and retains its evidence. Failed/slow samples are never silently
replaced or discarded. The driver also rejects changes to source, binary hashes,
or machine identity during the campaign.

## How the intervention works

`benchmarks/run_recovery.py` extends the existing benchmark/process ownership
code. Its single event loop admits jobs serially while inspecting the coordinator
log and running at most one asynchronous `jobs` query. Fault timers continue while
a submission CLI is waiting, so slow admission cannot postpone the intervention
until all submissions finish. Inspection queries use the profile's 200 ms cadence;
the event loop services log observations and fault timers between those queries.
Both query duration and actual signal lateness are retained.

The target is always the first acknowledged job's first attempt. The runner finds
its durable STARTED log, maps the coordinator-issued worker ID to an owned child,
and schedules the intervention 500 ms after observing STARTED. Immediately before
signaling it verifies the same lease is still active. It never substitutes another
job if that opportunity is missed. The control follows the same observation and
scheduled checkpoint without sending a signal.

For SIGKILL, the runner requires the child's signal exit and a matching connection
loss, durable requeue, and accepted completion on attempt 2 under another worker.
For heartbeat expiry, it confirms SIGSTOP with `waitpid(WUNTRACED)`, requires the
child to remain alive, and requires the coordinator to close that same connection
specifically because of heartbeat timeout. It preserves the timeout's silence
and threshold fields. SIGCONT occurs ten seconds after the successful stop
request. Missing expiry at that point fails coverage; the runner does not extend
the pause to obtain a passing result.

Exactly one replacement starts after the matching durable loss is observed, and
must register within ten seconds. The interrupted job joins the FIFO queue and
can be assigned to any eligible worker. A new worker does not guarantee that the
retried job is immediately next. The final pool must contain four ALIVE, idle
workers; an old DEAD registry entry may remain.

After resume, the expired worker must exit with code 1 because its connection was
revoked. Only that child's identified connection/send error and matching stopped
record are accepted. Other worker errors, unrelated timeouts, stale accepted
results, and extra retries invalidate the sample. The shared harness has explicit
hooks for these expected exits and final-log checks; normal batch/chaos runs retain
their strict defaults. Cleanup retains the CONT-before-TERM ordering and treats
unplanned forced kills or unreaped groups as failures.

## What the measurements mean

| Metric | Boundaries |
| --- | --- |
| Batch completion time | Immediately before first submission spawn to parsing the first complete terminal job listing after all ACKs |
| Observed detection delay | Pre-signal harness timestamp to observation of the matching durable worker-loss log |
| Observed reassignment delay | Observation of durable loss to observation of the target's attempt-2 assignment |
| Observed recovery-to-completion | Pre-signal timestamp to observation of accepted attempt-2 completion |
| Coordinator loss-to-assignment/completion | The same events' coordinator `monotonic_ms` timestamps, subtracted only within that process |
| Replacement launch/registration | Loss observation to replacement launch; launch to observing its new registration |
| Attempts, retries and terminal failures | Verified coordinator counters, cross-checked against every job's history and full status |
| Additional completion time | Fault batch time minus the no-fault batch time from the same measured round |
| Additional time (%) | `100 × additional time / matched control time` |

The signal's before/after syscall bracket is saved. Detection includes durable
WAL synchronization, logging, and observation delay; it is not an exact kernel
notification timestamp. Heartbeat timeout starts from the **last accepted
heartbeat**, not from SIGSTOP. Reassignment and completion can include substantial
FIFO waiting. Query observation adds overhead to batch completion time.

Harness nanosecond intervals and coordinator millisecond intervals are kept
separate. A worker's clock is never subtracted from either. The control has null
recovery-delay fields, rather than fictional zero-millisecond recovery. Paired
additional times preserve negative values caused by variation. Reports retain
all five values, median, minimum and maximum for each scenario, and source sample
indices for each comparison; warmups are excluded from those aggregates.

## Evidence and verification

Every sample retains its manifest, machine snapshots, commands, child processes,
submission/ACK ledger, query timings, signal/lease observations, exact statuses,
per-ID accounting, coordinator log transitions, latency values, WAL, stdout/stderr,
and summary. `fault.json` contains the raw recovery event timestamps. Campaign
metadata includes the profile, ordered plan, source snapshot, build/check logs,
binary hashes and aggregate report. Local evidence lives under Git-ignored
`build/benchmarks/`; `make clean` removes it.

Before a sample is accepted, the runner checks the exact acknowledged ID set,
every result byte, stable repeated terminal statuses, all durable lifecycle
transitions, retry attribution, healthy final workers, counters, and process
cleanup. A recovered job must have exactly seven transitions: submission,
assignment, start, loss/requeue, reassignment, restart, completion. Every other
job must have exactly four. This rejects an outdated attempt even when totals
and result lengths happen to look right.

```sh
make test-recovery-benchmark-harness
make SANITIZE=1 test-recovery-benchmark-harness
```

The 15 regression checks cover exact round pairing, negative overhead, repeatable
crash-diagnostic verification that still rejects extra warnings, clock
separation, missing/invalid measurements, old-attempt and result corruption,
scoped error acceptance, real no-fault/crash/expiry execution, concurrent slow
admission, insufficient expiry coverage, missed targets, wrong exact results,
deadlines, and SIGTERM during a pause. Reduced fixtures are correctness evidence,
not reportable benchmark samples. They remain separate from `make test` and CI's
fixed-seed chaos experiment.

The experiment covers one local worker failure per run, not network partitions,
multi-host deployment, CPU-bound failure costs, repeated crashes, coordinator
restart latency, or exactly-once execution. Five repetitions show observed
variation; they do not establish a strong confidence interval or tail guarantee.
