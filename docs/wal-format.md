# WAL format, version 1

Defined 2026-09-21. The byte format, in-memory encoders/decoders, and format tests
are implemented. File creation, locking, append/sync, replay, and coordinator
integration are later steps. The coordinator still loses its in-memory state
on exit. See the [durability contract](durability.md) for the behavior this
format must eventually support.

A WAL is an ordered history of durable state changes. A future writer will
append one complete record and sync it before publishing its effects. A future
reader will validate records in order and reconstruct jobs, queue order, and ID
allocation. The codec only converts between C values and bytes; successful
encoding is not a disk write or a durability guarantee.

## Layout and byte order

```text
24-byte file header
32-byte record header | payload
32-byte record header | payload
...
```

There is one file header, then consecutive records with no padding, separators,
footer, or end marker. A valid header with no records represents an initialized
empty history. A zero-byte file is not initialized. Version 1 has no compression,
rotation, compaction, optional records, or extension fields that readers may skip.

Every multibyte integer, including lengths, IDs, timestamps, and checksums, is
**big-endian**: most significant byte first. For example, `0x11223344` is
`11 22 33 44`. Fields are written explicitly; C struct padding, native enum widths,
`size_t` widths, pointers, and machine byte order are never serialized.
Byte offsets in the following tables are zero-based.

## File header: 24 bytes

| Offset | Bytes | Field | Version 1 value |
| --- | --- | --- | --- |
| 0 | 8 | Magic | `46 4c 49 4e 57 41 4c 00` (`FLINWAL` followed by zero). |
| 8 | 2 | Format version | `1`. |
| 10 | 2 | File header size | `24`. |
| 12 | 4 | Flags | `0`; other values are rejected. |
| 16 | 4 | Reserved | `0`; other values are rejected. |
| 20 | 4 | Header CRC32 | CRC of the preceding 20 bytes. |

The exact version 1 header is:

```text
46 4c 49 4e 57 41 4c 00  00 01 00 18 00 00 00 00
00 00 00 00 b6 59 66 1f
```

The reader validates all 24 bytes before processing any record. Missing,
incomplete, incompatible, or corrupt file headers are startup errors, never
permission to initialize an empty store. Future initialization must sync this
header/file and its parent directory as required by the durability contract.

## Record header: 32 bytes

| Offset | Bytes | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | Magic | `0x464c5752`, ASCII `FLWR` (Faultline WAL record). |
| 4 | 2 | Record format version | `1`. |
| 6 | 2 | Record type | One of the seven types below. |
| 8 | 4 | Payload length | Payload bytes only; excludes this header. |
| 12 | 4 | Flags/reserved | Must be `0`. |
| 16 | 8 | Sequence | Nonzero, starts at `1`, increases by exactly one per record. |
| 24 | 4 | Payload CRC32 | CRC of exactly the payload bytes. |
| 28 | 4 | Header CRC32 | CRC of header bytes 0 through 27, including the payload CRC. |

The next record begins exactly `32 + payload_length` bytes after this one.
The largest version 1 record is **2164 bytes**. The sequence never wraps;
`UINT64_MAX` may be the last record, but then no further append is possible.
Unknown versions, types, flags, and impossible lengths are errors.

## Record types and their meaning

These IDs belong to the disk format; they are independent of TCP message IDs.
A network request asks for a change. A WAL record describes the accepted change
after coordinator validation.

| ID | Type | Payload | Meaning for future replay |
| --- | --- | --- | --- |
| 1 | `WORKER_ID_ALLOCATED` | 4-byte worker ID | Advance the durable worker-ID allocation high-water mark. |
| 2 | `JOB_CREATED` | Job snapshot | Create a QUEUED job, reserve its job ID, and append it to the FIFO tail. |
| 3 | `JOB_ASSIGNED` | Job snapshot | Remove the oldest queued job; record its owner and new attempt. |
| 4 | `JOB_STARTED` | Job snapshot | Change the matching ASSIGNED attempt to RUNNING. |
| 5 | `JOB_COMPLETED` | Job snapshot | Change the matching RUNNING attempt to DONE and retain its exact result. |
| 6 | `JOB_REQUEUED` | Job snapshot | End a failed/lost active attempt, increment its retry count, clear its owner, and append to the FIFO tail. |
| 7 | `JOB_FAILED` | Job snapshot | End an active attempt as terminal FAILED because its retry allowance is exhausted. |

`WORKER_ID_ALLOCATED` contains exactly one nonzero big-endian `uint32_t` ID.
It is needed even for a registered worker that never receives a job: restart
must not reuse that worker's durable ID. It does not restore a live worker,
socket, connection, heartbeat, or scheduling eligibility.

Each job record contains the **complete job snapshot after its transition**.
That repeats the original arguments in later records, but makes the resulting
state, counters, timestamps, and result explicit. Bounded payloads keep the
format small enough for the MVP. Replay must still compare the snapshot with
the preceding state; a snapshot is not permission to overwrite arbitrary state.

There is no separate queue-insertion record or retry-counter record. Those are
consequences of the same job transition. This avoids recovering a retry without
its counter increment or a submission without its queue entry.

## Job payload: 84-byte prefix followed by data

Offsets below start at the payload, immediately after the 32-byte record header.
All integers are unsigned on disk; timestamps have the special encoding below.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Job ID, nonzero. |
| 8 | 2 | Task type. |
| 10 | 2 | Job state after this transition. |
| 12 | 4 | Worker ID; zero for QUEUED, historical owner otherwise. |
| 16 | 8 | Attempt number. |
| 24 | 4 | Retry count already consumed. |
| 28 | 4 | Maximum retries allowed. |
| 32 | 8 | Creation time, milliseconds. |
| 40 | 8 | Last update time, milliseconds. |
| 48 | 8 | Latest assignment time, milliseconds or unset. |
| 56 | 8 | Latest start time, milliseconds or unset. |
| 64 | 8 | Latest finish time, milliseconds or unset. |
| 72 | 2 | Failure reason. |
| 74 | 2 | Reserved, must be zero. |
| 76 | 4 | Argument length, `A`. |
| 80 | 4 | Result length, `R`. |
| 84 | A | Original argument bytes. |
| 84 + A | R | Result bytes. |

The payload length must be exactly `84 + A + R`. Both lengths are at most 1024.
They describe raw bytes: empty data and embedded zero bytes are permitted;
there is no string terminator. Only DONE records may contain result bytes.
Other job payloads are 84–1108 bytes; DONE payloads are 84–2132 bytes.
Task-specific argument/result interpretation remains in the task layer.

Version 1 fixes these enum values:

| Field | Values |
| --- | --- |
| Task | `1=sleep`, `2=prime_count`, `3=fibonacci`, `4=hash`. |
| State | `1=QUEUED`, `2=ASSIGNED`, `3=RUNNING`, `4=DONE`, `5=FAILED`. |
| Failure | `0=NONE`, `1=TASK`, `2=WORKER_LOST`. |

Times from zero through `INT64_MAX` are encoded as their unsigned 64-bit values.
Eight `ff` bytes (`UINT64_MAX`) encode the model's `-1` unset marker. Other values
above `INT64_MAX` are rejected. The decoder checks this before converting to a
signed C integer. Unset is accepted only where the job state permits it.
These are logical job times, not calendar dates or persisted heartbeat deadlines;
see [job time across restarts](durability.md#time-across-restarts).

## Snapshot validation

Both encoding and decoding enforce these rules. IDs/tasks must be valid,
`retry_count <= max_retries`, creation time is nonnegative, and update time is
at least creation time. Empty results are valid for DONE too.

| Record | Required snapshot |
| --- | --- |
| CREATED | QUEUED; worker/attempt/retries are zero; failure NONE; update equals creation; assignment/start/finish unset. |
| ASSIGNED | ASSIGNED; nonzero worker; `attempt = retry_count + 1`; assignment at/after creation; update equals assignment; start/finish unset; failure NONE. |
| STARTED | RUNNING; same active-counter rule; start at/after assignment; update equals start; finish unset; failure NONE. |
| COMPLETED | DONE; same active-counter rule; start at/after assignment; finish at/after start; update equals finish; failure NONE. |
| REQUEUED | QUEUED; worker zero; nonzero retry count; `attempt = retry_count`; assignment/start/finish unset; failure TASK or WORKER_LOST. |
| FAILED | FAILED; nonzero worker; `attempt = retry_count + 1`; retries equal the limit; failure TASK or WORKER_LOST; finish at/after assignment and any start; update equals finish. Start may be unset if failure occurred before STARTED. |

For every active or terminal job, assignment is at/after creation and update is
at/after assignment. Non-DONE records have no result. Equal event times are valid.
The largest valid active attempt is `UINT32_MAX + 1`, which fits the 64-bit field.

These checks describe one record. **History validation belongs to future replay**:

- Enforce sequences from 1 with no gaps, duplicates, or wraparound. The decoder
  compares each record against an expected sequence supplied by its caller.
- Require increasing fresh worker/job allocations, restore their high-water
  marks, reject duplicate creation, and enforce the 256 retained-job limit.
- Preserve each existing job's ID, task, exact arguments, retry limit, and creation
  time. Require a valid previous state, matching owner/attempt, correct counter
  changes, and nondecreasing transition times.
- Validate assigned workers against prior allocations and historical active
  ownership. A worker must not own two simultaneous active attempts. This does
  not recreate a live registry on startup.
- Apply CREATED/REQUEUED enqueue and ASSIGNED dequeue in sequence order, checking
  the FIFO head and preventing duplicate entries. Sorting jobs by ID is insufficient.
- Preserve DONE/FAILED terminal records. Reconcile recovered active jobs using
  the durability contract before admitting new work.

For example, a perfectly checksummed COMPLETED snapshot with no preceding
creation/assignment/start records must fail replay even though its bytes decode.

## Checksums and corruption detection

Use CRC-32/ISO-HDLC (the IEEE CRC32 used by gzip): polynomial `0x04c11db7`,
reflected implementation polynomial `0xedb88320`, initial value `0xffffffff`,
reflected input/output, final XOR `0xffffffff`. Check values are `cbf43926` for
ASCII `123456789` and zero for empty input. See
[RFC 1952, CRC algorithm](https://www.rfc-editor.org/rfc/rfc1952.html#section-8).
Faultline stores the resulting integer big-endian; it does not adopt gzip framing.

The file CRC covers its first 20 bytes. Each record's payload CRC covers exactly
its payload; its header CRC covers the first 28 header bytes, including the
stored payload CRC. Checksums never include their own storage bytes.

**Validate the fixed header before trusting its length.** With only 32 bytes
available, the reader can check the header CRC, magic, version, flags, type,
bounded length, and expected sequence. Only then may it wait for that payload.
Once present, validate the payload CRC and its lengths/fields/snapshot rules.

Why two checksums? Suppose a bit flip changes a payload length from 100 to 101.
If checksum verification required the alleged full payload first, a reader at
EOF could mistake corruption for an interrupted append and discard a record.
The independent header CRC rejects that damaged length immediately.

CRCs detect accidental corruption, not malicious tampering; collisions are
possible. They do not authenticate the writer, prove a sync/ACK happened, or
detect removal of an otherwise valid final suffix. Sequence checks detect gaps,
duplicates, and reordering among observed records, not missing records after EOF.
The retained-storage assumptions in the durability contract still apply.

## Incomplete input and future file-reader policy

The codec returns `INCOMPLETE` when it needs more bytes. That is a buffer result,
not proof of EOF and not permission to truncate a file.

| File-reader observation | Required action when file reading is implemented |
| --- | --- |
| Fewer than 24 file-header bytes | Refuse startup. |
| EOF exactly at a validated record boundary | Valid end of history. |
| EOF with 1–31 trailing record-header bytes after a valid prefix | Incomplete tail: truncate to the preceding boundary and sync. |
| Complete valid record header, EOF before its declared payload ends | Incomplete tail: truncate to the preceding boundary and sync. |
| Complete invalid header, including bad CRC/sequence/length | Refuse startup, even at EOF. |
| Complete record with bad payload CRC, invalid fields, or invalid replay transition | Refuse startup, even for the last record. |

Never scan forward for another magic value or skip a complete bad record.
An incomplete tail may be removed only after validating the file header and
every preceding record, observing actual EOF, and applying the durability
contract. Complete valid surviving records are replayed even if the previous
process never confirmed sync or delivered an ACK. Replay must sync the recovered
prefix before serving work. None of these file operations is implemented here.

## How a retry is represented

An illustrative history for one job with one retry is:

```text
seq 1  WORKER_ID_ALLOCATED worker=1
seq 2  WORKER_ID_ALLOCATED worker=2
seq 3  JOB_CREATED   job=1 state=QUEUED   worker=0 attempt=0 retries=0 max=1
seq 4  JOB_ASSIGNED  job=1 state=ASSIGNED worker=1 attempt=1 retries=0
seq 5  JOB_STARTED   job=1 state=RUNNING  worker=1 attempt=1 retries=0
        worker 1 is lost
seq 6  JOB_REQUEUED  job=1 state=QUEUED   worker=0 attempt=1 retries=1 failure=WORKER_LOST
seq 7  JOB_ASSIGNED  job=1 state=ASSIGNED worker=2 attempt=2 retries=1
seq 8  JOB_STARTED   job=1 state=RUNNING  worker=2 attempt=2 retries=1
seq 9  JOB_COMPLETED job=1 state=DONE    worker=2 attempt=2 retries=1 result=...
```

Every job line also carries the original arguments, retry limit, and timestamps.
Requeue clears current ownership; the interrupted owner remains in the preceding
assignment/start record. A future restart that finds the durable requeue at
sequence 6 restores an already-queued job with one retry consumed, without
charging that interrupted attempt again. Restart reconciliation uses these same
REQUEUED/FAILED record types with WORKER_LOST; it needs no separate retry reset.

## C API and tests

[`include/wal.h`](../include/wal.h) defines constants, host representations,
record types, result codes, and five codec functions. Their implementation is
[`src/coordinator/wal_format.c`](../src/coordinator/wal_format.c).

- `faultline_wal_file_header_encode/decode`: create/check the fixed file header.
- `faultline_wal_record_header_decode`: validate a fixed record header before
  trusting the payload length. Success does not validate its payload yet.
- `faultline_wal_record_encode/decode`: serialize/validate one complete record,
  reporting bytes written/consumed on success.

All pointers are required and must describe valid non-overlapping storage.
Invalid arguments, insufficient encoder capacity, incomplete decoder input,
checksum errors, and invalid fields leave every output unchanged. Decoders accept
unaligned input and ignore following bytes; the consumed count lets a future
reader advance to the next record. Decoded jobs own their argument/result bytes.
Encoders never write unused array capacity or native struct padding.

The implementation allocates no heap memory and performs no file/socket I/O,
`fsync()`, scheduler mutation, or replay. It currently links into the WAL tests,
not the coordinator executable. Runtime ACK and retry behavior are unchanged.

```sh
make test-wal
make SANITIZE=1 test-wal
make test-unit
make SANITIZE=1 test-unit
```

[`tests/test_wal.c`](../tests/test_wal.c) has ten groups. Literal file/header and
all-seven-record fixtures were independently generated with Python `struct.pack`
and `zlib.crc32`, rather than the C encoder. Coverage includes every incomplete
prefix and insufficient capacity, the maximum-size record, 6216 individual
single-bit corruptions across the fixtures, semantic errors with recomputed
valid CRCs, sequence mismatches, integer/time boundaries, ownership, unaligned
buffers, and snapshots produced by the existing job model. These are format
checks, not claims that disk recovery already works.

The next step is a WAL file writer: explicit initialization/opening and locking,
complete append handling, and the required sync/error boundary. Replay and
coordinator integration follow as separate steps.
