# Chaos-test contract

Status: **baseline and seeded worker-crash harnesses implemented.**
The [per-job accounting audit](chaos-testing.md#drain-and-per-job-proof) saves
the post-fault drain cohort and verifies the terminal ID partition, results,
and attributed retry histories before a run can pass.
This defines the first worker-crash experiment for the Testing & chaos phase.
The [baseline harness](batch-testing.md) runs with `make test-batch`, using the
profile below with fault duration **zero**.
The [crash harness](chaos-testing.md) runs with `make test-chaos`, using the
30-second window, saved candidate plan, replacements, and recovery checks.

The experiment must show that deliberately killing workers does not silently
lose acknowledged jobs, that eligible work finishes after faults stop, and that
retry limits remain enforced. The coordinator stays alive throughout this run.
Coordinator crashes, paused workers, storage failures, and stale reports remain
covered by their existing focused suites.

## Default profile

Contract/profile version: `faultline-chaos-v1`.

| Setting | Default | Reason |
| --- | --- | --- |
| Coordinator count | 1 | Exercise worker recovery under one durable job history. |
| Worker pool | 5 | Matches the MVP example and leaves other workers available during a crash. |
| Submitted jobs | 100 | Enough backlog for faults; below the 256 retained-job limit. |
| Workload | Every job is `sleep --args 3000` | Predictable duration and independently checkable output. |
| Retry allowance | 3 per job | At most four assignments: the original plus three retries. |
| Fault | SIGKILL one registered busy worker, then start its replacement | Exercises abrupt connection-loss recovery with real processes. |
| Fault window | 30,000 ms | A bounded period after admission; no new kills after it expires. |
| Fault spacing | Seeded integer delay in 2,000–10,000 ms | Matches the MVP's two-to-ten-second spacing. |
| Random seed | 42 | Explicit and recorded; never silently chosen from the clock. |
| Worker heartbeat interval | 2,000 ms | Preserve the current production default. |
| Coordinator heartbeat timeout | 6,000 ms | Preserve the current production default. |
| Inspection polling interval | 200 ms | Observe progress without a tight busy loop. |
| Overall deadline | 180,000 ms | Covers startup, admission, faults, drain, verification, and cleanup. |
| Reserved cleanup budget | 10,000 ms | Stop and reap children before declaring the run successful. |
| Endpoint | A fresh IPv4 loopback port | Isolate this experiment from an existing coordinator. |
| Storage | Fresh WAL in a unique run directory | Never open or overwrite the user's existing WAL. |

Successful output for every default job is exactly the 13 bytes
`slept_ms=3000`. The rough no-fault execution time is `100 × 3 / 5 = 60` seconds,
before submission, persistence, scheduling, and inspection overhead. This is
sizing guidance, not a timing assertion or benchmark.

The first workload intentionally keeps computation and expected results simple.
The existing [task tests](tasks.md) cover the other built-ins; mixed workloads
can be added as separate named profiles after this harness works.

## Configuration and preflight

The harness must expose/record worker count, job count, retry allowance,
fault duration, seed, overall deadline, and build/binary selection. Baseline mode
uses the same workload with a zero fault duration.

Validate configuration before starting a child:

- Worker count is an integer from 1 through 16. This harness limit leaves room
  for inspection/submission clients under the coordinator's 64-client limit.
- Job count is an integer from 1 through 256. Retained terminal jobs consume
  capacity too; the harness must not evade the limit by rotating WALs.
- Retry allowance fits the protocol's unsigned 32-bit field, including zero.
- Seed is an explicit integer from 0 through `2^32 - 1`.
- Durations are finite integer milliseconds; fault duration is nonnegative,
  and the overall deadline exceeds fault duration plus the cleanup reserve.
- Executables exist, the output directory is new, and all commands target the
  harness-owned coordinator endpoint. Do not attach to an existing service.

Small batches/short fault windows might not provide sufficient crash coverage.
Such a run must not receive a chaos PASS merely because its jobs finished.

The default run directory will be below `build/chaos/`, using a unique run ID.
`build/` is already ignored by Git. Keep the directory on success and failure,
so results can be inspected; artifact deletion is separate from process cleanup.
An output-directory override must also create a fresh directory.

## Lifecycle and deadlines

Capture a monotonic start time immediately before the first child is spawned.
Define:

```text
run_deadline  = start + 180 seconds
work_deadline = run_deadline - 10 seconds
```

Use the configured deadline in place of 180 for overrides. Every child wait,
socket/CLI operation, readiness check, poll sleep, and fault delay uses the
remaining shared budget. A local timeout must never extend `work_deadline`.
Cleanup has its own bounded reserve; there are no unbounded `wait()` calls.

The run proceeds in this order:

1. **Start the coordinator.** Create a new WAL with `--init-wal`, confirm that
   the owned process is serving, and record its PID and endpoint.
2. **Start the pool.** Wait for all five worker registrations and record each
   process-to-worker-ID mapping. Do not assume registration order matches spawn
   order or treat a socket descriptor as worker identity.
3. **Admit the batch.** Submit jobs through the real CLI, one at a time. Record
   input index, task, arguments, retry allowance, and acknowledged job ID.
   Workers may already execute while the rest of the batch is admitted.
4. **Inject faults.** Start the 30-second fault clock after every submission is
   acknowledged and the pool is ready. Keep the coordinator running.
5. **Drain.** Stop issuing faults, complete any replacement already in progress,
   and let the pool finish all remaining eligible jobs.
6. **Verify.** Query every acknowledged ID and compare listings/statistics at
   terminal quiescence, before shutdown changes worker gauges.
7. **Clean up.** Stop and reap every child, close owned resources, and only then
   publish the final verdict.

Startup sublimits are 5 seconds for coordinator readiness, 10 seconds shared by
the initial worker pool, and 5 seconds for each replacement registration.
Admission has a 30-second batch budget. Each CLI subprocess has at most 8 seconds.
All are capped by the remaining work budget; they do not add time to the run.

If the full fault window cannot fit before `work_deadline` once admission ends,
fail the run instead of silently shortening the requested experiment. In chaos
mode, keep the planned fault window even if jobs finish early; remaining events
can be recorded as skipped for lack of busy work. Baseline mode proceeds
directly to drain.

If work or verification has not finished by `work_deadline`, the run fails with
a deadline reason and immediately enters cleanup. The nominal 180-second bound
assumes a responsive OS: an uninterruptible process or blocked filesystem can
defeat a userspace timing guarantee. Never report successful cleanup for an
unreaped child.

## Seeded choices and crash evidence

Use a private `random.Random(seed)` instance. Before launching processes,
prepare up to `floor(fault_duration_ms / 2000)` candidate events. For each event,
draw its gap with `randint(2000, 10000)`, then shuffle the stable pool-slot list
`0 .. worker_count-1`. Save the complete candidate plan and Python version.
Do not draw extra random numbers during polling, timeouts, or error handling.

The first gap starts at the fault-window start. Later gaps start after the
previous event's replacement cycle completes (or after a skipped selection).
Only one crash/replacement cycle may be in progress. If a gap would end at or
after the fault-window deadline, record the unused plan suffix and stop issuing
faults. Never extend the window to fit more kills.

At each due event:

1. Refresh the worker snapshot and inspect live, registered children belonging
   to the harness. Walk the saved slot permutation and choose its first busy
   worker (ASSIGNED or RUNNING ownership). If none qualifies, record a skipped
   event; do not kill an unrelated or idle process merely to raise the count.
2. Record the slot, generation, PID, worker ID, observed job ID/attempt/state,
   and elapsed time before signaling the owned process with SIGKILL.
3. Wait for that child and confirm the signal exit. Replace it in the same
   logical slot, increment its generation, and require a fresh worker ID.
   Retire the old handle; do not later signal a saved PID that may be reused.
4. Correlate the coordinator's worker-loss/retry events with the victim's actual
   lease. Record both the pre-signal observation and any different lease lost
   in the race. A job can complete between inspection and SIGKILL, so a busy
   snapshot alone does not prove its attempt was interrupted.

A cycle completes only after the old child is reaped, the coordinator's death
event and any resulting job-loss transition are accounted for, and its replacement
has registered. Give the coordinator at most five seconds to acknowledge the
observed loss, capped by the work deadline. Finish this accounting for the last
injected crash before determining which jobs are eligible to finish in drain;
its delayed loss transition is still part of fault injection.

A seed reproduces the planned random choices under the recorded generator
version. It cannot reproduce OS scheduling, exact timings, worker IDs, target
eligibility, or accepted-result races. Preserve both the plan and actual events.
Reproducing a failure requires its configuration, code/binary versions, and
event trace as well as the seed; this is not deterministic execution replay.

The phase-review seed set is `42, 7, 2026`. Each is a separate run with a fresh
WAL and the same default profile. The [review record](chaos-review.md) documents
the completed normal/sanitizer matrix. Do not change seeds or automatically
rerun a failing seed until it happens to pass.

## Job accounting and pass criteria

The submission ledger is authoritative for what the harness was told was
accepted. Each successful CLI submission must return one distinct nonzero job
ID. A failed or missing ACK makes acceptance uncertain: abort admission, retain
the attempted input and diagnostics, and fail the run. Never blindly resubmit;
[request deduplication](request-deduplication.md) is still deferred.

For every admitted job, retain observations without assuming numeric IDs encode
submission order. A known FAILED job has a successful `status` lookup (exit 0);
an unknown acknowledged ID or a failed query cannot be counted as a terminal job.

Before cleanup, a successful run must satisfy all of these:

- Exactly the acknowledged ID set appears in `jobs`, with no missing, extra,
  or duplicate identities. All planned submissions were acknowledged.
- Every ID is DONE or FAILED; none remains QUEUED, ASSIGNED, or RUNNING.
- Every DONE result exactly matches the independently known workload result,
  and has `failure=NONE`.
- Every FAILED job has `failure=WORKER_LOST`, no result, and exhausted its
  configured retry allowance. TASK failure is unexpected for these valid inputs.
- Once a job is observed terminal, later observations must preserve its state,
  owner, attempt, retries, failure, and result.
- Each terminal job has `1 <= attempt <= max_retries + 1`,
  `0 <= retry_count <= max_retries`, and `attempt = retry_count + 1`.
  FAILED additionally requires `retry_count = max_retries`.
- The terminal ID sets are disjoint and their union equals the submission
  ledger. Consequently `submitted = completed + terminally_failed`, but matching
  totals alone is insufficient.
- `stats` agrees with the per-job state counts and sums of attempts/retries.
  Queued/assigned/running counts are zero; all configured worker slots are
  registered and ALIVE, idle, and own no active lease. Historical terminal
  worker IDs need not remain in the current worker registry.
- The coordinator never exited unexpectedly. There were no unplanned worker
  exits/expiry, replacement failures, malformed command replies, unexpected
  protocol rejections, or sanitizer diagnostics.
- Process cleanup succeeds under the rules below.

Ordinary bounded retry exhaustion is an allowed terminal outcome. Do not demand
100 successes after arbitrary repeated interruption. Once faults stop, the
remaining eligible queued/active jobs must reach DONE under the healthy pool
before the deadline; unexpected drain-time failure fails this profile.

**Chaos coverage is also required:** at least one registered worker must have
been deliberately killed, and coordinator evidence must confirm worker loss
interrupted an owned job. With a positive retry allowance, at least one such
interrupted job must reach DONE on a higher attempt. With zero retries, require
an attributable terminal WORKER_LOST failure instead. If that evidence is absent,
report `INSUFFICIENT_COVERAGE` and exit unsuccessfully, even if accounting is sound.
This distinguishes a workload that finished from a demonstrated recovery.

**Baseline mode** injects no faults and requires every job to be DONE with
attempt 1, retry count 0, and the expected result. It reports BASELINE_PASS,
not CHAOS_PASS.

## Process ownership and cleanup

Track the coordinator, every worker generation, every submission/query CLI,
and any helper immediately after creation, before waiting for readiness.
Keep process handles, role, PID, start/exit times, return code, and whether a
signal was intentional. Reap short-lived CLIs promptly; completed/crashed
children remain in the ledger but need no further signals.

Launch children directly with argument lists, without background shell commands.
Use owned sessions/process groups so cleanup can also address descendants of
a child. Signal only tracked, still-owned processes/groups. Never use `pkill`,
`killall`, process-name matching, or a broad scan of the user's processes.

One outer cleanup path must cover success, assertions, exceptions, partial
startup, admission/registration failures, deadlines, SIGINT, and SIGTERM.
Signal handlers request shutdown; cleanup must still execute. Stop fault
injection and prohibit further spawns before beginning teardown.
Cleanup is idempotent: repeated shutdown requests do not reset its deadline,
re-signal retired handles, or abandon the remaining children. Record an individual
cleanup error and continue trying to stop/reap the rest of the owned processes.

Within the ten-second reserve, use shared deadlines rather than a fresh timeout
for each child:

1. At cleanup start, request SIGTERM for live workers and CLI/helpers. Resume
   any owned stopped process before graceful termination. Wait/reap these
   children for at most four seconds in total.
2. Send SIGKILL to any of those still alive, request SIGTERM for the coordinator,
   and wait/reap under a shared deadline six seconds from cleanup start.
3. Send SIGKILL to any remaining owned process/group, including the coordinator.
   Wait/reap all remaining children by ten seconds from cleanup start.
4. Verify every direct child has a recorded terminal status and was reaped,
   and no descendants remain in still-owned process groups. Retire a group once
   it is confirmed empty and never signal it again. Close owned sockets, pipe
   ends, and log handles after draining/capturing diagnostics.

The coordinator normally stops after workers. Keep stdout/stderr in owned log
files or continuously drain pipes; a full unread pipe must not deadlock shutdown.
Cleanup failure is secondary evidence alongside the original failure, never a
replacement that hides it. A run that needed an unplanned forced kill during
cleanup cannot claim a clean PASS, even if the child was eventually reaped.

A final PASS is withheld until all cleanup checks succeed. On timeout or a
cleanup problem, print the remaining owned PIDs/roles, preserve available
artifacts, and return failure. SIGKILL of the harness itself, a host crash, or
loss of the runner cannot execute its cleanup code; an outer supervisor/CI
runner must dispose of the process environment in those cases. This contract
does not claim that a Python `finally` block survives SIGKILL.

## Evidence and verdict

Retain these planned artifacts in the unique run directory:

- `manifest.json`: contract version, full configuration, seed and candidate
  plan, endpoint, environment/Python/compiler/build metadata, binary identities,
  and source revision/dirty state when available.
- `events.jsonl`: timestamped spawn, registration, ACK, target selection, signal,
  exit/reap, replacement, observed transition, skipped event, and cleanup records.
  Use monotonic elapsed times for durations.
- Coordinator and per-generation worker logs, plus CLI arguments, stdout/stderr,
  and exit status; the submission ledger and final command snapshots.
- The run's WAL and `summary.json` containing verdict, first failure, coverage,
  job/attempt/retry totals, elapsed times, cleanup outcome, and unreaped children.

Exit 0 means a fully verified CHAOS_PASS or explicitly labeled BASELINE_PASS.
Exit 1 means a failed invariant, insufficient coverage, deadline, process/storage/
protocol error, or failed cleanup. Invalid configuration exits 2 before spawning.
Handled SIGINT/SIGTERM perform cleanup and exit 130/143 respectively. Never
silently skip unavailable binaries/platform facilities or convert a partial
experiment into success.

Both modes now create these artifacts. Chaos mode also saves the candidate plan,
actual crash/replacement events, and coverage behind CHAOS_PASS.
Logging is diagnostic evidence; durable
state and execution limits remain defined by the
[recovery](recovery.md) and [persistence](persistence.md) contracts.

## Implementation sequence

1. **Implemented:** the [batch harness and baseline mode](batch-testing.md),
   including ownership, deadlines, artifacts, exact results, and cleanup checks.
2. **Implemented:** the [saved fault plan and bounded SIGKILL/replacement cycle](chaos-testing.md),
   including per-ID accounting and coverage checks.
3. **Reviewed:** coverage and per-ID invariants across the planned seed set;
   see [evidence and limits](chaos-review.md).
4. **Validated:** bounded seed-42 and harness regression steps in [Linux CI](ci.md),
   including [GitHub-hosted execution](ci.md#github-hosted-linux-validation) of
   the complete workflow in both builds and independent audits of all four
   downloaded artifacts.
5. Proceed to the separate scaling and controlled failure-recovery benchmarks,
   with measurements defined independently from these correctness experiments.

Baseline/chaos runs and their regression suites have explicit Make targets.
They are not part of `make test`. Linux CI explicitly runs `test-batch-harness`,
`test-chaos-harness`, and the full seed-42 `test-chaos` experiment in both builds.
The full no-fault `test-batch` experiment remains a manual check.
