# WAL reading and replay

The standalone replay module now reads an existing WAL, validates its entire
surviving history, reconstructs state, repairs a permissible incomplete tail,
and returns a locked writer ready for further storage appends. The coordinator
does not use this module yet: live submissions still have in-memory ACKs and
coordinator exit still loses live state. Startup reconciliation and runtime
integration are subsequent steps in the [durability contract](durability.md).

## What replay reconstructs

[`faultline_wal_replay_open()`](../include/wal_replay.h) returns:

- A scheduler containing every retained job: identity, task, exact binary input,
  status, worker/attempt ownership, retry count and allowance, failure reason,
  timestamps, and completed result bytes.
- The FIFO queue built by applying creation, assignment, and requeue records in
  sequence order. Creation/requeue adds to the tail; assignment removes the head.
- The highest allocated job and worker IDs, and each allocator's next ID.
- The last record sequence and the greatest recorded job timestamp, used as the
  base for a future session's logical job clock.
- An empty live worker registry with only its ID allocator restored. No socket,
  liveness state, heartbeat time, or connection deadline survives a restart.

DONE and FAILED jobs remain terminal and count toward the existing 256-job
store limit. ASSIGNED and RUNNING jobs retain their exact recorded snapshots;
replay does not charge another retry, invent a worker connection, or execute work.
The next startup step must reconcile those interrupted attempts durably before
the coordinator serves requests. A writer becoming READY means that storage
appends are permitted, including reconciliation records; it is not coordinator
readiness.

For example, if the log says:

```text
create job 10                     queue: [10]
create job 20                     queue: [10, 20]
assign job 10, attempt 1          queue: [20]
start job 10
requeue job 10, retry count 1     queue: [20, 10]
```

Replay recovers queue `[20, 10]`, with job 10 still at attempt 1 and retry count
1. Reading this history repeatedly does not consume more retries. Sorting jobs
by ID would produce the wrong queue order.

Fresh job/worker allocations must increase, but gaps are allowed. After worker
IDs 2 and 7, the next ID is 8; assignment to worker 5 is invalid because it was
never allocated. A temporary index of allocated worker-ID ranges verifies exact
membership, including workers older than the live registry's 64 slots. Adjacent
IDs share one range; sparse ranges grow as needed and are released after replay.
The maximum ID leaves the corresponding next-ID field at zero, the existing
exhausted sentinel. Allocators never wrap to 1. Record sequences are stricter:
they start at 1 and have no gaps; sequence exhaustion also prohibits further appends.

Job event times must be nondecreasing across the log, matching the contract's
single logical job timeline. Equal times are valid. Individual unset timestamps
remain `-1`; an empty store has time base zero. Replay returns the recovered base
but does not implement the future live clock or add coordinator downtime.

## Opening, validation, and publication

```text
open existing regular file and acquire exclusive lock
                           ↓
validate file header and read records in order
                           ↓
validate format and apply each transition to temporary state
                           ↓
at actual EOF, remove only an allowed incomplete final record
                           ↓
sync the surviving file, then its parent directory
                           ↓
publish reconstructed state and enable the same locked writer
```

The existing-file path never creates or resets a missing log. It rejects a final
symlink and non-regular files. A nonblocking open avoids waiting on a FIFO before
the regular-file check. It shares the writer's path handling and exclusive lock;
the descriptor is not closed and reopened between validation and append. The
stable-path, cooperative-single-writer assumptions in the
[writer guide](wal-writer.md#creating-a-new-wal) still apply.

The reader accumulates short reads, retries EINTR, and treats only a zero-byte
read as EOF. An I/O error is never converted into a repairable short tail. See
the [read manual](https://man7.org/linux/man-pages/man2/read.2.html).

Validation has two layers:

1. The existing codecs check magic, version, flags, bounded lengths, sequence,
   checksums, and the validity of each individual snapshot. The complete record
   header is validated before its length is used to read a payload.
2. Replay checks history: a job must first be created, immutable fields cannot
   change, ownership and attempt must match, counters must follow the retry
   rules, assignments must take the FIFO head, and a worker cannot own two
   active jobs. Terminal jobs cannot be revived.

For job changes, replay derives the expected next snapshot using the existing
job model, encodes it canonically, and compares every persisted byte with the
record. This avoids C structure padding and catches altered arguments, budgets,
timestamps, or counters even when an altered record has valid checksums. Replay
never recomputes a completed task's result; it restores the recorded result.

The reconstructed store is built on the heap in private temporary memory.
The caller's output state remains byte-for-byte unchanged unless all validation,
any tail repair, and both syncs succeed. The writer rejects appends while in
RECOVERING state. The file is read one bounded record at a time, rather than
loading the whole history into memory. Historical worker ranges are the only
temporary index that grows with sparse allocations; allocation failure stops
replay without publishing partial state.

Complete valid surviving records are adopted even if the previous process
never confirmed their sync or delivered an ACK. The file sync establishes the
new recovery boundary, including when there is no tail to repair. The parent
directory sync also covers a surviving file whose initial creation was not
previously confirmed. This retains the contract's process-crash scope, not a
universal power-loss guarantee.

## Incomplete tails and corruption

| Observation | Action |
| --- | --- |
| Missing file, empty file, or fewer than 24 file-header bytes | Fail; never initialize implicitly. |
| Valid file header with no records | Recover an empty store; next sequence is 1. |
| EOF at a complete valid record boundary | Recover all records. |
| EOF with 1–31 bytes of a new record header | Remove that final fragment after validating all preceding history. |
| Valid complete record header followed by a short payload at EOF | Remove that final record's header and partial payload. |
| Complete bad header, payload, checksum, sequence, or history transition | Fail, including at the end of the file; leave bytes intact. |
| Read error before EOF | Fail without truncating. |

After the maximum record sequence, any additional bytes are an error because
there is no legal next record. Recovery never scans ahead for a new magic value
or skips a damaged complete record. Checksums detect accidental damage, not
malicious alteration or loss of a complete suffix at a valid record boundary.

For an allowed tail, `ftruncate()` removes bytes after the last fully validated
record. Replay then syncs before returning success. Truncation does not itself
flush the file or reset its offset; the retained writer uses append mode, so its
next write goes to the repaired end. See the
[truncate manual](https://man7.org/linux/man-pages/man2/ftruncate.2.html).

If truncation succeeds but a later sync fails, the physical tail may already
have changed. Replay still returns failure, leaves the caller's state unchanged,
and disables the writer. It cannot promise to undo a storage operation that
already happened. Close the failed handle and use a new handle to attempt recovery.

## API and diagnostics

Initialize the writer with `FAULTLINE_WAL_WRITER_INIT`, allocate the large output
state on the heap, and supply an output report. All pointers must be valid and
non-overlapping. Always close the writer after an opening attempt, including
failure. A successful handle retains its lock until close and resumes at the
next validated record sequence.

Results distinguish invalid arguments/state, storage errors, invalid format,
invalid history, retained-job capacity, and memory exhaustion. The report gives
the validated byte boundary, last accepted sequence, incomplete tail size, failing
record/header offset, and format/history reason. The writer retains its failed
operation and system error. A report describes diagnostic progress on failure;
it does not make that prefix available for scheduling.

Appending a new record through the recovered writer still requires the caller
to validate and prepare that transition, then publish it after append/sync
success. The writer does not automatically update the recovered scheduler.

Implementation files:

- [`include/wal_replay.h`](../include/wal_replay.h): recovery state, reports, API.
- [`src/coordinator/wal_replay.c`](../src/coordinator/wal_replay.c): reader,
  historical validation, state reconstruction, tail repair, and publication.
- [`src/coordinator/wal_writer.c`](../src/coordinator/wal_writer.c): shared open,
  regular-file check, locking, sync/error handling, and append continuation.
- [`src/coordinator/wal_replay_internal.h`](../src/coordinator/wal_replay_internal.h)
  and the writer's internal header: controlled I/O fault injection for tests.

## Verification

```sh
make test-wal-replay
make SANITIZE=1 test-wal-replay
make test-unit all
make SANITIZE=1 test-unit all
```

The twelve new groups in [`tests/test_wal_replay.c`](../tests/test_wal_replay.c)
bring the C unit total to 100. All 100 pass in normal and ASan/UBSan builds.
Coverage includes mixed terminal/queued/active jobs, binary inputs/results,
retries, FIFO, sparse and exhausted identities, timestamps, and store capacity.
Repeated replay preserves the same state and consumes no extra retries.

The tail test exercises every incomplete prefix of a maximum 2164-byte record,
plus complete-record recovery and appending after repair. Corruption tests cover
file headers, record headers, payloads, sequences, and 25 checksummed but
impossible histories. Fault tests use real temporary files with short reads,
EINTR, read errors, truncate errors, and file/directory sync failures; they verify
that neither recovered state nor append permission is published prematurely.

A process test syncs one record, begins the next append, pauses after seven
bytes, and receives SIGKILL. The parent recovers the first record, removes the
seven-byte tail, appends the next record, and replays again. This demonstrates
the standalone storage recovery path. It does not establish coordinator restart
recovery, client ACK ordering, or power-loss survival. Those runtime checks remain
part of the next integration work.
