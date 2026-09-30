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

### Drain and per-job proof

Once the fault window expires, no more SIGKILLs are injected. Any final crash
cycle finishes its loss accounting and replacement registration before drain.
The first drain listing is saved in `drain-start.json`, along with the exact
queued/active IDs that remain eligible. Those IDs must all finish DONE before
the existing work deadline. A timeout fails the experiment; it never converts
unfinished jobs into terminal failures to make the totals balance.

After every ID is terminal, the harness reads each full status twice and checks
the final worker pool and statistics. It then writes `accounting.json`, joining
each original submission to its checked terminal status and coordinator-confirmed
lost attempts. The report contains:

- `counts`: submitted, completed, and terminally_failed.
- `submitted_ids`, `completed_ids`, and `terminally_failed_ids`: exact ID sets,
  sorted for readability. Completed and failed sets must be disjoint, and their
  union must equal the ledger's distinct acknowledged IDs.
- `drain_eligible_ids`: the cohort saved at the start of drain, all now DONE.
- `jobs`: entries in submission order, each with its original input/ACK,
  full terminal status, and lost-attempt records including worker ID, attempt,
  REQUEUED/FAILED outcome, and WAL sequence.

For example, acknowledging IDs `{101, 205, 309}` and later observing
`{101, 205, 999}` must fail even though both sets contain three jobs. The error
names `missing=[309]` and `unexpected=[999]`. IDs need not be consecutive or
ordered by submission. Duplicate IDs are rejected rather than silently collapsed
when comparing identity sets.

Only after that identity check does the numeric equation provide a useful
cross-check:

```text
submitted = completed + terminally_failed
```

DONE also requires the exact expected result text, not just its length. For
this profile, `sleep --args 3000` must produce `slept_ms=3000`. Each terminal
job must have `attempt = retry_count + 1`, with retries bounded by its allowance.
A FAILED job must exhaust that allowance, report WORKER_LOST, and contain no
result. A three-retry allowance permits four total attempts, not three.

The attempt history is checked separately from the final counters: every retry
needs a matching REQUEUED loss for the same job and attempt, and an exhausted
failure needs one FAILED loss for its last attempt. Gaps, duplicates, extra
losses, and losses naming an unacknowledged ID fail accounting. This catches a
plausible final counter that is unsupported by the recorded experiment.

The report is copied into `summary.json` under `accounting` and a successful
audit emits `accounting_verified` in `events.jsonl`. If accounting is not reached
or fails, that summary field is null and no verified report is written. Earlier
CLI logs, observations, and the submission ledger remain available to diagnose
the failure. A valid accounting report can coexist with a failed experiment:
coverage or process cleanup may still fail, so use the final summary verdict.

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
| `drain-start.json` | First post-fault listing, elapsed time, and exact eligible IDs |
| `accounting.json` | Verified terminal partition and per-submission result/retry evidence |
| `summary.json` | All actions including an incomplete last cycle, seed, window times, unused suffix, drain IDs, coverage, completed/failed counts, accounting report or null |
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

The 32 chaos checks cover seed plans/private RNG state, snapshot races, loss/death
correlation, warnings, retry/exhaustion accounting, real SIGKILL and replacement,
zero retries, insufficient coverage, zero-window baseline, failed replacement,
SIGTERM during replacement, window-fit validation, and reaping all generations.
The accounting checks include equal-sized but different ID sets, duplicated ACK
ledgers, missing/swapped status IDs, wrong same-length results, invalid retry
limits, incomplete/duplicated loss histories, unknown loss IDs, and drain-time
failures. Real-process fixtures substitute a listing ID after faults stop,
return an unknown status, and force drain to miss its deadline; each must fail
and still reap every owned process. Successful live runs check the saved report
against submissions, final snapshots, and actual fault actions, including zero
retries with one allowed terminal failure.
The 20 baseline checks remain a separate suite and also verify the new artifacts.

The [phase review](chaos-review.md) records the full `42, 7, 2026` seed matrix,
sanitizer/regression evidence, reproduction commands, and limits. A bounded
chaos run in Linux CI remains a separate follow-up.

## Linux harness regression verification

Verified on 2026-09-30 against committed source
`a3557199abd57571e6db957686f840f234ce6413`. Both harness suites passed unchanged
in separate local Ubuntu 24.04.5 ARM64 containers, running as UID/GID 1000 with
external networking disabled. Tests used loopback, fresh builds, and the CI
compiler selections. Host Linux kernel: `6.12.76-linuxkit`; GNU Make 4.3;
Python 3.12.3.

| Suite | GCC 13.3.0 | Clang 18.1.3 + ASan/UBSan |
| --- | --- | --- |
| `test-batch-harness` | 20 passed (28.770 s) | 20 passed (28.423 s) |
| `test-chaos-harness` | 32 passed (58.055 s) | 32 passed (58.020 s) |

All **104 test executions passed**, with zero skips, compiler warnings, or
unexpected sanitizer findings. Both builds used `CFLAGS='-O0 -g -Werror'`.
The sanitizer run enabled leak detection and fail-fast diagnostics:

```sh
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18
```

To reproduce with those toolchains installed, run in fresh Linux checkouts:

```sh
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-batch-harness test-chaos-harness
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-batch-harness test-chaos-harness
```

The container runner additionally bounded compilation to 180 seconds and each
suite to 240 seconds, with a 15-second forced-stop grace. Neither bound was hit.
Both container exits were zero. Logs, suite exit codes, and 26 run summaries per
build were copied out before removing the stopped test containers. Every saved
run summary had no remaining children/groups and recorded all direct children
reaped with their groups retired. Expected failure fixtures remain preserved;
their rejection is part of the passing regression suite.

Local evidence is retained under `build/linux-harness-20260930-LHBZrF/`:
`gcc-evidence/` and `sanitize-evidence/` contain environment/build/suite logs;
the corresponding `*-artifacts/` directories contain raw harness evidence.
`results.json` records the checked suite outcomes, log hashes, container image
identity, and run-summary index. `review-configuration.json` identifies the
source revision and archive SHA-256. The read-only source export omitted `.git`,
so those external provenance records identify the source when per-run Git
metadata is unavailable. The Dockerfile and runner script are retained alongside
the logs. These files are Git-ignored and removed by `make clean`.

This establishes **local Linux ARM64 harness-regression evidence**, including
the suites' small real-process crash/replacement cases. It does not rerun the
full 100-job three-seed matrix on Linux, run GitHub-hosted x86-64 jobs, or add
the harness to the CI workflow. No runtime or test-code fixes were needed.

## Accounting verification record — 2026-09-30

Local macOS/Python 3.9.6 checks passed with normal and AddressSanitizer/UBSan
binaries. The updated suites contain 20 baseline checks and 32 chaos checks
(14 new accounting regressions). Both suites passed in both build modes.
`caffeinate -i` again prevented idle sleep during these local Make invocations.

| Default seed-42 experiment | Normal | ASan/UBSan |
| --- | --- | --- |
| Verdict | CHAOS_PASS | CHAOS_PASS |
| Submitted = completed + terminally_failed | 100 = 100 + 0 | 100 = 100 + 0 |
| Eligible IDs at drain start, all completed | 51 | 43 |
| Confirmed interrupted attempts / replacements | 5 / 5 | 5 / 5 |
| Total attempts / retries | 105 / 5 | 105 / 5 |
| Cleanup | All 568 children reaped; groups retired | All 482 children reaped; groups retired |

The full runs are retained in `build/chaos/chaos-eai_7cah/` (69.996 seconds)
and `build/chaos/chaos-l4uwm3qi/` (79.936 seconds). These durations are
observations, not performance guarantees. Every default job retained exactly
`slept_ms=3000`. The live zero-retry regression separately verified the valid
mixed outcome `6 = 5 + 1`, with an attributed WORKER_LOST terminal failure.

An independent artifact pass checked the new reports against original
submissions, full statuses, drain cohorts, actual lost attempts, retry/attempt
sums, fault cutoffs, child exits, and reaping in both full runs. The final
artifact assertions also passed for 16 regression runs with verified accounting,
including runs that correctly failed the separate coverage requirement.
Focused accounting/evidence checks, Python syntax, changed Markdown links/fences,
and `git diff --check` passed. The changes affect the Python harness and docs;
the C runtime and Linux CI configuration are unchanged. This record claims local
execution only.

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
