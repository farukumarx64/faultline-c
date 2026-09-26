# Job messages and payloads

The shared codec in `include/protocol.h` and `src/common/protocol.c` defines six
job lifecycle messages. Three additional read-only query formats are defined in
[job-status-protocol.md](job-status-protocol.md). Each uses the existing [12-byte version 1 header](protocol.md).
All metadata integers are unsigned and big-endian; arguments/results are opaque
bytes. No C struct, enum representation, pointer, or `size_t` is sent directly.

Encoding, decoding, validation, and runtime submission/assignment/report handlers
are implemented. The CLI can submit jobs; the coordinator stores and schedules
them using the [job model](jobs.md) and [FIFO queue](queue.md). Workers receive
and execute assignments while heartbeating, then send real execution reports.
The [task guide](tasks.md) defines numeric inputs and result formats.
See [scheduling.md](scheduling.md) for runtime behavior and limits.

## Message overview

Existing message IDs 1–5 are unchanged. New IDs are explicit and must not be
renumbered. This is an additive version 1 extension: old binaries reject unknown
job types, so job-capable peers will need matching implementations. There is no
version or capability negotiation.

| ID | Message | Direction | Payload size | Whole frame size |
| --- | --- | --- | --- | --- |
| 6 | `JOB_SUBMIT` | CLI → coordinator | 10 + argument bytes | 22–1046 bytes |
| 7 | `JOB_SUBMIT_ACK` | Coordinator → CLI | 8 | 20 bytes |
| 8 | `JOB_ASSIGN` | Coordinator → worker | 26 + argument bytes | 38–1062 bytes |
| 9 | `JOB_STARTED` | Worker → coordinator | 20 | 32 bytes |
| 10 | `JOB_COMPLETED` | Worker → coordinator | 24 + result bytes | 36–1060 bytes |
| 11 | `JOB_FAILED` | Worker → coordinator | 22 | 34 bytes |

Arguments and results each allow 0–1024 bytes, matching the model's limits. The
largest frame in this table is the 1062-byte assignment. The shared
`FAULTLINE_MESSAGE_MAX_FRAME_SIZE` is 9744 bytes to also fit a full
[job listing](listings.md#wire-format); status responses remain at most 1072 bytes.
The generic header still allows up to 1 MiB;
complete-message validation imposes the smaller bounds above.

## Submission and acknowledgment

`JOB_SUBMIT` asks the coordinator to create and queue a job. It has no job ID:
the coordinator must allocate a unique, nonzero 64-bit ID when accepting it.
Offsets in all following tables are relative to the payload, after the header.

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 2 | `task_type` |
| 2 | 4 | `max_retries` |
| 6 | 4 | `argument_length` (0–1024) |
| 10 | `argument_length` | Argument bytes |

Task IDs are SLEEP=1, PRIME_COUNT=2, FIBONACCI=3, and HASH=4, shared with the job
model. Zero and unknown task types are invalid. [Built-in executors](tasks.md)
define task-specific schemas, ranges, and algorithms. The codec preserves opaque
bytes; workers interpret and validate their task arguments.
Byte counts are not string lengths: embedded zero bytes are preserved, and no
terminating NUL is appended or required.

`max_retries` counts additional attempts after the first. Zero permits one
attempt; two permits up to three. All 32-bit unsigned values are structurally
valid. The current acceptance handler preserves that value; policy may be tightened later.
The retry budget stays coordinator-owned and is not included in assignments.

`JOB_SUBMIT_ACK` has just one field:

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 8 | Coordinator-issued `job_id`, nonzero |

Its meaning is durable acceptance into the coordinator's job store and FIFO.
The coordinator ACKs only after appending and syncing the creation record, then
publishing the prepared state; queue/store exhaustion cannot produce a success
ACK. It is not a completion result. See the [durability contract](durability.md).

This initial format has no request ID or rejection payload. The intended first
CLI flow has one outstanding submission per connection, so that connection
correlates its ACK. A lost ACK leaves the client uncertain whether acceptance
occurred; blindly resubmitting may create another job. Deduplication, multiplexed
request correlation, and structured submission errors are not provided here.

## Assignment and report identity

All assignments and worker reports begin with this 20-byte identity:

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 8 | `job_id`, nonzero |
| 8 | 4 | `worker_id`, nonzero |
| 12 | 8 | `attempt`, nonzero |

A job keeps its ID across retries. Each assignment increments its attempt number,
even when reassigned to the same worker. A report for job 42 / worker 7 / attempt 1
cannot stand in for job 42 / worker 7 / attempt 2. The codec preserves the full
64-bit values, including values above `UINT32_MAX`. It checks nonzero identity,
not whether a particular job, worker, or attempt currently exists.

`JOB_ASSIGN` appends:

| Offset | Width | Field |
| --- | --- | --- |
| 20 | 2 | `task_type` |
| 22 | 4 | `argument_length` (0–1024) |
| 26 | `argument_length` | Argument bytes |

The worker must compare the assigned worker ID with its own registered identity.
The coordinator scheduler selects an alive, idle worker, assigns the model
record, and uses its new attempt number. It reserves that worker before queuing
the output frame and handles transport failures through the retry policy.

## Started, completed, and failed reports

`JOB_STARTED` contains only the common 20-byte identity. It reports that this
attempt began execution; its model operation is `faultline_job_start()`.
It does not include a worker timestamp. The coordinator timestamps accepted
reports using its own monotonic clock.

`JOB_COMPLETED` appends a result to the identity:

| Offset | Width | Field |
| --- | --- | --- |
| 20 | 4 | `result_length` (0–1024) |
| 24 | `result_length` | Result bytes |

The coordinator uses `faultline_job_complete()` for an accepted report. STARTED must be
accepted before COMPLETED: completion moves RUNNING → DONE. An empty result is
valid. The result goes to the coordinator's authoritative job record; result
retrieval by the CLI uses the status response format through
[`faultline status ID`](status.md).

`JOB_FAILED` appends a failure code:

| Offset | Width | Field |
| --- | --- | --- |
| 20 | 2 | `failure`, currently TASK=1 only |

TASK includes executor errors and inability to start an assigned attempt, so
failure may be reported from ASSIGNED or RUNNING. There is no free-form error
text in this first payload. NONE=0, WORKER_LOST=2, and unknown values are invalid
in this worker report. WORKER_LOST is a decision made locally by the coordinator when a
connection fails or heartbeats expire; a worker cannot report itself lost.
The coordinator can include WORKER_LOST in a read-only status response.

`JOB_FAILED` means **this attempt failed**. The coordinator handler calls
`faultline_job_fail()` with TASK. The model returns to QUEUED when retry allowance
remains, or reaches terminal FAILED when it is exhausted. An explicit queue push
is performed by the scheduler to make the retry pending. The message itself neither retries
nor changes the state of a record.

Coordinator report handlers validate the sending connection and its registered
worker ID, find the job by ID, and check ownership, attempt, and state through the
model API. Invalid/duplicate/stale reports close the sending connection without
applying the report. Normal worker-loss cleanup then handles any active assignment
on that connection. Valid bytes alone do not authorize an update.

## Example exchange

```text
CLI                Coordinator                         Worker 7
 | -- SUBMIT --------> |                                   |
 | <- ACK(job 42) ---- |                                   |
 |                    | -- ASSIGN(job 42, worker 7, #1) --> |
 |                    | <- STARTED(job 42, worker 7, #1) -- |
 |                    | <- COMPLETED(..., result bytes) -- |
```

A failed attempt sends FAILED instead of COMPLETED; STARTED may be absent when
the task cannot begin. Heartbeats continue independently of these messages.
There is no additional ACK for STARTED, COMPLETED, or FAILED in this format.
The worker main thread keeps heartbeats running while a separate task thread
computes. Only the main thread sends frames, preserving message boundaries.

A complete `JOB_STARTED` frame for job 42, worker 7, attempt 1 is:

```text
46 4c 49 4e | 00 01 | 00 09 | 00 00 00 14
      FLIN |    v1 | type 9 | payload length 20 (hex 14)
00 00 00 00 00 00 00 2a | 00 00 00 07 | 00 00 00 00 00 00 00 01
               job 42 |    worker 7 |                attempt 1
```

The identity's wire length is exactly 20 regardless of C struct padding.
Timestamps, queue links, and socket descriptors stay local. These six lifecycle
messages omit retries already spent; status responses expose that counter and
the retry limit. The wire contains only fields needed for the particular message.

## C representation and validation

`struct faultline_message` now contains a tagged union named `payload`.
`message_type` selects exactly one active member:

| Message | Payload member |
| --- | --- |
| PING, PONG, WORKER_REGISTER | `worker_id`, initialized to zero |
| WORKER_REGISTER_ACK, HEARTBEAT | `worker_id`, nonzero |
| JOB_SUBMIT | `job_submit` |
| JOB_SUBMIT_ACK | `job_submit_ack` (the 64-bit ID) |
| JOB_ASSIGN | `job_assign` |
| JOB_STARTED | `job_started` |
| JOB_COMPLETED | `job_completed` |
| JOB_FAILED | `job_failed` |
| JOB_STATUS_REQUEST | `job_status_request` (the 64-bit ID) |
| JOB_STATUS_RESPONSE | `job_status_response` |
| JOB_STATUS_NOT_FOUND | `job_status_not_found` (the echoed 64-bit ID) |

The union shares storage between alternatives instead of holding all message bodies
at once. Initialize the correct member, and inspect the type before reading it.
Argument/result arrays belong to the message. The decoder copies bytes into them,
so reusing the receive buffer does not invalidate a decoded message. Only the
first `argument_size`/`result_size` bytes are meaningful. These host lengths use
`size_t`, but their wire fields are explicitly 4-byte integers.

```c
struct faultline_message started = {
    .message_type = FAULTLINE_MSG_JOB_STARTED,
    .payload.job_started = {.job_id = 42, .worker_id = 7, .attempt = 1}
};
uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
size_t written = 0;
enum faultline_protocol_result status =
    faultline_message_encode(wire, sizeof(wire), &started, &written);
/* On OK, written == 32. Sending those bytes is a separate operation. */
```

The same complete-message encode/decode API handles lifecycle and job messages:

- Encoding validates type, fields, byte counts, and output capacity before
  writing. Sizes above 1024, including `SIZE_MAX`, are rejected before narrowing
  to 32 bits, adding the prefix, or copying data.
- Decoding validates the header and the message's outer size bounds before
  waiting for a payload. Fixed-size mismatches return INVALID_PAYLOAD_LENGTH;
  variable payloads above their maximum return PAYLOAD_TOO_LARGE.
- Once the fixed prefix is available, it validates task/identity/failure fields
  and the inner byte count. The outer payload length must equal prefix + inner
  length exactly. Extra declared bytes cannot be treated as optional fields.
- A valid incomplete frame returns BUFFER_TOO_SMALL. The caller retains those
  bytes and retries after appending more. Detectably invalid prefixes are rejected
  immediately, without waiting for the claimed variable data.
- Only a fully valid frame is copied to the output. Errors leave the entire
  destination and written/consumed counts unchanged. One successful decode
  consumes exactly one frame, preserving following messages for the caller.

New errors distinguish INVALID_JOB_ID, INVALID_ATTEMPT, INVALID_TASK_TYPE, and
INVALID_FAILURE. INVALID_WORKER_ID also applies to the common job identity.
The functions require valid, non-null, non-overlapping caller-owned storage,
perform no socket I/O, and allocate no heap memory.

## Verification

`tests/test_job_messages.c` adds eight groups covering independent literal frames
for all six types; unaligned buffers; every prefix/short capacity of the example
frames and the largest assignment; 0/1/255/256/1024-byte data, embedded zeroes, and
receive-buffer reuse; all task IDs; maximum IDs/retry values and 64-bit attempts;
invalid identities/tasks/failures; outer/inner length mismatches and overflow;
mixed heartbeat/job streams; and a decoded old attempt rejected by the model.
Every rejected codec call is checked for unchanged outputs. Existing lifecycle
vectors still verify their original bytes and null-pointer contracts.

TCP integration tests cover CLI acceptance, live FIFO dispatch, fragment handling,
worker identity, validated reports, and retries. Real workers execute tasks;
controlled TCP peers also send start/result/failure reports to test the handlers.
Wrong-direction job headers remain rejected before collecting payloads.
Run `make test-unit`, `make SANITIZE=1 test-unit`, and `make test-integration`.
