# Seeded worker-crash experiments

`tests/chaos/run_chaos.py` extends the [batch harness](batch-testing.md) with the
SIGKILL/replacement cycle from the [chaos contract](chaos.md). It uses Python's
standard library to drive the real executables. The coordinator stays alive.

## Run an experiment

```sh
make test-chaos
make SANITIZE=1 test-chaos
```

Each command starts **five workers**, submits **100 three-second sleep jobs**
with three retries allowed, and uses **seed 42** for a **30-second fault window**.
The window starts after submission acknowledgments and pool readiness. Fault
spacing is 2000–10000 ms. The 180-second overall deadline includes ten seconds
reserved for cleanup. Heartbeat interval/timeout remain 2000/6000 ms.

A smaller experiment with backlog for one crash:

```sh
make test-chaos CHAOS_ARGS='--workers 2 --jobs 6 --sleep-ms 2000 --seed 1 --fault-duration-ms 4000 --deadline-ms 30000'
```

Use `CHAOS_ARGS` for this target; `BATCH_ARGS` still configures `test-batch`.
Direct invocation supports the same binary/output directory and workload options:

```sh
python3 tests/chaos/run_chaos.py --bin-dir build/debug --seed 42 \
  --fault-duration-ms 30000 --output-dir build/chaos/my-seed-42-run
```

Every run gets a fresh loopback endpoint, WAL, and output directory. An explicit
output path must not exist. The default directory starts with `build/chaos/chaos-`.
Artifacts remain on success and failure, are ignored by Git, and are removed by
`make clean` unless copied elsewhere first.

`--fault-duration-ms 0` selects baseline rules and BASELINE_PASS. `run_batch.py`
still rejects nonzero windows. Other limits match
[baseline configuration](batch-testing.md#configuration). Chaos windows accept
0–20000000 ms, capping plans at 10000 candidates. The overall deadline must exceed
the window plus 10000 ms. After admission, the full window must still fit the
remaining work budget; it is never silently shortened.

These targets remain opt-in, outside `make test`, `make test-integration`, and CI.

## Seed and schedule

Before launching children, a private `random.Random(seed)` prepares
`floor(fault_duration_ms / 2000)` candidates. Each contains an index, an integer
delay in 2000–10000 ms, and a shuffled order of stable pool slots. The manifest
saves the entire plan, seed, Python version, and generator name. Polling, errors,
and replacements make no extra random draws. For seed 42 with five workers, the
first candidate has gap 7238 ms and order `[3, 1, 2, 4, 0]` in the validated generator.

The first delay starts at window start. Later delays start after the previous
crash/replacement cycle completes or selection is skipped. Only one cycle runs
at a time. A delay reaching or exceeding the cutoff leaves an unused plan suffix,
which is recorded. The full window is maintained even if all jobs finish early.

The same seed/configuration reproduces the **candidate plan** under the recorded
generator version. It does not reproduce scheduling, IDs, exact timings, busy
worker eligibility, or which job is lost in a race. Keep the actual trace and
binary identities too. There are no automatic reruns that discard failing evidence.

## Crash and replacement

1. Read `jobs` and `workers`. Walk the slot order and select its first busy,
   registered, live worker owned by the harness. Skip when no worker is busy.
2. Record candidate, slot, generation, PID, worker ID, job/attempt, and snapshots.
   If snapshots crossed an assignment boundary, record `ASSIGNED_OR_RUNNING`
   instead of inventing a more precise state.
3. Recheck the cutoff and send SIGKILL through the owned process handle. Require
   exit `-SIGKILL`, reap the child, and confirm its process group is gone.
4. Account for coordinator death/close records and any job-loss transition within
   five seconds, capped by the shared work deadline.
5. Start a replacement in the same slot with its generation incremented. Require
   registration within five seconds and an ID never used earlier in this run.
   Confirm the full live pool through `workers` before starting the next gap.

The pool slot is a test position; the coordinator-issued ID identifies a specific
worker registration. A socket descriptor is diagnostic context, never identity
or the target used to send a signal. Old process handles are retired after reaping.
No new kill occurs after the cutoff. A cycle already in progress can finish its
loss accounting and replacement afterward, before drain eligibility is determined.

## Evidence of an interrupted attempt

Coordinator assignment, STARTED, completion, loss, and death logs maintain a
separate active-lease history. A worker can finish the observed job just before
SIGKILL and possibly receive another job. Each action keeps both the pre-signal
observation and the actual lease from `job_worker_lost`. A busy snapshot alone
earns no recovery credit. Death without the required active-job loss record fails.

Expected crashes can produce EOF, reset/broken-pipe, or truncated-stream logs.
Only close records for the killed registration are accepted. Transport warnings
must match that crash's close reason and coordinator timestamps. Unmatched warnings,
heartbeat expiry, task failures, protocol rejection, and unplanned exits fail.
The loss record's WAL sequence and `durable=1` describe the accepted transition;
logs remain diagnostic evidence under the existing persistence/recovery contracts.

## Accounting and verdicts

The final ID set must equal all acknowledged IDs. Each terminal job satisfies
`attempt = retry_count + 1` inside its allowance. DONE requires the exact
`slept_ms=N` result and failure NONE. FAILED is allowed only for WORKER_LOST after
exhausting retries, with no result. Every retry and terminal failure must match
an actual crash loss, including job and attempt identities.

Queued/active jobs remaining after the last cycle settles form the drain cohort
and must finish DONE. Previously observed terminal records must remain unchanged,
including full results. Status reads, listings, stats, and worker gauges must agree.
The summary separates `verified_completed` and `verified_failed`; statistics use
actual terminal counts and sums of attempts/retries rather than baseline assumptions.

CHAOS_PASS requires at least one killed worker and a coordinator-confirmed
interrupted attempt. With positive retries, at least one interrupted job must
finish DONE on a higher attempt. With zero retries, an attributable terminal
WORKER_LOST failure is required instead. Cleanup must succeed with no unexpected
runtime or sanitizer diagnostics.

Correct results without sufficient interruption evidence produce
`INSUFFICIENT_COVERAGE` in the failure and summary coverage status, and exit 1.
Even valid exhaustion of every job cannot demonstrate successful reassignment
when a positive retry allowance was used.

## Artifacts and cleanup

The [baseline artifacts](batch-testing.md#retained-evidence-and-exit-codes) remain,
with these additions:

| Artifact | Added evidence |
| --- | --- |
| `manifest.json` | Mode, seed usage, complete candidate plan, generator/Python version |
| `events.jsonl` | Scheduled candidates, selections, skips, signals, actual losses/deaths/closes, transport diagnostics, replacements, window settling, drain cohort |
| `fault-actions.json` | Completed cycles, rewritten after each replacement is ready |
| `summary.json` | All actions including an incomplete last cycle, seed, window times, unused suffix, drain IDs, coverage, completed/failed counts |
| Worker logs | Separate files for every slot/generation; earlier generations remain |
| Child ledger | `intentional_crash` distinguishes expected fault-phase SIGKILL from an unexpected exit or cleanup escalation |

Every generation, failed replacement, CLI, and helper stays owned until reaped.
Ctrl+C/SIGTERM uses the shared cleanup path even during replacement. Intentional
fault-phase SIGKILL exits are accepted; unplanned forced kills during cleanup
still fail. There is no process-name scanning or attachment to existing services.
Baseline limits concerning host loss, SIGKILL of the harness, and descendants
escaping process groups still apply.

Exit 0 means CHAOS_PASS or explicitly selected BASELINE_PASS; 1 means experiment,
coverage, or cleanup failure; 2 means invalid configuration before spawning.
Handled SIGINT/SIGTERM exit 130/143 after cleanup. Retain summary, manifest, events,
logs, and WAL together when investigating an incomplete or failed experiment.

## Regression checks

```sh
make test-chaos-harness
make SANITIZE=1 test-chaos-harness
make test-batch-harness
make SANITIZE=1 test-batch-harness
```

The 18 chaos checks cover seed plans/private RNG state, snapshot races, loss/death
correlation, warnings, retry/exhaustion accounting, real SIGKILL and replacement,
zero retries, insufficient coverage, zero-window baseline, failed replacement,
SIGTERM during replacement, window-fit validation, and reaping all generations.
The 20 baseline checks remain a separate suite.

Next comes review across the planned seeds `42, 7, 2026` and remaining phase
acceptance cases before adding a bounded chaos run to Linux CI.

## Verification record — 2026-09-29

Local macOS/Python 3.9.6 validation used normal and AddressSanitizer/UBSan builds.
Make commands ran under `caffeinate -i` to inhibit idle sleep; this external
validation aid is not a dependency or subprocess of the portable harness.

| Check | Normal | ASan/UBSan |
| --- | --- | --- |
| Baseline regression suite | 20 passed | 20 passed |
| Chaos regression suite | 18 passed | 18 passed |
| Default seed-42 profile | CHAOS_PASS | CHAOS_PASS |
| Acknowledged / DONE / FAILED | 100 / 100 / 0 | 100 / 100 / 0 |
| Crashes / replacements / interrupted attempts | 5 / 5 / 5 | 5 / 5 / 5 |
| Total attempts / retries | 105 / 5 | 105 / 5 |
| Final workers | 5 ALIVE and idle | 5 ALIVE and idle |
| Cleanup | All 570 children reaped; groups retired | All 492 children reaped; groups retired |

The full normal artifacts are in `build/chaos/chaos-vc01icu0/` (70.141 seconds),
and sanitizer artifacts in `build/chaos/chaos-w5c5r5df/` (80.164 seconds). Each
run used replacement IDs 6–10, had no unexpected exits or sanitizer diagnostics,
and recorded every SIGKILL inside its fault window. Child counts include CLI and
metadata processes; elapsed times are observations, not benchmark guarantees.

An independent saved-artifact audit compared acknowledged ID sets, exact results,
attempt/retry totals, signal cutoffs, and child exits/reaping. The candidate plans
matched between builds, while recovered job IDs differed: normal
`19, 26, 37, 44, 57`; sanitizer `24, 31, 42, 44, 60`. This demonstrates why the
seed reproduces choices rather than execution scheduling. Complete traces remain
alongside the manifests and WALs.

The final regression rerun also covered anchoring the first gap directly to
fault-window start. Python syntax, changed Markdown links/fences, and
`git diff --check` passed. This record claims local macOS execution; new chaos
targets have not been executed by Linux CI or GitHub-hosted runners here.
