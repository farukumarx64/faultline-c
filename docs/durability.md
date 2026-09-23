# Coordinator durability contract

Defined 2026-09-21. [Coordinator persistence](persistence.md) now implements
this contract's durable operation ordering and startup reconciliation. The
[format step](wal-format.md) implements byte codecs and validation, and the
[writer step](wal-writer.md) implements new-file creation, locking, complete
appends, and synchronization. [Replay](wal-replay.md) implements existing-log
validation, state reconstruction, incomplete-tail repair, and append resumption.
`--wal PATH` selects the retained log; `--init-wal` explicitly creates a new one.

The promise is: **after the client receives a valid submission ACK,
the job can be recovered from the same WAL after a coordinator process crash.**
Recovery preserves the job's identity, input, retry budget, and any durably
recorded terminal outcome. It may retry interrupted work; it does not promise
successful or exactly-once execution.

## Scope and storage policy

- One coordinator owns one local append-only write-ahead log (WAL). It must
  hold an exclusive lock for its lifetime; a second writer refuses startup.
- The MVP uses synchronous persistence: write the complete record and call
  `fsync()` successfully for every durable transition before publishing its
  effects. There is no asynchronous acknowledgment mode or group commit yet.
- The recovery guarantee covers coordinator process exit/crash, including
  SIGKILL, with the same retained WAL and available local storage. It assumes the
  filesystem honors successful I/O and synchronization operations. Storage loss,
  manual log deletion/rollback, corruption repair, replication, and universal
  OS-crash/power-loss survival are outside this first contract.
- Ordinary recovery requires the existing WAL. Creating an empty store must be
  an explicit initialization action. Missing, inaccessible, incompatible, or
  corrupt state must not silently become a new empty coordinator. `--init-wal`
  explicitly creates the file selected by `--wal` (default `faultline.wal`).
- A newly initialized WAL must have its header/file synced and its parent
  directory synced before service begins. Initialization must not overwrite an
  existing log. The first implementation does not rotate or compact the WAL.

Successful `write()` or C-library `fflush()` alone is not the chosen durability
boundary. File synchronization and directory-entry synchronization are distinct;
see the [Linux fsync manual](https://man7.org/linux/man-pages/man2/fsync.2.html).
Apple documents that `fsync()` can leave data in a drive cache and describes
`F_FULLFSYNC` for stronger flushing. This contract deliberately claims process
crash recovery, not a platform-independent power-loss guarantee. See
[Apple's fsync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html).

Synchronous storage work can delay the single coordinator event loop. The first
implementation favors a clear acknowledgment boundary over throughput; later
measurements must include flush latency and its effect on heartbeat processing.

## State that must survive

| State | Required recovered meaning |
| --- | --- |
| Job definition | Original nonzero job ID, task type, exact argument bytes/length, and `max_retries`. |
| Job outcome | Last valid durable state; exact result bytes/length for DONE; failure reason for a failed/requeued attempt. DONE and FAILED remain terminal. |
| Attempt history needed for safety | Latest attempt number, retry count, and last assigned worker ID. Recovery must never reset the budget or reuse a durable attempt number. |
| Job timestamps | Creation/update and latest assignment/start/finish values, including unset markers, on the logical job timeline defined below. Creation time and terminal metadata are preserved. |
| Pending order | The FIFO order established by durable submissions and requeues. It cannot be reconstructed by sorting all jobs by ID. |
| Job ID allocation | Highest durably allocated job ID, including terminal jobs. The next ID is greater; exhaustion rejects allocation rather than wrapping. |
| Worker ID allocation | Highest durably allocated worker ID, including registrations that never owned a job. Persist allocation before sending its registration ACK. |
| Log order | A contiguous record sequence starting at 1, with no gaps, duplicates, or wraparound. Exhaustion stops writes. Replay applies transitions in this order. |

One logical record must represent a complete transition, including its job,
counter, and queue consequences. For example, recovering a requeue without its
retry increment, or a created job without reserving its ID, is forbidden. The
[WAL format](wal-format.md) specifies the record types and exact serialized
bytes; raw C structs are never the persistent format.

The existing 256-job store limit includes retained DONE and FAILED records.
Persistence means a restart does not free those slots. Replay must reject
state that exceeds supported limits rather than dropping records. Eviction,
archival, and WAL compaction are separate future work; the log can continue to
grow through transitions and registrations even with a bounded job count.

Live worker registrations, sockets/descriptors, heartbeat deadlines, partial
network frames, threads, and pointers do **not** survive. On restart the live
registry is empty, with its ID allocator restored. Workers need fresh
connections and registrations; the current worker exits on connection loss and
must be launched again. Restoring automatic reconnect is not part of this step.
Old worker IDs remain historical metadata, not restored scheduling authority.
Identity continuity applies to the same WAL history, not independently initialized
stores or copies run as separate coordinators.

## Submission acknowledgment boundary

The coordinator must perform these operations in order:

1. Validate the request and prepare a complete candidate job, ID allocation, and
   FIFO insertion. Check capacity and every condition that could reject it.
   Do not publish it into live scheduling yet.
2. Append the complete creation record containing everything needed to replay
   that accepted job and its enqueue operation.
3. Successfully sync the WAL with `fsync()`.
4. Publish the prepared job, queue insertion, and ID advancement in memory.
   This application must not introduce a new ordinary validation/allocation
   failure after the record is durable; an internal failure stops the coordinator.
5. Queue/send `JOB_SUBMIT_ACK` with the existing job ID payload. Only now may
   the job become available for normal dispatch.

This strengthens the existing ACK's meaning without adding an ACK field. It
means the creation is recoverable, possibly followed by later durable transitions;
it does not mean the job has started or succeeded. A submitter disconnect after
acceptance never cancels the job.

| Crash point | Required interpretation |
| --- | --- |
| Before a complete creation record exists | No success ACK was permitted; there may be no recoverable job. |
| Complete record written, sync not confirmed | No success ACK was permitted. Replay may find a complete valid job; the client's outcome is uncertain. |
| Sync succeeded, before the client receives a valid ACK | Job is recoverable, but acceptance is still unconfirmed from the client's perspective. |
| After the client receives a valid ACK | Job must be recoverable from this WAL under the stated storage assumptions. |

Replay cannot infer whether a previous `fsync()` returned or an ACK arrived at
the client. It adopts the entire valid log prefix, including complete records
that survived without a confirmed sync or ACK. It syncs that recovered prefix
before serving work. Missing an ACK is therefore not proof of rejection, and
manual resubmission can create another job. Client-request deduplication remains
outside the MVP protocol.

## Ordering other durable transitions

The general rule is **prepare and validate, append, sync, publish, then expose
the resulting action**. Only one transition is committed at a time. Invalid
requests/reports are rejected before appending anything.

| Transition | Required ordering |
| --- | --- |
| Worker registration | Persist the new worker ID allocation before publishing the registration or sending WORKER_REGISTER_ACK. Live heartbeat state remains volatile. |
| QUEUED to ASSIGNED | Persist owner, incremented attempt, and removal from pending order before publishing the reservation or sending any JOB_ASSIGN bytes. |
| ASSIGNED to RUNNING | Persist acceptance of STARTED before publishing/logging RUNNING or processing the next dependent report. |
| RUNNING to DONE | Persist the full result and terminal metadata before publishing/logging completion or scheduling new work on the released worker. |
| Task failure or worker loss | Persist the resulting QUEUED/FAILED state, failure reason, retry count, and queue effect before publishing it or dispatching a retry. |
| Startup reconciliation | Persist each interrupted attempt's recovery outcome before exposing it to scheduling. All reconciliation finishes before serving clients. |

Heartbeats and routine connection logs do not require WAL records. A connection
loss with active work does require the job transition above. A durable failure
record is authoritative even if the worker's socket has not yet been closed;
startup never restores that old live connection.

The durable DONE record is the acceptance point for a result. Computing a result,
sending it, or reading its bytes is not enough. If the coordinator crashes before
that record is recoverable, replay may see an active attempt and retry it. If DONE
is recoverable, the exact stored result survives and the job must never be retried.
There is still no completion ACK sent to the worker.

Every issued/dispatchable identity must already be durable. A synced assignment
consumes an attempt even if the coordinator crashes before actually sending it.
Gaps in allocated IDs are allowed; reuse of IDs from the recovered history is not.

## Startup and interrupted attempts

Before accepting clients or scheduling anything, startup must:

1. Exclusively open the expected WAL, validate it, and replay its complete valid
   records in sequence into an isolated store and FIFO.
2. Repair only a structurally incomplete final record as specified below, sync
   the surviving prefix, and restore allocator high-water marks and the job clock.
3. Start with no live worker connections. Treat every recovered ASSIGNED or
   RUNNING job as an interrupted attempt whose former lease is no longer valid.
4. Reconcile those active jobs in ascending job ID order. Append/sync each outcome
   before applying it. Existing queued jobs keep their FIFO order; recovered
   active jobs with retries join the tail in this deterministic recovery order.
5. Announce readiness only after replay and all recovery transitions succeed.
   Then accept fresh registrations/submissions and resume normal dispatch.

| Last recovered state | Startup action | Retry/attempt effect |
| --- | --- | --- |
| QUEUED | Keep queued in its recovered position. | No increment; no interrupted active attempt exists. |
| ASSIGNED or RUNNING, `retry_count < max_retries` | Persist WORKER_LOST and QUEUED; clear owner and assignment/start/finish timestamps; append to the queue tail. | Increment retry count once; retain the old attempt number. Increment attempt only on the next durable assignment. |
| ASSIGNED or RUNNING, `retry_count == max_retries` | Persist terminal FAILED with WORKER_LOST and a finish time; retain the last owner/attempt as history. | Neither counter increases; no queue insertion. |
| DONE | Keep the exact result and terminal metadata. | No retry or counter change. |
| FAILED | Keep the failure and terminal metadata. | No retry or counter change. |

`WORKER_LOST` expresses loss of the old assignment's usable worker connection;
it does not assert that the old physical process stopped. Startup does not wait
another heartbeat timeout. It uses the same retry allowance as task errors and
ordinary worker loss; there is no separate free restart allowance. An active
record with an already-invalid counter/state combination is corruption, not a
reason to grant more retries.

For a job with `max_retries=2`:

| State before a crash | Durable recovery outcome | Next possible assignment |
| --- | --- | --- |
| RUNNING, attempt 1, retry count 0 | QUEUED, attempt 1, retry count 1 | Attempt 2, retry count 1 |
| RUNNING, attempt 2, retry count 1 | QUEUED, attempt 2, retry count 2 | Attempt 3, retry count 2 |
| RUNNING, attempt 3, retry count 2 | FAILED, attempt 3, retry count 2 | None |

With zero retries, a recovered ASSIGNED/RUNNING job becomes FAILED immediately,
even if it had not started computing. A queued job with zero retries still gets
its original attempt. Restart cannot replenish a budget or erase a terminal
failure. These rules also apply after an orderly shutdown: an already-durable
requeue is preserved, while an active durable record is reconciled once.

### Crashing during recovery must not charge twice

Suppose attempt 1 was interrupted with retry count 0. Recovery prepares a QUEUED
record with retry count 1, then the coordinator crashes again:

- If that recovery record is incomplete/absent, replay still sees active attempt
  1 with count 0 and derives the same QUEUED/count-1 outcome.
- If the complete recovery record survives, replay sees QUEUED/count 1 and does
  not increment it again, even if the previous process died before updating RAM.
- If attempt 2 was subsequently durably assigned, that is a new interrupted
  attempt and may consume the next retry. This also holds if its send never began.

Replay reconstructs recorded outcomes; it does not rerun historical failures
against the final job record. Reconciliation applies only to jobs still active
after replay. Recovery records must identify the job and interrupted attempt so
their state/counter changes can be validated. This rule works independently for
each job if a crash interrupts reconciliation of several active jobs.

## Time across restarts

Raw monotonic readings from different coordinator sessions cannot be compared
directly. The implementation uses a **logical job timeline** for job fields:

- Preserve every recorded timestamp and `-1` unset marker during replay.
- Set a new session's base to the greatest nonnegative job timestamp in the
  recovered prefix, or zero for an empty store.
- For new transitions, use `base + elapsed_monotonic_ms_since_session_start`.
  Equal timestamps remain valid. Detect overflow and stop instead of wrapping.
- Job event times are nondecreasing across the log, not only within each job.
  Replay enforces this ordering and returns the recovered base; the transaction
  layer supplies this clock for live job changes and startup reconciliation.
- Use this job clock for all job creation/transitions, including reconciliation.
  Continue using fresh raw monotonic time for sockets, heartbeat deadlines, and
  worker execution; old heartbeat times are never restored.

This preserves timestamp ordering without inventing wall-clock dates or importing
an old heartbeat deadline. It deliberately excludes downtime: timestamp
differences across sessions are not real elapsed job age. WAL sequence establishes
transition order when timestamps are equal. Calendar timestamps, downtime-aware
latency metrics, and a full attempt audit history are separate work.

## Incomplete records and storage failures

Only a final, structurally incomplete record after a valid prefix may be discarded
automatically. Truncate to the last complete boundary and sync before appending
again. This is safe for an interrupted append because no ACK or dispatch was
allowed before the entire record and its sync succeeded.

The format must validate a complete header before trusting its declared length
to classify a trailing payload as incomplete. A damaged length/header must not
be mistaken for permission to discard an acknowledged record.

A complete record with a bad checksum, unknown version/type, impossible size,
invalid sequence, or invalid state transition must stop startup with an error,
even if it is the last record. Do not skip damaged records, scan forward for a
new magic number, or silently reset the store. The [WAL format](wal-format.md)
defines exact framing/checksum rules; corruption detection does not provide
corruption repair.

Handle short writes by continuing from the unwritten offset and interrupted
system calls according to their API rules. If a required append, sync, truncation,
or initialization operation ultimately fails, stop admission and scheduling,
report a storage error, close connections, and exit unsuccessfully. Do not send
a success ACK, dispatch an uncommitted assignment, fall back to memory-only mode,
or keep appending after an uncertain write. Storage-failure shutdown must not
try to record additional job-loss transitions through ordinary connection cleanup.
An operation whose sync failed may still appear after replay if its complete
valid record survived; the client must treat that operation as unconfirmed.

Previously synced records remain the recovery basis under the storage assumptions.
The operator must restore usable storage and restart; storage failure is not a
task error and must not consume retry allowance by itself. Subsequent startup
still reconciles any active attempts because their coordinator connections ended.

## Acceptance checks and remaining phase verification

This is the phase's acceptance matrix. The [coordinator tests](persistence.md#files-and-verification)
now cover live ordering, SIGKILL/restart, terminal results, FIFO/retry accounting,
storage failure, and startup rejection. The [writer tests](wal-writer.md#files-and-verification)
and [replay tests](wal-replay.md#verification) cover storage and reconstruction.
An exhaustive controlled crash at every listed boundary and the final phase
review remain further work; this is not a claim that every permutation has
already been exercised:

1. Kill after submission ACK; recover the exact job, arguments, budget, and ID.
2. Kill before sync/ACK and after sync but before ACK; recover valid surviving
   records without claiming the client received acceptance.
3. Restart queued jobs repeatedly; preserve FIFO order and counters. Restore
   terminal results (including binary bytes) and failures without re-execution.
4. Crash with ASSIGNED and RUNNING jobs at zero, remaining, and exhausted retry
   budgets; verify the state table and unchanged job ID.
5. Crash during recovery before append, during a partial record, after sync,
   after in-memory publication, and after a new assignment. Verify exactly one
   retry increment per interrupted durable attempt, never per startup invocation.
6. Crash around DONE persistence; preserve a durable result, allow retry only
   when replay still finds an active attempt, and reject old-attempt reports.
7. Preserve ID allocation across idle-worker registrations, lost ACKs, terminal
   jobs, and restart. Check exhaustion without wrapping. Restore an empty live
   registry; never restore sockets or heartbeat deadlines.
8. Mix pre-existing queued jobs and interrupted active jobs; verify queue order
   and no duplicate entries. Retain the full-store limit across restart.
9. Exercise partial trailing records, complete corrupt records, invalid replay
   transitions, incompatible versions, missing WALs, and competing writers.
10. Inject write/sync/truncate errors; verify no premature ACK/dispatch, no
    memory-only fallback, and no extra writes during storage-failure shutdown.
11. Replay timestamps greater than the new raw clock and force equal timestamps;
    verify valid future job transitions and fresh heartbeat timing. Check overflow.

The [WAL record format and codecs](wal-format.md) now define headers/versions,
record types, lengths, byte order, sequence, checksums, payloads, and validation
rules that encode this contract. The [WAL writer](wal-writer.md) now implements
new-log initialization and reliable append/sync. [Replay](wal-replay.md) now
restores the complete valid history and repairs permitted incomplete tails.
[Coordinator integration](persistence.md) now applies those pieces to live
operations and startup reconciliation. Further crash-point experiments and
the persistence phase review remain. The existing
[worker-recovery guarantees](recovery.md) continue to apply; persistence does
not make execution exactly once or replenish finite retry budgets.
