# Batch baseline harness

For the seeded crash/replacement mode, see [chaos experiments](chaos-testing.md).
This guide describes the no-fault baseline, which remains independently runnable.

`tests/chaos/run_batch.py` implements the no-fault baseline of the
[chaos-test contract](chaos.md). It starts real executables, submits through the
CLI, and independently checks the coordinator's retained results. It uses Python
3.9 or newer and the standard library on macOS/Linux.

## Run it

From the project root:

```sh
make test-batch
make SANITIZE=1 test-batch
```

Each command builds the selected executables and runs **one coordinator, five
workers, and 100 `sleep --args 3000 --max-retries 3` jobs**. The coordinator gets
a fresh loopback port and a fresh WAL. No existing service or WAL is used. These
are separate normal/sanitizer runs, each with its own output directory.

A default batch needs roughly a minute of execution plus process, submission,
durability, and verification overhead. It has a 180-second overall deadline,
including a ten-second cleanup reserve. These durations are bounds and workload
sizing, not benchmark results.

For a quick development check:

```sh
make test-batch BATCH_ARGS='--workers 3 --jobs 9 --sleep-ms 25'
```

To select already-built binaries or retain output at an explicit new location:

```sh
python3 tests/chaos/run_batch.py --bin-dir build/debug \
  --workers 5 --jobs 100 --sleep-ms 3000 --max-retries 3 \
  --seed 42 --fault-duration-ms 0 --deadline-ms 180000 \
  --output-dir build/chaos/my-first-baseline
```

The output override must not exist, even as an empty directory. The usual
location is a unique `build/chaos/baseline-*` directory. All run files are kept
after success or failure and are ignored by Git under `build/`. `make clean`
removes the entire build directory, including these artifacts; copy any evidence
you want to retain before cleaning.

## Configuration

| Option | Default | Accepted values |
| --- | --- | --- |
| `--workers` | 5 | 1–16 |
| `--jobs` | 100 | 1–256, including all retained terminal jobs |
| `--sleep-ms` | 3000 | 0–86400000; valid built-in sleep input |
| `--max-retries` | 3 | 0–4294967295 |
| `--seed` | 42 | 0–4294967295; recorded, unused in baseline |
| `--fault-duration-ms` | 0 | **Only zero** here; use `run_chaos.py` for faults |
| `--deadline-ms` | 180000 | 10001–2147483647; includes cleanup |
| `--bin-dir` | `build/debug` | Directory containing all three executable binaries |
| `--output-dir` | unique directory under `build/chaos/` | A new directory |

Heartbeat interval/timeout remain 2000/6000 ms, with a 200 ms polling interval.
Changing the job count or sleep duration does not increase the deadline
automatically. An oversized workload can therefore fail its deadline correctly.
Invalid arguments or unavailable executables fail before any child starts.

## What a run does

1. Start the owned coordinator with `--init-wal`. Wait for its own listening
   record, then verify PING/PONG. The ephemeral port reservation is released just
   before launch; if another process wins the bind race, fail without sending
   submissions to it.
2. Start the worker pool, correlate each worker's registration log with that
   process's PID and coordinator-issued ID, and compare against `workers`.
   Spawn order need not match registration order. Each process has a stable pool
   slot and generation zero for the future replacement step.
3. Submit jobs serially. Save the attempted input before calling the CLI, then
   save the distinct nonzero ACK ID. Missing/malformed/duplicate ACKs or a failed
   CLI make admission uncertain and stop the batch. No submission is retried.
4. Poll `jobs` and `workers` until all acknowledged jobs are DONE. Record changed
   job observations and reject lost IDs, unexpected jobs, failures, retries,
   missing workers, expiry, or an unexpected runtime exit.
5. Read every job's full `status`, then repeat the final listing and every full
   status. Compare the terminal records, pool, and `stats` while the workers are
   still alive and idle. Save the per-submission accounting report with all IDs
   in the completed partition and an empty failed partition.
6. Stop and reap all owned processes before publishing `BASELINE_PASS`.

No fault clock or random candidate plan is used in baseline mode. The seed is retained as
configuration, and the manifest explicitly records `candidate_plan=[]` and
`seed_used=false`. `BASELINE_PASS` demonstrates healthy batch execution and the
harness's accounting; it does not demonstrate worker-crash recovery.

## What counts as success

The acknowledged ID set must exactly equal the `jobs` listing. For every ID:

- State is DONE, attempt is **1**, retries are **0**, and failure is NONE.
- The retained worker ID belongs to this run's registered pool.
- Result length and complete result text match `slept_ms=N`, with `N` taken
  from the configured input. Default output is exactly 13 bytes, `slept_ms=3000`.
- Status fields agree with the listing. Once observed terminal, later records
  preserve the owner, attempt, retry allowance/count, state, failure, and result.

Every worker must finish ALIVE and idle, with no active lease. Stats must show
the submitted count equals the completed count and attempt count; queued,
assigned, running, failed, and retry counts are zero. Current-session counts
must agree with the fresh WAL, and startup recovery counters must be zero.
Worker gauges must agree with the pool. Throughput is checked against the
reported uptime and completed count, including its three-decimal rounding.
Latency is retained as an observation; this baseline does not set a latency SLA.

Parsing is strict about field names, duplicate identities, row counts, headers,
and result text. A zero command exit or a matching total alone cannot establish
success. Runtime WARN/ERROR records during work and sanitizer diagnostics also
fail the run. Shutdown worker-death warnings are expected after verification;
ERROR/sanitizer output is still checked after cleanup.

## Deadlines and cleanup

One monotonic clock starts immediately before the first child. Work stops ten
seconds before the overall deadline. Coordinator readiness has a five-second
limit, initial pool registration ten seconds, admission thirty seconds, and
each CLI/helper eight seconds, all capped by the shared work deadline.

Each child is launched directly in its own session/process group, with stdout
and stderr in files. The harness retains its process handle immediately, before
waiting for readiness or output. Files avoid a blocked child caused by a full
unread stdout pipe. Git metadata helpers and short-lived CLI commands are owned
and reaped too; source metadata is unavailable when Git or repository metadata
is unavailable, and binary SHA-256 hashes are always recorded.

The outer cleanup path runs after success, exceptions, startup/admission errors,
timeouts, SIGINT, and SIGTERM. A signal handler records a shutdown request;
it does not interrupt process creation before the handle is tracked.

Cleanup sends TERM (and CONT for potentially stopped children) to workers and
CLI/helpers, waits up to four seconds shared across them, escalates survivors,
then stops the coordinator. At six seconds it escalates remaining groups, with
ten seconds total for cleanup. It reaps each direct child and confirms its group
has disappeared, including descendants that stay in the group. A denied probe
or signal is inconclusive: ownership remains until disappearance is confirmed.
Signal denials are logged; persistent denial fails cleanup. Retired groups are
never signaled again. Repeated cleanup calls reuse the original result.

Unplanned SIGKILL during cleanup makes the run fail even if it ultimately reaps
everything. A remaining group/child also fails and is printed with its role/PID.
Cleanup errors are retained alongside the original experiment failure.

The known binaries do not daemonize. A descendant that deliberately creates a
different session escapes this process-group boundary. SIGKILL of the harness,
host loss, and OS/filesystem operations that cannot return remain outside a
userspace cleanup/deadline guarantee; an outer supervisor is needed for those
cases. The harness does not scan or kill unrelated processes by name.

## Retained evidence and exit codes

| Artifact | Contents |
| --- | --- |
| `manifest.json` | Contract, configuration, environment/Python/platform, endpoint, source revision/dirty state when available, binary paths/sizes/SHA-256 identities |
| `events.jsonl` | Monotonic elapsed times for process creation, worker registration, submission attempts/ACKs, job observations, reaping, signals, cleanup, and verdict |
| `submissions.json` | Input index, task/arguments, retry limit, acknowledged ID or null for an uncertain submission |
| Numbered `*.stdout.log` / `*.stderr.log` | Separate output for every coordinator, worker, CLI, and helper invocation; commands and PIDs are in the event ledger |
| `coordinator.wal` | This run's coordinator state |
| `drain-start.json` | Initial drain listing and queued/active IDs that must complete |
| `final-snapshots.json` | Checked job listings, full statuses, workers, and stats; written before the final history/coverage audit |
| `accounting.json` | Verified submission/terminal ID sets, counts, drain cohort, and every input joined to its full terminal status; baseline lost-attempt lists are empty |
| `summary.json` | Final verdict/exit code, first failure, totals, accounting report or null, elapsed time, cleanup errors, remaining groups, and every child's exit/reap/signal record |

Accounting now produces an explicit `submitted = completed + terminally_failed`
cross-check; baseline requires the last count to be zero. The shared
[per-job audit](chaos-testing.md#drain-and-per-job-proof) checks identity sets as
well as counts. A report confirms job accounting, while the final summary verdict
also depends on successful cleanup (and recovery coverage in chaos mode).

Exit 0 means `BASELINE_PASS`; 1 means experiment/cleanup/artifact failure;
2 means preflight/configuration failure. Handled SIGINT/SIGTERM exit 130/143
after cleanup. A storage failure can prevent complete artifact recording; it
does not bypass process cleanup or turn a failed run into a pass.

## Harness regression checks

```sh
make test-batch-harness
make SANITIZE=1 test-batch-harness
```

`tests/chaos/test_batch.py` contains 20 regression tests for real batch results and artifact accounting,
zero-length sleeps/retry boundaries, invalid preflight, ambiguous/duplicate
ACKs, a hung CLI, wrong or changed results, stats disagreement, partial startup,
deadlines, SIGINT/SIGTERM, a stopped worker, sanitizer diagnostics, owned
descendant cleanup, forced-kill failure, and idempotent cleanup. Small wrapper
fixtures deliberately corrupt replies or interrupt test processes to challenge
the verifier; these are harness regression tests, not baseline workload faults.

Both targets remain separate from `make test` and `make test-integration`, so the
existing 131 C groups and 143 integration-test counts remain unchanged.
[Linux CI](ci.md) explicitly runs the 20 `test-batch-harness` checks in both
builds; the full no-fault `test-batch` experiment remains manual. The
[chaos mode](chaos-testing.md) now implements the saved random fault plan,
bounded SIGKILL/replacement cycle, and
recovery-coverage assertions as separate targets.

## Verification record — 2026-09-29

Validated locally on macOS with Python 3.9.6 and the normal and
AddressSanitizer/UBSan build directories:

| Check | Outcome |
| --- | --- |
| `make test-batch-harness` | All 20 regression tests passed. |
| `make SANITIZE=1 test-batch-harness` | All 20 regression tests passed. |
| `make test-batch` | BASELINE_PASS: 100 acknowledged, 100 DONE, 100 attempts, zero retries/failures; five ALIVE idle workers; all 761 children reaped and their groups retired. |
| `make SANITIZE=1 test-batch` | BASELINE_PASS with the same job/worker outcomes; all 637 children reaped and their groups retired; no sanitizer diagnostics. |
| Saved-artifact audit | Submission ID sets, listings, exact full results, summary counts, and child exit/reap records agreed in both full runs. |
| Static checks | Python syntax, changed Markdown file links/fences, and `git diff --check` passed. |

The full normal run is retained locally in `build/chaos/baseline-ocsxm6fe/`
(66.953 seconds including cleanup); the full sanitizer run is in
`build/chaos/baseline-d082glg9/` (72.998 seconds). The child counts include
short-lived CLI inspections and metadata helpers; polling timing changes that
count. These are validation observations, not performance benchmarks.

Two earlier attempts recorded simultaneous coordinator heartbeat gaps of about
106 seconds, consistent with host suspension or a host-wide timing interruption.
They correctly failed and cleaned up; their artifacts remain in
`build/chaos/baseline-_3bd1r5u/` and
`build/chaos/regression-s389l4gr/run-1/`. Final validation used macOS
`caffeinate -i` around each Make command to inhibit idle sleep. Heartbeat limits,
job inputs, and pass criteria were unchanged. Idle-sleep inhibition is an
external validation aid, not a dependency or subprocess of the portable harness.

This record covers local macOS execution of the new harness. No Linux or
GitHub-hosted execution of these new targets is claimed; the existing Linux CI
checks remain separate.
