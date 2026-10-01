# Benchmark contract

Status: **contract defined; the one-worker baseline is implemented and verified.** See
[baseline execution and evidence](benchmark-baseline.md). The
[ordered scaling runner](benchmark-scaling.md) is implemented; its measurements
and the controlled recovery driver remain pending.
Contract version: `faultline-benchmarks-v1`. The versioned
[scaling profile](../benchmarks/profiles/scaling-v1.json) and
[recovery profile](../benchmarks/profiles/recovery-v1.json) freeze the numeric
settings; this document defines their interpretation and acceptance rules.
Changing workload, flags, timing, polling, fault policy, or sample selection
requires a new profile version and a complete new comparison matrix.

The MVP asks whether additional workers improve useful throughput and how much
controlled worker failure costs. These are separate experiments. Existing
[chaos checks](chaos-review.md) establish correctness and bounded recovery;
their elapsed times are not benchmark samples. The existing batch/chaos runners
remain correctness experiments. `make benchmark-baseline` now builds optimized
binaries and runs one warmup plus five one-worker measurements as a development
baseline, labelled separately from the full scaling campaign.
`make benchmark-scaling` runs that full campaign, requiring clean committed source.

## Scaling workload

| Setting | `scaling-prime-count-v1` |
| --- | --- |
| Coordinator | One, on the same host as the workers and CLI |
| Worker counts | 1, 2, 4, 8 |
| Jobs in every run | 64 identical jobs, submitted serially through the CLI |
| Task and argument | `prime_count --args 10000000` |
| Expected result | Exactly six ASCII bytes: `664579`, without a trailing NUL |
| Retry allowance | 0; every job must finish on attempt 1 |
| Failure injection | None |
| Warmups | One full, unreported run at each worker count |
| Measured repetitions | Five full runs at each worker count |

`prime_count` counts primes at or below the argument. Its current executor uses
trial division, so this exercises CPU work rather than just overlapping waits.
The expected count was independently checked with a Python sieve when defining
this contract. The bound is within the task's 100,000,000 limit. Sixty-four jobs
divide evenly across all four pool sizes and stay below the coordinator's
256 retained-job capacity. This measures fixed-workload scaling, not increasing
the work as workers are added. Current `hash` and `fibonacci` inputs are short
bounded tasks; neither is the first CPU-scaling profile.

Each run starts a fresh coordinator, WAL, and pool. Warmup jobs never share the
measured run's job store, WAL, or counters. Run the four warmups in order 1/2/4/8,
then use the following five rounds, once each:

| Round | Worker-count order |
| --- | --- |
| 1 | 1, 2, 4, 8 |
| 2 | 2, 4, 8, 1 |
| 3 | 4, 8, 1, 2 |
| 4 | 8, 1, 2, 4 |
| 5 | 1, 4, 2, 8 |

This gives 4 warmups and 20 measured runs: **1536 jobs**, of which **1280** belong
to measured runs. The first four rounds rotate order to reduce systematic order
bias. The final round is fixed too; there is no clock-selected random seed.
Pause for 30 seconds between runs, after cleanup, outside the measured interval.
This reduces immediate carryover but does not guarantee equal chip temperatures.

Warmups also check workload sizing: each must last at least ten seconds in the
batch timing defined below. If a warmup is too short, fails correctness, or times
out, stop before the measured rounds. Preserve its evidence; revise the shared
profile and restart the entire campaign instead of changing only the fast or
slow configuration. Ten seconds makes the nominal 200 ms polling interval small
relative to a batch, but query delays still add observation overhead.

## Controlled recovery workload

The independent `recovery-sleep-v1` profile fixes **four workers**, **32 jobs**,
`sleep --args 10000`, and **one permitted retry per job**. Every final result must
be exactly the 14 bytes `slept_ms=10000`. A ten-second task gives enough time to
interrupt a known active attempt and wait for heartbeat expiry. These results
describe controlled recovery of waiting tasks, not CPU recovery throughput.

Run three scenarios with identical workload, build, storage, and timing:

| Scenario | Intervention | Required outcome |
| --- | --- | --- |
| `no_fault` | None | 32 DONE, 32 attempts, 0 retries |
| `sigkill` | Kill the first acknowledged job's attempt-1 worker once | 32 DONE, 33 attempts, 1 retry |
| `heartbeat_expiry` | Pause that worker with SIGSTOP for 10,000 ms, then SIGCONT | Same counts, with loss specifically caused by heartbeat expiry |

For each fault scenario, observe the coordinator's matching `job_started` event
for that first acknowledged job, then schedule the signal 500 ms after that
observation. Admission and event observation must proceed concurrently so serial
submission cannot delay the signal until after the target finishes. Map the
lease `(job_id, worker_id, attempt=1)` to an owned worker process; record actual
signal request/return times and target identity. Do not switch to another job if
the target finishes or ownership changes. A missed interruption makes the run
invalid, even when all jobs complete. The no-fault control records the same
target/start observation and passes the same scheduled point without signaling.

Start exactly one replacement immediately after observing the matching durable
`job_worker_lost outcome=REQUEUED`, and require registration within ten seconds.
Record its new worker ID and actual delay. The retry can run on any eligible
worker; it need not run on the replacement. The surviving pool continues working
during fault handling, and all four active slots must be restored before the run
passes. The old DEAD registration can remain in `workers`; the final requirement
is four ALIVE, idle workers, not a registry containing only four records.
Recovery includes replacement launch/registration and FIFO waiting.

For SIGKILL, confirm the child's signal exit and connection-loss handling. For
SIGSTOP, confirm the process is stopped and has not exited while its TCP
connection remains open until the coordinator expires it. Require a matching
`heartbeat_timeout`/`worker_dead reason=heartbeat_timeout` before resuming it.
Resume at 10,000 ms after the successful stop signal; if expiry has not occurred,
resume for bounded teardown and mark insufficient coverage instead of extending
the pause. The six-second timeout runs from the last accepted heartbeat, not
from SIGSTOP. Detection therefore depends on heartbeat phase and scheduling.

The resumed old worker is expected to exit with code 1 because its connection
has been revoked, as in the existing [old-attempt checks](recovery.md). Permit
only that worker's attributable connection/send error and stopped record; do not
globally ignore ERROR logs. No stale attempt may change the accepted job result.
The planned SIGKILL is an expected exit; unplanned worker exits remain failures.

Use one full warmup per scenario, followed by five measured rounds in the exact
orders saved in the recovery JSON. Each round includes its own no-fault control.
That gives **3 warmups and 15 measured runs**, or 576 jobs (480 measured), with a
30-second cooldown between runs. Exactly one job retries in each fault run;
terminal failure or additional loss is invalid for this controlled profile.
Random crash rates and coordinator-restart performance require separate profiles.

## Build, durability, and machine controls

Use **Clang, `-O2 -g -Werror`, `SANITIZE=0`**, retaining the Makefile's C11 and
warning flags. No LTO, `-march=native`, CPU affinity, or disabled durability is
part of v1. All three programs come from one source revision and build. Log every
compile/link command, compiler version/path, effective flags, and binary SHA-256.
Reportable timing uses no sanitizer instrumentation. Sanitizer correctness
results remain separate evidence.

The current Makefile accepts a command-line `BUILD_DIR` override, so an isolated
optimized build can be prepared without changing its normal debug defaults:

```sh
# Use a new, empty build path for each campaign; do not clean old evidence.
make BUILD_DIR=build/benchmark-v1 CC=clang SANITIZE=0 CFLAGS='-O2 -g -Werror' all
make BUILD_DIR=build/benchmark-v1 CC=clang SANITIZE=0 CFLAGS='-O2 -g -Werror' test
```

These are build/correctness commands, not benchmark commands. Build and test
once before measurements, outside their clocks. If that path already contains a
build, choose a new path: Make does not detect arbitrary flag changes in cached
objects. Do not change binaries midway through the matrix. Keep a committed
source revision, raw Git status, and any generated-cache exceptions in provenance;
tracked edits or untracked source inputs disqualify a reportable campaign.

Keep normal WAL creation, appends and `fsync` behavior, and normal runtime logs
enabled. Use a fresh `--init-wal` on the internal disk for every run, with an
unused loopback port and direct file-backed stdout/stderr. WAL synchronization,
CLI startup, logging, scheduling, and the worker's result-reporting delay are
real costs of this configuration. Do not use a RAM disk, suppress heartbeats,
reuse retained jobs, drop caches, or include networked machines in this profile.
Use 2000 ms heartbeats, a 6000 ms timeout, and 200 ms inspection polling in both
experiments. Do not mix compiler/OS/hardware configurations into one speedup plot.

The [initial machine inventory](../benchmarks/machines/apple-m4-20261001.json)
was read on **2026-10-01**:

| Property | Recorded value |
| --- | --- |
| CPU/model | Apple M4, `Mac16,12`, ARM64 |
| Cores | 10 physical/logical: 4 Performance + 6 Efficiency |
| Memory | 16 GiB (17,179,869,184 bytes) |
| OS | macOS 26.6.2, build 25G83; Darwin 25.6.0 |
| Compiler | Apple Clang 21.0.0, `clang-2100.1.1.101` |
| Make / Python | GNU Make 3.81 / Python 3.9.6 |
| WAL filesystem | Internal SSD, APFS; container 494,384,795,648 bytes |
| Free space at inventory | Approximately 88 GiB |
| Power at inventory | Battery, 58%; low-power mode disabled in both saved power profiles |

This is an inventory, **not performance evidence or a claim of run readiness**.
Before each campaign, refresh versions and hardware/storage fields. Before each
run, require AC power, low-power mode off, no active thermal/performance warning,
at least 1 GiB of available output space, and no concurrent build/test/benchmark
controlled by the harness. Record power and available thermal/load information
before and after each run. Missing thermal telemetry means unknown; the inventory
message that no warning was recorded does not prove absence of throttling.
If AC power is lost or a warning appears, retain the run as invalid; do not silently
replace it in a table. Use an owned idle-sleep inhibitor such as `caffeinate -i`
on macOS, record interruptions/background activity, and leave OS power settings
and CPU placement unchanged. The OS can distribute work across unequal M4 core
classes, so eight workers are not eight identical dedicated performance cores.

## Timing boundaries and metrics

Use the harness's monotonic nanosecond clock for deadlines and observed timings.
UTC identifies a run; calendar time never determines an elapsed duration.
Do not subtract a worker/harness timestamp from a coordinator timestamp. All
coordinator-log intervals below use that same coordinator process's
`monotonic_ms` fields, matched by identity and WAL sequence. Missing/negative
timestamps, missing/duplicate transition records, or inconsistent ordering make
the relevant run invalid; never fill gaps with zero.

Before timing, confirm the owned listener is ready and all expected workers are
registered, alive, idle, and uniquely identified. Check the endpoint belongs to
this coordinator, including a port-bind race. No workload is submitted yet.

- **Batch start `t0`:** immediately before spawning the first submission CLI.
- **Admission end `t_ack`:** after all expected unique submission ACKs have been
  parsed. Save each request and its ACK separately; never retry uncertain admission.
- **Batch end `t_done`:** immediately after parsing the first successful `jobs`
  response that proves the exact acknowledged ID set is terminal, after all ACKs.
  Queries run at a 200 ms cadence, one at a time, with no catch-up burst when slow.
  Record actual query intervals and durations. Polling includes during admission
  for recovery observation, but it must not overlap another inspection query.
- **Verification and cleanup:** after freezing `t_done`, retrieve every full
  status, compare logs/counters, and clean up. These costs are excluded from the
  primary batch time but included in the run deadline and separately reported.
  A timing remains provisional until all correctness and cleanup checks pass.

| Metric | Definition |
| --- | --- |
| `batch_elapsed_ms` | `(t_done - t0) / 1e6`; includes serial CLI submission, ACKs, execution, persistence, and completion observation |
| `admission_ms` | `(t_ack - t0) / 1e6`; submission can overlap execution |
| `completed_jobs_per_second` | Verified DONE count divided by `batch_elapsed_ms / 1000` |
| Per-job accepted latency | Coordinator `job_completed.monotonic_ms - job_submitted.monotonic_ms` for that job |
| Mean accepted latency | Arithmetic mean across all verified jobs in the run |
| p95 accepted latency | Nearest rank: sorted job latencies at one-based index `ceil(0.95 * jobs)` |
| Speedup at W workers | Median one-worker batch time divided by median W-worker batch time |
| Worker-normalized efficiency | Speedup divided by W, reported as a percentage with the heterogeneous-core caveat |
| Correctness/recovery counts | Submitted, DONE, FAILED, total attempts, consumed retries, and actual faults |
| Run overhead | Startup, post-timing verification, cleanup, and total run elapsed time separately |

The per-job interval starts at the durable acceptance log, so it excludes the
client's initial request/ACK time. It includes queueing, computation/reporting,
retries, and the final durable commit before completion logging. Log timestamps
have millisecond resolution and are emission times, not CPU-time measurements.
Batch time includes polling and query delay and is an observed upper bound on
when all work finished; 200 ms is a target interval, not a strict error bound.
The existing `stats` throughput uses whole-session uptime, and its latency uses
the durable logical timeline. Preserve them as diagnostics; neither substitutes
for the benchmark's explicitly bounded metrics.

For each recovery fault, also record these harness-clock observations:

1. Before/after the actual signal syscall (its delivery-time bracket).
2. Receipt of the matching durable worker-loss record.
3. Receipt of the interrupted job's new assignment and STARTED records.
4. Receipt of that job's accepted completion record.

Report **observed detection delay** as 2 minus the pre-signal time, **observed
reassignment delay** as 3's assignment observation minus 2, and **observed
recovery-to-completion** as 4 minus the pre-signal time. Include the syscall
bracket and polling/query delays; these are not exact kernel detection times.
Also report coordinator loss-to-assignment and loss-to-completion intervals from
its own log clock. Recovery-to-completion includes FIFO waiting and re-execution,
not just failure detection. The durable loss observation includes requeue WAL
synchronization and log/observer delay, so it is not pure detector latency.
Retain the heartbeat-timeout record's silence/timeout fields as supporting data.
Keep SIGKILL and heartbeat-expiry results separate.

Compare each fault sample to the no-fault sample in the same measured round:
`extra_ms = fault_batch_ms - control_batch_ms` and
`overhead_percent = 100 * extra_ms / control_batch_ms`. Preserve negative values
caused by variation; never clamp them. Summarize per-scenario paired differences.

## Acceptance, repetition, and evidence

Every run must establish the exact submitted ID set, independently checked
results, valid owners/attempts/retry limits, terminal-record stability, matching
list/status/stats/log histories, and a final healthy idle pool. Fresh-WAL startup
recovery counters must be zero. No-fault runs require no retries or failures.
Fault runs require exactly the specified interrupted lease, one attributed retry,
and its accepted completion under a different worker ID. All jobs still complete;
ordinary retry exhaustion may be valid chaos behavior but is not a valid sample
of these controlled benchmarks.

After `t_done`, query every full status, then repeat the final listing and full
statuses before stopping workers. Check exact bytes, not only lengths or totals.
Missing IDs, wrong results, uncertain ACKs, malformed output, unexpected process
exits/errors, unexpected heartbeat losses, inadequate fault coverage, invalid
timing, timeouts, and cleanup errors all disqualify the run. Expected fault events
must be narrowly attributed; unrelated warnings/errors cannot be waived.

Keep all warmup and measured attempts. A failed sample stops that campaign after
cleanup. Investigate and start a new complete campaign with a new ID; do not
retry until five good samples appear, drop slow valid runs, or pool results from
incomplete campaigns. Keep failures visible even when a later campaign passes.
For each configuration publish all five raw values, median, minimum, and maximum
batch time/throughput. Summarize the five per-run mean/p95 latency values instead
of merging correlated jobs into an apparent larger sample. Five runs do not
justify a strong confidence claim or a p99 latency claim. No minimum speedup is
a pass condition; bottlenecks and slower configurations are findings to explain.

Every campaign gets a new directory under Git-ignored `build/benchmarks/`, with
one subdirectory per warmup/measured run. Never overwrite an existing directory.
Retain on success and failure:

- Contract/profile copies and hashes, ordered run plan, machine/power snapshots,
  source revision/status, exact commands, build log, and binary hashes.
- Submission/ACK ledger, timestamped event trace, signal targets/actions, process
  ledger, full status/list/stats snapshots, per-ID accounting, and raw metrics.
- Summary with verdict/first failure/cleanup result, coordinator WAL, all process
  and outer harness logs, and any unpublished `.json.tmp` files.

Metadata fields not available on a platform are explicitly unknown, not zero.
Preserve raw timestamps and derive rounded display values afterwards. The final
campaign report must link each aggregate to its five source samples. Later
publication can copy reviewed reports/plots to `benchmarks/results/`; the
[one-worker baseline report](../benchmarks/results/one-worker-20261001.json) is
available, separately scoped from the full matrix. `make clean` removes local
build evidence, so copy evidence
before cleaning. Benchmark execution/uploads are not added to Linux CI by this
contract; hosted correctness checks remain separate.

## Deadlines and ownership

Each profile campaign has a **two-hour absolute monotonic deadline**, including
warmups, cooldowns, measurements, verification, and cleanup. It begins immediately
before the first warmup child. A run has **600 seconds**, beginning immediately
before its first owned child, including setup and teardown. Its effective end is
the earlier of its own limit and the campaign deadline. Reserve the final **ten
seconds** for cleanup: no work, spawn, query, or cooldown can extend that limit.
An earlier failure begins cleanup immediately with at most ten seconds remaining.
Do not start another run unless the reserve fits in the remaining campaign budget.

All waits are capped by that shared work deadline, plus these phase maxima:
coordinator readiness **5 s**, initial pool registration **10 s**, admission
**60 s**, each CLI/helper **8 s**, replacement registration **10 s**, and final
verification **60 s**. Fault timers and sleeps also use the remaining budget.
Timeout is a failed run, not a slow sample silently omitted from the report.
Build/correctness checks run before the campaign and retain their own failures.

Use the existing [process-ownership contract](chaos.md#process-ownership-and-cleanup):
track handles immediately, launch each child in an owned session/group, and reap
every coordinator, worker generation, CLI, metadata helper, and sleep inhibitor.
Only signal still-owned processes/groups; never use name-based or global kills.
One idempotent cleanup path handles success, exceptions, partial startup,
SIGINT/SIGTERM, and deadlines. Stop spawning/fault injection before teardown.

Within the shared ten-second reserve, TERM workers/helpers and CONT stopped
children; allow at most four seconds, then KILL survivors and TERM the coordinator.
At six seconds KILL remaining owned groups, and reap/confirm disappearance by ten.
Record each cleanup error without hiding the original failure. Retired groups
are never signaled again. An unplanned forced kill during cleanup, unreaped child,
or remaining/inaccessible group disqualifies the sample even when all jobs are
DONE. Final PASS waits for successful cleanup. A planned fault's signal is not a
cleanup error. Host loss, harness SIGKILL, uninterruptible OS calls, and descendants
that escape their owned session remain outside this process-based guarantee.

## Implementation sequence

1. **Defined:** these profiles, timing/acceptance rules, and initial machine record.
2. **Verified:** the [one-worker baseline](benchmark-baseline.md), with
   per-ID verification, timing, retained evidence, deadlines, and cleanup.
   All 384 jobs passed; median measured batch duration was 59.395 seconds.
3. **Implemented; measurements pending:** run the complete optimized
   [scaling campaign](benchmark-scaling.md) with the fixed worker-count matrix.
4. Implement and verify the controlled recovery scenarios, then run their campaign.
5. Audit all IDs/results, raw samples, and provenance; publish tables/graphs and
   explain workload, machine, polling, durability, and scheduling limitations.

This defines the experiments without adding production behavior or claiming
performance. Multi-host speedup, maximum coordinator capacity, random crash-rate
sweeps, coordinator-restart timing, and performance regression gates need their
own profiles and evidence.
