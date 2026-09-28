# List retained jobs and worker liveness

`faultline jobs` and `faultline workers` read one bounded coordinator snapshot,
print a table sorted by ID, and exit. Both default to `127.0.0.1:9000` and accept
`--coordinator IPv4:PORT`:

```sh
./build/debug/faultline jobs
./build/debug/faultline workers
./build/debug/faultline jobs --coordinator 127.0.0.1:9100
```

No ID, filter, or pagination argument is required. A valid response, including
an empty list, exits 0. Invalid arguments, connection/protocol errors, and output
errors exit 1. Diagnostics go to stderr. The CLI validates the whole response
before printing a count, table header, or row, so a truncated response cannot
look like a complete listing. It does not automatically retry or poll.

## Jobs

For aggregate counters and their restart scopes, use [`faultline stats`](stats.md).

For example (spacing shortened here):

```text
jobs=3
JOB_ID TASK  STATE   WORKER_ID ATTEMPT RETRIES FAILURE     RESULT_BYTES
1      hash  DONE    1         1       0/1     NONE        16
2      sleep RUNNING 2         2       1/2     NONE        0
3      hash  QUEUED  none      1       1/1     WORKER_LOST 0
```

The list includes all retained jobs: queued, assigned, running, completed, and
failed. It shows task type, current state, current/last worker, attempt, consumed
retry allowances and limit, failure reason, and saved result length. QUEUED has
no owner; DONE/FAILED retain the last owner as history. A queued retry retains
its last attempt and failure until reassignment. These are the same meanings as
[`status ID`](status.md). An empty store prints:

```text
jobs=0
No retained jobs.
```

Rows are in ascending job ID order, not FIFO execution order. A retried job can
have a lower ID while waiting behind a newer job. The command does not expose
queue position. Arguments and result bytes are omitted to keep every row small;
use `faultline status ID` for the saved result. No terminal jobs are evicted by a
query. All 256 retained job slots can be listed, even when submissions are full.

## Workers

```text
workers=3 heartbeat_timeout_ms=6000
WORKER_ID LIVENESS HEARTBEAT_AGE_MS JOB_ID ATTEMPT
1         ALIVE    180              2      1
2         ALIVE    32               none   0
3         DEAD     7200             none   0
```

`JOB_ID` and `ATTEMPT` identify an active ASSIGNED/RUNNING job. `none` and zero
mean no active assignment. Liveness and activity are separate: an ALIVE worker
may be idle. Heartbeats demonstrate contact, not execution progress.

Heartbeat age is a duration computed by the coordinator at snapshot time, in
milliseconds since its last accepted heartbeat. Registration supplies the initial
baseline before the first heartbeat. The CLI never subtracts timestamps from
different machines. The age is not updated while the response travels or after
the command exits. A DEAD worker's age is time since its last heartbeat, not
time since death.

| Display | Meaning at snapshot time |
| --- | --- |
| ALIVE | Registry says ALIVE and heartbeat age is below the configured timeout. |
| EXPIRED | Registry still says ALIVE, but the deadline has passed and normal event-loop cleanup is pending. |
| DEAD | The coordinator has marked that registration dead. |

EXPIRED is computed for display from registry state, age, and timeout. It does
not introduce a new registry state or perform cleanup from a query. An EXPIRED
row can still show the assignment pending normal worker-loss handling. The
coordinator's ordinary event loop revokes the lease and applies retry policy;
listing neither postpones nor initiates that transition.

Only nonempty registry slots are included. Dead entries remain until a later
registration reuses their slot; new registrations get fresh IDs. Reuse can change
physical slot order, so the coordinator sorts copies by ID before encoding.
This is a view of up to 64 retained registrations, not a permanent worker audit
log. It contains no socket descriptors or process IDs.

After coordinator restart the live registry is empty, while the worker ID
allocator is recovered from the WAL. Workers must reconnect with new IDs. Jobs
survive and may still name a historical worker absent from this list. Empty
worker output is:

```text
workers=0 heartbeat_timeout_ms=6000
No retained worker registrations.
```

The existing limit of 64 simultaneous client/worker connections still applies;
a CLI query needs a free connection slot.

## Wire format

These are additive version 1 messages with the existing 12-byte FLIN header.
All integers are unsigned and big-endian, with no padding. IDs 1–14 are unchanged;
older peers reject the new types. There is no capability negotiation.

| ID | Message | Direction | Payload bytes | Whole frame bytes |
| --- | --- | --- | --- | --- |
| 15 | JOBS_REQUEST | Client → coordinator | 0 | 12 |
| 16 | JOBS_RESPONSE | Coordinator → client | 4 + count × 38 | 16–9744 |
| 17 | WORKERS_REQUEST | Client → coordinator | 0 | 12 |
| 18 | WORKERS_RESPONSE | Coordinator → client | 8 + count × 30 | 20–1940 |

JOBS_RESPONSE starts with a 4-byte count (0–256), then that many summaries:

| Row offset | Width | Field |
| --- | --- | --- |
| 0 | 8 | Job ID, nonzero |
| 8 | 2 | Task: SLEEP=1, PRIME_COUNT=2, FIBONACCI=3, HASH=4 |
| 10 | 2 | Job state: QUEUED=1 through FAILED=5 |
| 12 | 4 | Current/last worker ID, or zero for QUEUED |
| 16 | 8 | Attempt |
| 24 | 4 | Retry count |
| 28 | 4 | Retry limit |
| 32 | 2 | Failure: NONE=0, TASK=1, WORKER_LOST=2 |
| 34 | 4 | Saved result length, 0–1024; no result bytes follow |

The [status consistency rules](job-status-protocol.md#state-and-counter-consistency)
also apply to each summary. Only DONE may have a nonzero result length.

WORKERS_RESPONSE starts with a 4-byte count (0–64) and a nonzero 4-byte heartbeat
timeout in milliseconds, then these rows:

| Row offset | Width | Field |
| --- | --- | --- |
| 0 | 4 | Worker ID, nonzero |
| 4 | 2 | Recorded registry state: ALIVE=1 or DEAD=2 |
| 6 | 8 | Heartbeat age in milliseconds |
| 14 | 8 | Active job ID, or zero |
| 22 | 8 | Active attempt, or zero |

Job ID and attempt must either both be zero or both be nonzero. DEAD rows have
neither. An ALIVE row can have age at/above the timeout, supporting the EXPIRED
display without changing the recorded state. Rows in both formats must have
strictly increasing IDs: duplicates and descending order are rejected.

The codec checks count limits before multiplication or array access, and the
outer payload length must equal the prefix plus count times row size exactly.
Counts and lengths are rejected from the available prefix; row validation waits
for the bounded body. Partial valid frames preserve outputs and return
BUFFER_TOO_SMALL. The decoder owns copies of all rows. The maximum shared frame
capacity is now 9744 bytes, with compile-time checks against both store limits.

## Snapshot and transport guarantees

The coordinator copies and encodes the whole listing in one event-loop action.
Sorting touches only the copy. Later state changes cannot alter buffered reply
bytes, even if transmission takes several writes. Separate commands are separate
snapshots and may disagree because events happened between them.

Queries allocate no identities, append no WAL records, consume no retries,
reorder no queue entries, and refresh no heartbeat times. Startup replay and
reconciliation complete before queries are served. Job state and results retain
their existing [durability guarantees](durability.md).

Unregistered client connections may mix listings, status, stats, submissions, and PING
with serialized replies. After any valid job/list query or submission, the
connection cannot become a worker. Registered worker connections cannot submit
listing requests. Response types are outbound-only at the coordinator.

The CLI shares its query receiver with `status` and `stats`: one five-second response
deadline covers header, fixed prefix, and every row. Connect and send have their
own five-second budgets. Oversized declarations, inconsistent counts, invalid
rows, unexpected reply types, and premature EOF fail without partial tables.

## Verification

```sh
make test-list-protocol
make test-listings
make SANITIZE=1 test-listings
make test-listings INTEGRATION_ARGS='--port 9000'
```

Seven C groups cover independent literal bytes, empty lists, integer limits,
maximum counts, every truncated prefix of both maximum frames, short output
buffers, invalid rows/counts/order, owned copies, and mixed streams. Process
checks cover real CLI/coordinator/worker behavior, read-only WAL comparisons,
all retained job states, full job storage, stable encoded snapshots, heartbeat
ages and expiry, dead-slot reuse, restart recovery, fragmented/coalesced frames,
malformed/truncated replies, endpoint/options, and whole-response deadlines.
