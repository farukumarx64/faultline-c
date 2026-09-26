# Coordinator persistence

The coordinator now uses the WAL for every accepted job transition and worker-ID
allocation. Its submission ACK means the job's creation record has been appended
and successfully synchronized. Startup recovers the same file and reconciles
interrupted attempts before opening the listening socket. This implements the
ordering in the [durability contract](durability.md), with the same retained local
storage and process-crash assumptions. It does not promise exactly-once execution,
automatic coordinator failover, or universal power-loss survival.

## Starting and restarting

Create a new log explicitly on the first launch:

```sh
make
./build/debug/faultline-coordinator --wal faultline.wal --init-wal --port 9000
```

For subsequent launches, recover that file:

```sh
./build/debug/faultline-coordinator --wal faultline.wal --port 9000
```

The default path is `faultline.wal` in the current directory. `--wal PATH` selects
another file whose parent directory already exists. `--init-wal` creates a new
file and refuses to overwrite any existing path. Without it, a missing, locked,
incompatible, or corrupt log fails startup; there is no in-memory fallback or
implicit empty-store initialization. The default file is ignored by Git.

CLI submission and worker commands are unchanged. Worker and job IDs continue
from their recovered allocation counters. Workers need fresh connections and
registrations after coordinator exit; the worker executable currently exits on
connection loss rather than reconnecting automatically.

## One transaction at a time

The new [`coordinator_store`](../include/coordinator_store.h) owns the published
scheduler, live registry, writer, and logical job clock. It also owns one reusable
scratch scheduler. Each job operation follows this sequence:

```text
copy the published scheduler into scratch
                  ↓
validate and apply the proposed operation to scratch
                  ↓
append its full post-transition snapshot and successfully fsync
                  ↓
copy scratch into the published scheduler and return outputs
                  ↓
queue ACK/assignment bytes, announce results, or schedule subsequent work
```

The existing scheduler and job model still implement FIFO and transition rules.
They run on scratch first. Any invalid request, stale report, capacity rejection,
or exhausted job allocator leaves live state unchanged and writes no record.
The final copy cannot allocate or introduce an ordinary validation failure after
the record is durable. Job addresses in the published scheduler remain stable.

This deliberately copies the bounded 256-job store for each job transition. It
uses extra heap memory and memory bandwidth to keep the first transactional
implementation simple. Synchronous flushes can also delay the event loop.
Throughput tuning, smaller transaction deltas, and group commit are future work;
none may weaken the publication boundary.

Worker registration uses a temporary registry copy. It validates capacity and
allocates a candidate ID, persists that allocation, then publishes the registry
and exposes the ID. Heartbeats and live connection/death metadata remain volatile.

## What is made durable before exposure

| Operation | Saved record | Only after successful sync |
| --- | --- | --- |
| Register worker | `WORKER_ID_ALLOCATED` | Publish registration and queue its ACK. |
| Submit job | `JOB_CREATED` | Publish job/ID/FIFO insertion and queue `JOB_SUBMIT_ACK`. |
| Assign job | `JOB_ASSIGNED` | Publish owner/attempt/dequeue and queue `JOB_ASSIGN`. |
| Accept STARTED | `JOB_STARTED` | Publish/log RUNNING and process dependent reports. |
| Accept COMPLETED | `JOB_COMPLETED`, including all result bytes | Publish/log DONE, release worker, schedule next job. |
| Accept task failure or detect worker loss | `JOB_REQUEUED` or `JOB_FAILED` | Publish retry/outcome and make a retry dispatchable. |
| Recover an interrupted active job | `JOB_REQUEUED` or `JOB_FAILED` with WORKER_LOST | Publish reconciliation; all interrupted jobs finish reconciliation before listening. |

There is no new result ACK. A worker finishing its computation or sending a
result is not sufficient: the coordinator's accepted DONE outcome is published
only after the result record is synced. A recovered DONE job retains its exact
binary result and is not executed again.

A submission ACK can remain queued when a later operation fails. For example,
creation can be durable while an immediately attempted assignment fails before
the ACK is transmitted. The job survives, but the client did not receive success.
A missing ACK remains an uncertain outcome; manual resubmission can duplicate
work because request deduplication is not implemented.

## Startup and interruption accounting

Startup holds the log's exclusive lock while replay validates and restores jobs,
FIFO, ID counters, and results. Only permissible incomplete final records are
removed. The existing [reader](wal-replay.md) syncs the surviving history before
the transaction layer proceeds.

`recover_interrupted_jobs()` performs reconciliation through the same private
durable worker-loss operation as live disconnects. The store's `opened` flag stays
false throughout replay and reconciliation, including during every sync. Public
registration, submission, assignment, report, and worker-loss calls cannot mutate
it until recovery succeeds. The coordinator then opens the listener. A ready WAL
is necessary, but it does not by itself make the coordinator store ready.

All recovered ASSIGNED/RUNNING jobs lost their old coordinator connection. The
store visits them in ascending job ID order, using the existing worker-loss rule:

- If another retry is allowed, persist QUEUED with retry count increased once,
  owner cleared, and the job added to the FIFO tail. Retain its attempt number
  until its next durable assignment.
- Otherwise, persist FAILED with WORKER_LOST, retaining its final attempt and
  owner as historical metadata.

Existing queued jobs stay ahead of these recovered retries. DONE/FAILED remain
terminal. The live registry starts empty, with only its ID allocator restored.
The job array is already ordered by increasing creation IDs, as replay requires.

| Last saved state | Recovered outcome |
| --- | --- |
| QUEUED, including zero retries or an already-used retry allowance | Keep the exact job and FIFO position; its pending attempt can still run. |
| DONE | Keep exact binary result, timestamps, counters, and historical owner; never dispatch it again. |
| FAILED | Keep failure reason, timestamps, counters, and historical owner; never dispatch it again. |
| ASSIGNED/RUNNING with allowance remaining | Append/sync WORKER_LOST → QUEUED, increment retry count once, retain attempt number, clear worker and assignment/start/finish times, enqueue at the tail. |
| ASSIGNED/RUNNING with allowance exhausted (including zero retries) | Append/sync WORKER_LOST → FAILED; retain attempt, retry count, owner, and available assignment/start times; set finish time. |

For example, if jobs 9 and 10 were queued while jobs 5 and 6 were active and still
had retries available, the restored queue is `[9, 10, 5, 6]`. Restarting again
before dispatch does not change those counters or add more requeue records.
Only the next durable assignment advances an attempt number.

The old TCP connection cannot return when the coordinator binds the same host
and port again. A fresh socket starts unregistered; sending an old worker ID or
completion report does not restore its authority. Fresh registration allocates a
new ID above the recovered high-water mark. A terminal job's retained worker ID
is historical metadata, not a live registry entry. No heartbeat deadline or socket
descriptor is restored. The old worker process might still be computing, so this
continues to permit overlapping execution under the at-least-once policy.

If startup stops midway, another startup adopts complete surviving reconciliation
records. Those jobs are already QUEUED/FAILED and are not charged again. Remaining
active snapshots are reconciled. A failed recovery never opens the listener.
`wal_ready` reports restored job counts, repaired bytes, and interrupted jobs;
`job_recovered` logs each resulting state and any completed result.

Graceful coordinator shutdown also leaves active durable attempts for the next
startup. Shutdown closes sockets and the WAL without writing job-loss records.
This gives one interruption-accounting path and keeps storage-failure cleanup
from attempting further appends.

### Focused startup checks

```sh
make test-startup-recovery
make SANITIZE=1 test-startup-recovery
```

The C store suite covers 27 histories across all five states, budgets 0–2, and
every used-retry count allowed by those budgets. It compares every persisted
job field after recovery and another restart, checks an entirely empty live
registry, and reuses an old descriptor number with a new worker ID. A queued
job still gets its pending attempt; a terminal job is not dispatched. Sync
observers verify that public mutations are blocked throughout startup and that
failed reconciliation never sets `opened`.

Four process tests in
[`test_startup_recovery.py`](../tests/integration/test_startup_recovery.py) add:

- One ten-job history containing DONE, FAILED, queued, and both active states
  with zero, remaining, and exhausted allowances. Recover only the six active
  jobs, preserve exact terminal/queued snapshots, then dispatch in recovered FIFO
  order. Existing terminal jobs do not run again.
- Restart on the same TCP endpoint. The old socket closes; new unregistered
  sockets cannot report under old identities. A freshly registered worker receives
  the next attempt and completes it under a new worker ID.
- Repeated coordinator SIGKILL while a job is ASSIGNED, consuming two retries
  and then reaching FAILED at attempt 3. Extra restarts between assignments do
  not change the WAL or retry count.
- The same repeated-crash scenario for RUNNING jobs. A fresh job still executes
  after the original job's allowance is exhausted.

The existing persistence tests also inject incomplete reconciliation writes and
sync failures, requiring safe retry accounting on the next startup. These checks
use controlled TCP peers; they do not add automatic reconnect to the worker or
prove that a disconnected task process stopped computing.

## Clocks and fatal failures

Job times use `recovered_base + (raw_now - session_start)`. The base is the greatest
recovered job timestamp, or zero for a new log. Startup reconciliation uses the
base; subsequent operations use elapsed session time. Equal event times are
valid. Overflow or a job-time regression stops mutations rather than wrapping.
Heartbeat/socket deadlines continue to use fresh raw monotonic readings.

The event loop refreshes raw time between clients and scheduling decisions,
because a preceding disk sync may have taken time. A queued outgoing frame gets
a fresh progress timestamp after its commit, preserving its send budget.

A failed append/sync or exhausted WAL sequence latches a fatal store failure.
The coordinator stops further request handling, pending sends, and dispatch;
closes connections without durable cleanup transitions; closes the writer; and
exits unsuccessfully. It reports `persistence_failed` with the operation, saved
system error, and replay/format/history diagnostics when applicable. A failed
record may be absent, partial, or complete. Recovery determines what survived.
The process never rolls memory forward on a failed commit or retries an uncertain
append as if it were a new operation.

## Files and verification

- [`src/coordinator/coordinator_store.c`](../src/coordinator/coordinator_store.c):
  preparation, durable commit/publication, startup reconciliation, and logical time.
- [`src/coordinator/main.c`](../src/coordinator/main.c): persistence options,
  startup-before-listen, durable handlers, failure exit, and socket cleanup.
- [`tests/test_coordinator_store.c`](../tests/test_coordinator_store.c): seven groups
  observing live state and outputs during writes/syncs, failures at all nine
  mutation variants, invalid input, reconciliation, clocks, and sequence exhaustion.
- [`tests/integration/test_persistence.py`](../tests/integration/test_persistence.py):
  seven process scenarios using independent Python WAL decoding, SIGKILL/restart,
  binary results, FIFO/retry preservation, file-size-limit write failures, startup
  rejection, and graceful shutdown. Each scenario may contain multiple cases.
- [`tests/integration/test_startup_recovery.py`](../tests/integration/test_startup_recovery.py):
  four focused startup scenarios described above; `test-startup-recovery`
  selects the focused suite.
- [`tests/integration/test_coordinator_crashes.py`](../tests/integration/test_coordinator_crashes.py):
  ten methods covering 35 live-write crash cases, ten interrupted-startup cases,
  and two real CLI/worker scenarios. The [crash guide](coordinator-crashes.md)
  explains the test-only harness and exact boundaries. `test-persistence`
  includes all three process suites.

```sh
make test-persistence
make SANITIZE=1 test-persistence
make test
make SANITIZE=1 test
```

The file-size limit affects only test child processes and private temporary WALs;
it does not fill the disk. Diagnostics use pipes. There are no production fault
switches. C tests inject sync failures through an internal I/O seam while checking
that outputs, live jobs, and allocation counters remain unpublished during I/O.

The suite now contains 114 C groups and 113 process scenarios (two default-port
checks are skipped with automatic ports). Existing process fixtures each create
their own temporary WAL. The persistence scenarios establish concrete restart
and ordering behavior, including selected crashes before/during/after WAL writes
and flushes. The [phase review](persistence-review.md) records the full regression
results and precise guarantees. These checks do not simulate
power loss, prove exactly-once side effects, or benchmark flush latency.
