# Worker failure recovery

Six fault-tolerance acceptance checks exercise recovery and retry limits with
real busy workers. SIGKILL exercises connection-loss recovery; SIGSTOP exercises
heartbeat expiry while the worker process remains paused and keeps its socket
open until the coordinator closes it.
Two scenarios resume that expired worker while its retry is running or
after its retry has completed, checking that the current job remains unchanged.
Two more interrupt every allowed attempt, checking that the job becomes FAILED
and stays out of the queue even when another worker is available. The coordinator,
workers, and submission CLI are the actual executables.

## Assignment lease

A lease is revocable permission to run a particular attempt of a job. In this
implementation, the coordinator owns that permission and ties it to the worker's
registered connection and heartbeat liveness. There is no separate lease message,
per-job lease timer, or expiry timestamp sent to the worker.

| Part of the lease | Current behavior |
| --- | --- |
| Grant | The coordinator changes QUEUED to ASSIGNED, records the worker ID, increments the attempt, and reserves the worker before sending JOB_ASSIGN. |
| Identity | Every assignment and job report carries `(job_id, worker_id, attempt)`. Worker identity is also checked against the sending connection. |
| Liveness | Registration initializes the last-heartbeat time. Each accepted HEARTBEAT refreshes it using the coordinator's monotonic clock. Assignment, PING, and job reports do not refresh it. |
| Default timing | Workers send a heartbeat every 2000 ms; the coordinator expires them after 6000 ms without an accepted heartbeat. Both values are configurable independently. |
| Expiry | A live worker expires when `now_ms - last_heartbeat_ms >= heartbeat_timeout_ms`. The event loop checks expiry before reading that connection, so late bytes cannot renew an already-expired registration. |
| Early revocation | Disconnects and connection/protocol failures also revoke ownership, including failure while sending an assignment. |
| End of attempt | An accepted completion stores DONE and the result. Task failure or worker loss requeues the job if retries remain; otherwise it stores FAILED. |

The configuration flags are `--heartbeat-interval-ms` on the worker and
`--heartbeat-timeout-ms` on the coordinator. They are not negotiated. Allow
headroom between the interval and timeout for delivery and scheduling delays.
Expiry is observed when the event loop runs, not at a guaranteed real-time instant.
An assignment inherits the worker's current heartbeat freshness; it does not
start a new six-second job timer.

Revocation removes the old connection from scheduling and report handling. A
retry returns to the FIFO tail with the same job ID and no current owner. Its
next assignment uses a higher attempt number. `max_retries=N` allows at most
`N+1` assignments, including the original; the default zero allows one. An
assignment can consume this allowance even if its worker never receives it or
reports STARTED. No eligible worker means the queued retry waits.

Within one coordinator lifetime, each job has at most one authoritative active
assignment. The scheduler validates the connection's worker ID, the current job
owner and attempt, and the permitted state transition before accepting a report.
DONE and FAILED are terminal. A successful job has one accepted completion; an
old report cannot replace that result or claim ownership of a newer attempt.
Job/worker IDs and attempt counters now survive coordinator restart through
the [WAL](persistence.md); identity continuity applies to the same retained history.

A rejected scheduler report leaves its state unchanged. At the TCP handler,
an invalid report also closes the offending connection; if that connection owns
active work, separate worker-loss cleanup applies to that work. This distinction
matters for invalid reports on a still-current connection. An expired worker's
old connection has already been removed and cannot report into a newer attempt.

A timeout means the coordinator stopped hearing from a worker. It does not prove
the process stopped computing. A paused or partitioned worker may continue or
resume its old computation after another worker starts the retry. The worker
cancels its task when it detects a lost connection, but the coordinator cannot
remotely force that detection or undo effects already performed.

Heartbeats also do not prove task progress. A hung task can remain RUNNING while
the worker's networking thread continues heartbeating. A separate execution
deadline, task-progress watchdog, and enforced expiry at external resources are
not implemented. The current lease protects coordinator ownership and accepted
results; it does not prevent overlapping physical executions or external effects.

## At-least-once execution and its limits

Faultline uses bounded at-least-once retry semantics: work whose successful
completion is unconfirmed may be executed again, from the beginning. For example:

1. Worker A runs job J on attempt 1 and finishes its computation.
2. Its connection fails before the coordinator accepts the completion report.
3. The coordinator still sees unfinished work, so it requeues J if a retry remains.
4. Worker B runs J on attempt 2 and reports a result that the coordinator accepts.

The task executed twice even though the coordinator stores only one accepted
result. A worker's `job_completed_sent` log establishes a successful local send,
not remote acceptance. The protocol has no completion acknowledgment to the worker.

Task implementations should be idempotent: repeating the same logical job should
have the same intended external effect as performing it once. Setting a value to
the same value can satisfy that property; incrementing a counter again does not.
The current built-in tasks return computed results without application-level
external writes. Future tasks with effects need their own idempotency/deduplication
mechanism at the destination. Attempt validation inside the coordinator cannot
undo or deduplicate those effects.

The execution guarantee is conditional:

- While the coordinator remains running, detected worker loss preserves the job
  as either a queued retry or a terminal FAILED record. It does not silently drop
  the job from its store. Completed/failed records remain within the 256-job limit.
- A retry can progress only when an eligible worker is available and the
  coordinator, task, and network can make progress before the allowance runs out.
- Finite retries can end in FAILED without any successful execution. A job may
  never begin computing if every assignment fails before execution or no worker
  becomes available. At-least-once describes the retry/duplicate-execution policy
  here; it is not an unconditional promise of delivery or eventual success.
- A submission ACK follows durable creation and in-memory publication. The
  [persistence contract](durability.md) defines retained-WAL restart recovery.
- A lost submission ACK leaves acceptance uncertain. Manually resubmitting can
  create a second job ID; there is no client submission deduplication key yet.

These are guarantees of a single coordinator. Exactly-once execution
and coordinator failover remain outside the implementation. Durable recovery
is covered separately by the [persistence tests](persistence.md#files-and-verification).

## Run the checks

```sh
make test-recovery
make SANITIZE=1 test-recovery
```

The target builds the executables and runs `tests/integration/test_recovery.py`.
It selects an available loopback port, starts its own coordinator and workers,
captures their logs, and cleans up the processes afterward. Each scenario uses
a fresh coordinator. Signals target only the test's own worker child processes.

All six checks are also included in `make test-integration`, `make test`, and
`make test-sanitize`. The existing `INTEGRATION_ARGS='--port 9000'` option works
when that port is available.

## Phase review

Reviewed on 2026-09-19 on macOS arm64, using runtime and tests at commit
`0ea8118`. This review changes documentation only. The fault-tolerance phase's
exit criterion is met: killing a worker during a job allows another connected
worker to complete that job when a retry remains. The broader recovery checks
also pass in normal and AddressSanitizer/UBSan builds:

| Full regression command | Review result |
| --- | --- |
| `make test` | 67 C test groups and 75 integration checks passed; 2 default-endpoint checks skipped. |
| `make test-sanitize` | The same 67 C groups and 75 integration checks passed with AddressSanitizer/UBSan; the same 2 checks skipped. No sanitizer diagnostics were reported. |

Each run discovered 77 integration scenarios, including all six recovery checks.
Available loopback ports were selected automatically, so the CLI and worker
default-port-9000 checks were skipped. Those endpoint defaults were not reverified
in this review. No runtime or test-code changes were needed.

| Acceptance check | Observed result in both builds |
| --- | --- |
| SIGKILL during attempt 1 | Connected worker completes the same job on attempt 2, then completes a follow-up job. |
| SIGSTOP with the connection still open | Six-second heartbeat expiry requeues the job; the connected survivor completes attempt 2 while the original worker remains paused. |
| Resume old worker during attempt 2 | Old process exits on its closed connection; the current running attempt and eventual result are preserved. |
| Resume old worker after attempt 2 completes | The accepted DONE result is preserved. |
| Repeated crashes with two retries | Three interrupted attempts end in FAILED; the spare completes unrelated work, with no fourth assignment. |
| Crash with zero retries | The first interrupted attempt ends in FAILED; the spare remains usable. |

The C scheduler tests independently reject 24 old STARTED/COMPLETED/FAILED
reports across queued, assigned, running, and completed states, on the same or a
different worker ID. Every rejection preserves the scheduler snapshot. Job-model
tests cover all state transitions, terminal states, attempt validation, and retry
boundaries. Registry and heartbeat tests cover renewal, the exact expiry boundary,
and late buffered heartbeats. Execution and scheduling tests cover task errors,
graceful disconnect, retry FIFO ordering, and continued worker reuse.

The six recovery scenarios are deterministic local failure experiments. Resuming
an old process does not guarantee that an outdated completion reaches the report
handler; the direct scheduler matrix covers validation separately. These runs
do not exercise real multi-host partitions, repeated heartbeat-expiry exhaustion,
external side-effect deduplication, or coordinator restart. Test timing bounds
allow local scheduling slack and are not production latency guarantees.

This phase review established worker-failure behavior before persistence. The
subsequent [durability contract](durability.md) and [coordinator implementation](persistence.md)
now add WAL-backed identities, sync-before-ACK ordering, replay, and interrupted
attempt accounting at startup. Separate persistence tests provide that evidence;
the six worker-recovery scenarios above still have their original scope.

## Hard-crash scenario

1. Start two workers with 100 ms heartbeat intervals. Wait for both registrations
   and a heartbeat from each before submitting any work.
2. Submit `sleep --args 3000 --max-retries 1` through the CLI.
3. Wait until the coordinator records RUNNING for attempt 1. Read its worker ID
   to identify the owner; the test does not assume launch order determines ownership.
4. Verify the other worker is still idle, then observe another heartbeat from
   the owner after STARTED. Check that the job has not completed.
5. Send SIGKILL to the owner and verify its process exits because of that signal.
6. Verify the coordinator releases and requeues the unfinished job exactly once.
7. Wait for the already-connected worker to start the same job under attempt 2.
8. Verify DONE with `result="slept_ms=3000"`, the survivor's worker ID, and one retry.
9. Submit `fibonacci --args 10`. Verify that the survivor also completes this new
   job with result `55`, continues heartbeating, and the CLI still receives PONG.

The test waits for specific observed transitions with bounded deadlines, rather
than using a fixed pause to guess when it is safe to kill the worker. Only complete
log lines are parsed. Attempt-aware waits ensure a previous STARTED event cannot
be mistaken for the start of the retry.

## Why SIGKILL matters

SIGTERM and Ctrl+C allow the worker's signal handler to request shutdown. Its
ordinary cleanup then cancels and joins the task thread and closes the socket.
The execution suite already covers that cooperative path.

SIGKILL terminates the process without running that cleanup. The operating system
closes its sockets, and the coordinator observes EOF or a transport error. The
failed worker cannot send a task-failure report or put its own job back in the
queue. Recovery must come from the coordinator's existing record of ownership.

The coordinator retains its default six-second heartbeat timeout in this check.
The test requires the survivor's attempt-2 STARTED report within 2.5 seconds of
signalling the crash, verifies a transport-related death reason, and rejects a
heartbeat-timeout event for that worker. This establishes prompt disconnect-based
recovery. The bound allows local scheduling slack; it is not a network latency SLA.

## Heartbeat-expiry scenario

The second test registers two workers with 100 ms heartbeat intervals and uses
the default 6000 ms coordinator timeout. It submits another three-second sleep
with one retry, identifies its owner from the first RUNNING report, and waits
for another heartbeat from that owner before sending SIGSTOP.

SIGSTOP suspends the entire process, including its networking and task threads.
The process has not exited, and its socket is not closed by the pause. No
application signal handler needs to run. The coordinator receives no special
message saying that the worker was paused; it must detect heartbeat silence.

The test verifies these stages:

1. **The worker is actually stopped.** `os.waitpid()` with `WUNTRACED` observes
   the operating system's stopped status and confirms the stop signal was SIGSTOP.
   `WNOHANG` and a deadline keep the status check from blocking indefinitely.
2. **No immediate reassignment.** For an observation window before expiry, the
   original job remains RUNNING on attempt 1, with no worker-death, timeout,
   requeue, or second assignment event. The paused process is still present.
3. **The rest of the system remains responsive.** The idle worker keeps sending
   heartbeats, and the CLI can receive PONG while the owner is suspended.
4. **Heartbeat expiry causes worker loss.** Exactly one timeout and one death
   are recorded with reason `heartbeat_timeout`. The observed silence must be
   at least 6000 ms and below 7500 ms, allowing local scheduling slack. The test
   checks `silence_ms = detected_at_ms - last_heartbeat_ms`, the matching socket
   descriptor, and the timestamp of the last accepted heartbeat.
5. **The survivor completes the retry.** The same job ID is requeued once and
   assigned to the connected idle worker with attempt 2 and retry count 1. Its
   result is `slept_ms=3000`. It then completes a new Fibonacci job with result 55.
6. **The original worker stays paused.** It sends no terminal report and remains
   present through the recovered completion. The healthy worker retains its
   original registration and continues heartbeating. The complete expected job
   transition sequence is checked, just as in the hard-crash scenario.

The timeout is measured from the last heartbeat the coordinator accepted,
not from the harness's call to SIGSTOP. Bytes already sent before suspension may
still be delivered. The coordinator's own timestamps establish the silence
interval without comparing clocks from different processes.

At expiry the coordinator marks the registration DEAD, handles the job as
WORKER_LOST, and closes that connection. DEAD describes its scheduling/liveness
decision; the operating-system process can still be alive and stopped. The
connection stays open before expiry, and the coordinator closes it as part of
expiry cleanup.

The paused worker is deliberately not resumed in this scenario. A `finally`
block kills and reaps that test-owned process after verification, or after a
test failure, because a stopped process cannot handle SIGTERM. On a passing run,
this cleanup happens after recovery and completion; it cannot be the event that
caused reassignment. The separate scenarios below exercise resuming the old attempt.

## Resuming an expired attempt

`OldAttemptRecoveryTests` adds two scenarios using the same real coordinator,
workers, CLI, three-second sleep, and default six-second timeout. Both register
two workers, wait for attempt 1 to start, pause its owner with SIGSTOP, and wait
for heartbeat expiry and reassignment to the connected survivor.

The old process is then resumed with SIGCONT at one of two points:

| Resume point | What must be protected |
| --- | --- |
| Attempt 2 is RUNNING | Current ownership, attempt, retry count, and running state |
| Attempt 2 is DONE | The accepted result and terminal state, as well as identity |

The harness snapshots the original job's complete event history immediately
before SIGCONT and checks that it is unchanged when the old worker exits. The
RUNNING case explicitly requires the retry to still be RUNNING at this point;
the DONE case includes the stored `slept_ms=3000` result. After attempt 2 completes,
a follow-up Fibonacci job must return `55` from the survivor, with no further
events for the original job.

The resumed process keeps its old socket, worker ID, and attempt-1 assignment.
Expiry has already removed that connection from the coordinator's event loop.
On resuming, the worker encounters the closed connection and exits with status 1.
The test requires a connection-related error, not termination by a cleanup signal.
Its previously accepted heartbeat history must remain unchanged, with exactly
one old-worker registration, one timeout, and one death. The survivor retains its
own registration, continues heartbeating, and the CLI still receives PONG.

Thread scheduling determines whether the old task finishes before the networking
thread detects the closed connection. A local socket send can also succeed without
the coordinator accepting the report. The test prints `old_local_completion_send`
as diagnostic information, but neither requires nor forbids a local completion
log. Its assertions use coordinator events. It does not claim that every run
delivers an outdated completion into the coordinator's report handler.

To exercise that handler's underlying validation independently of this race,
`tests/test_scheduler.c` also adds a direct C test group. It passes old STARTED,
COMPLETED, and FAILED reports to the scheduler in each of these states:

- QUEUED after the first attempt failed or its worker was lost.
- ASSIGNED to attempt 2.
- RUNNING on attempt 2.
- DONE with attempt 2's accepted result.

It runs this matrix for a task-error retry on the same worker ID and for a
worker-loss retry on a different worker ID: 24 rejected reports in total. The
same-worker case checks why the attempt number is necessary even when worker
identity matches. The old completion carries `OLD`, while the accepted completion
carries `NEW`, so an overwrite would be visible. Every rejected call must leave
the entire scheduler snapshot unchanged, including records, timestamps, retry
counters, result bytes, ID allocation state, and the FIFO.

Together these checks cover the closed-connection boundary and state/identity
validation. The current wire format already carries job ID, worker ID, and attempt;
no new message, field, or production C change was required.

Each real-worker scenario has a finally block that kills and reaps the old child
if it is still present after a failure, including if it is still stopped. On a
passing run the resumed worker has already exited by itself. SIGCONT resumes the
old process; it does not restore its expired registration or ownership.

## Expected state changes for a successful retry

Let J be the submitted job, A its original worker, and B the connected survivor:

| Event | Job state | Owner | Attempt | Retry count |
| --- | --- | --- | --- | --- |
| Submission | QUEUED | None | 0 | 0 |
| First assignment | ASSIGNED | A | 1 | 0 |
| First start | RUNNING | A | 1 | 0 |
| A is lost | QUEUED | None | 1 | 1 |
| Reassignment | ASSIGNED | B | 2 | 1 |
| Retry starts | RUNNING | B | 2 | 1 |
| Result accepted | DONE | B | 2 | 1 |

The job ID stays J throughout. The retry counter increases when the job is
requeued, and the attempt number increases when it is assigned again. A retry
runs the task from the beginning: B performs the full three-second sleep.

The test compares this complete transition sequence after the follow-up job
finishes. It also checks one recorded death for A, no death for B before cleanup,
and one registration for B. B keeps its original process and identity; no
replacement worker is launched after the crash. Missing or duplicate transitions
for J fail the check.

## Retry-exhaustion scenarios

`RetryExhaustionTests` checks two retry allowances: two retries and zero retries.
The allowance counts **additional attempts after the original attempt**. Thus
`--max-retries 2` permits three total attempts; `--max-retries 0` permits just one.
The retry counter increases when the coordinator requeues a job. It does not
increase on the final failure because no further retry is granted.

For the two-retry case, the harness starts four workers with 100 ms heartbeat
intervals and waits for every registration and heartbeat before submitting
`sleep --args 60000 --max-retries 2`. Three workers will be interrupted, leaving
one healthy spare. The long sleep keeps attempts busy during the checks; a passing
test kills each attempt promptly and does not wait a minute for it to complete.

For each attempt, the test waits for the coordinator's RUNNING event and another
heartbeat from its owner. It discovers that owner from the event rather than
assuming worker launch order. It then sends SIGKILL and requires the child to exit
because of that signal, with no completion or task-failure report. The coordinator
must record one transport-related death for that worker and no heartbeat timeout.
The coordinator retains its default six-second heartbeat timeout, while each
expected job-loss transition has a two-second observation deadline.

The same job ID passes through these outcomes:

| Interrupted attempt | Retry count before loss | State after loss | Retry count after loss | Pending jobs |
| --- | --- | --- | --- | --- |
| 1: original attempt | 0 | QUEUED | 1 | 1 |
| 2: first retry | 1 | QUEUED | 2 | 1 |
| 3: second retry | 2 | FAILED | 2 | 0 |

Every retry is assigned to a different already-connected worker, moves through
ASSIGNED and RUNNING, and executes the same job from the beginning. The harness
checks the complete sequence, including submission, all three assignments/starts,
and all three loss events. Missing, duplicate, or extra transitions fail the test.
The final event is `job_worker_lost` with `state=FAILED`: this is a coordinator
decision after connection loss, not a worker-sent `JOB_FAILED` message.

The terminal record retains the last worker ID and attempt as history. Queued
records clear the worker ID to zero. Keeping the last owner on a FAILED record
does not reserve a worker or make the job eligible for scheduling; active ownership
only applies to ASSIGNED/RUNNING jobs. There is no result for this failed job.

The healthy spare makes the stop condition observable: a lack of workers cannot
explain why the job stopped retrying. After the final failure, the harness submits
`fibonacci --args 10` and requires that spare to complete it with result `55`.
It then observes another heartbeat and a successful CLI PING/PONG. The failed
job's event history must remain unchanged, and the spare must never receive it.
An erroneous fourth sleep attempt would occupy the only live worker and prevent
the follow-up job from completing within its deadline.

The zero-retry case repeats the same checks with two workers. Losing the first
attempt immediately produces FAILED with `attempt=1`, `retry_count=0`, and an
empty queue. The remaining worker completes the follow-up job. This boundary
case catches accidentally granting an extra retry when the allowance is zero.

`contextlib.ExitStack` manages the variable number of worker fixtures and closes
all of them if an assertion fails. Every deliberately killed child is waited on;
the spare shuts down normally after the checks. Each scenario has a fresh
coordinator and uses bounded waits for observed events.

## Existing implementation exercised

These acceptance checks required no changes to the C recovery implementation:

- `src/coordinator/main.c`: connection cleanup finds the worker's active job,
  invokes worker-loss handling, marks the registration dead, and closes the socket.
  Its poll loop checks heartbeat expiry before reading socket events and calls
  that same cleanup path with reason `heartbeat_timeout`.
- `src/coordinator/scheduler.c`: `faultline_scheduler_worker_lost()` applies
  WORKER_LOST to the active job and publishes the resulting queue/state update.
- `src/coordinator/job.c`: the model requeues only while `retry_count < max_retries`,
  clearing queued ownership and incrementing the retry counter. Otherwise it
  records FAILED and the finish time. The next assignment increments the attempt.
- The scheduler selects the oldest queued job for an eligible idle worker.
- Only QUEUED outcomes go back into the FIFO; exhausted FAILED jobs stay in the
  job store as terminal records.
- Healthy workers execute recovered or follow-up tasks and send normal
  STARTED/COMPLETED reports.

The existing [scheduling guide](scheduling.md) explains validation and queue
ownership; [task execution](tasks.md) explains the worker's computation and reporting.

## Scope of this step

These checks verify crash recovery, heartbeat-based recovery, old-attempt
protection, and repeated worker-loss exhaustion while the coordinator remains
alive. Existing model/scheduler tests also cover retry limits, and the execution
suite checks exhaustion after task errors. The new exhaustion scenarios use
SIGKILL; repeated heartbeat-expiry exhaustion is not a separate scenario here.
Both detection paths use the same worker-loss operation.

Jobs and accepted results now survive coordinator restart through the
[WAL](persistence.md); these worker-recovery checks still focus on a running coordinator. The CLI reports submission acceptance, not the eventual result or
terminal failure; these checks observe coordinator logs. At-least-once retry
semantics permit repeated task execution, and a finite retry allowance can end
in FAILED without a successful result. None of these checks guarantees that a
task executes only once.
