# Coordinator crash verification

The coordinator is killed with SIGKILL at selected durability boundaries, then
the ordinary executable restarts on the same TCP endpoint and the same WAL.
These checks verify the [durability contract](durability.md), including jobs
whose submission ACK was received, uncertain submissions, retained terminal
outcomes, and unfinished attempts eligible for retry.

```sh
make test-coordinator-crashes
make SANITIZE=1 test-coordinator-crashes
```

`make test-persistence` includes this suite, the earlier persistence/startup
process suites, and the C store tests. `make test-integration` includes each
process suite once. The crash suite has ten test methods: seven live-transition
matrices with five cases each, one startup matrix with ten cases, and two real
CLI/worker scenarios (47 cases total).

## How the crash boundary is selected

`tests/crash-coordinator` is a separate test executable. It compiles the actual
coordinator `main.c` with only its store-open call renamed, then links the normal
networking, scheduler, store, and WAL objects. The replacement open function in
[`crash_coordinator_io.c`](../tests/crash_coordinator_io.c) uses the existing
internal I/O seam. It delegates real file writes and fsync calls to the system
implementation, while pausing a selected record at a selected boundary.

The harness emits a marker and stops before returning to its caller. Python
waits for that marker, checks that no success ACK/assignment or accepted-event
log escaped the interrupted operation, and sends SIGKILL. No graceful shutdown
or job-loss cleanup runs. Recovery always uses `faultline-coordinator`, not the
instrumented executable. Test controls are absent from the production binary;
`make all` does not build the harness.

All processes, loopback sockets, and WALs belong to the test. WALs live in private
temporary directories. Tests wait for explicit boundaries or durable event logs
rather than sleeping for a guessed interval before killing the coordinator.

## Live transition matrix

Every row below runs at all five boundaries:

1. Before the first record byte is written.
2. After seven bytes of its header (an incomplete header).
3. After the complete header and all but the final payload byte.
4. After the complete record is written, before fsync.
5. After successful fsync, before the storage call returns and publishes state.

| Interrupted record | If the record is absent/incomplete | If the complete valid record survives |
| --- | --- | --- |
| Worker ID allocation | The unissued ID may be allocated later. | The ID stays consumed despite the missing registration ACK. |
| Job creation | No job exists for this unacknowledged submission. | The same job survives as QUEUED despite the missing submission ACK. |
| Assignment | The acknowledged job remains queued, with no attempt consumed. | The assignment counts as an interrupted attempt even though no assignment bytes were sent; retry accounting applies. |
| Started | The prior ASSIGNED attempt is interrupted. | The RUNNING attempt is interrupted. Both use one retry if eligible. |
| Completion | The active attempt is interrupted and retried if eligible. | DONE and the exact binary result remain terminal; no retry. |
| Requeue after task failure | Startup records WORKER_LOST and one retry. | Preserve the recorded TASK failure and already-charged retry. |
| Final task failure | Startup records terminal WORKER_LOST when no retry remains. | Preserve terminal TASK failure and its exact metadata. |

Each case starts with a previously completed job containing a 1024-byte binary
result and a permanently failed job. Both must survive byte-for-byte at the
decoded-record level, including timestamps and record sequence. Assignment and
later cases also start with an acknowledged queued job.

An independent Python decoder checks record lengths, CRCs, sequences, arguments,
results, and counters. Recovery must repair exactly the incomplete suffix,
preserve the prior prefix, restore identity allocation, and let a newly
registered worker finish eligible work. A subsequent new job verifies that
terminal jobs were not dispatched and the allocator continues correctly.
Another restart with all jobs terminal must leave the WAL unchanged.

The pre-fsync case deliberately distinguishes a process crash from power loss.
SIGKILL leaves the operating system running, so its complete written record is
available to replay and is adopted. This is not a guarantee that unflushed
bytes survive a machine or storage failure. A missing ACK remains uncertain;
this test does not add request deduplication.

## Crashing while recovering a crash

The startup matrix seeds two retryable active jobs (one RUNNING, one ASSIGNED),
an active job with no retries, and a queued job. It stops at each of the five
boundaries during either the first or second requeue: ten cases.

No listener may exist at the stopped boundary. After SIGKILL and normal restart,
each retryable job must have exactly one requeue record and one retry charge;
the exhausted job must have one terminal failure. Already committed recovery
outcomes are retained, and a partial trailing outcome is repaired and redone.
An extra restart before dispatch must add no records. A fresh worker then
receives jobs in order `[4, 1, 2]`: pre-existing queued work before retries.

## Real CLI and worker execution

Two additional scenarios use only the ordinary executables:

- Submit sleep, prime_count, fibonacci, and hash through the CLI and receive
  all four job IDs. SIGKILL the coordinator before workers connect. Restart,
  compare the restored job snapshots, and run a real worker. All four tasks
  must produce their expected results on attempt 1 with zero retries. Kill
  after completion and restart again; preserve all terminal records and let
  fresh work complete with the next job and worker IDs.
- Start a real 1500 ms sleep and wait for the coordinator's durable RUNNING
  event. SIGKILL the coordinator. The old worker detects the lost connection
  and exits; a fresh worker after restart completes the same job on attempt 2,
  with one retry consumed and result `slept_ms=1500`. A further restart retains
  that result unchanged.

These establish recovery of acknowledged work and actual execution after
restart. Automatic worker reconnection is not implemented; the tests start a
replacement worker. Execution remains at least once, subject to finite retries.

## Scope of the evidence

These are deterministic samples around complete-record durability and two
representative incomplete-record positions, not a kill at every instruction or
every possible byte offset. The existing writer/replay/store suites complement
them with short I/O, storage errors, arbitrary incomplete tails, invalid history,
corruption, readiness gating, and retry-boundary checks. They do not simulate
power loss, verify exactly-once external side effects, or measure flush latency.
The final persistence phase review remains separate work.
