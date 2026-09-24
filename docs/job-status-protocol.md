# Job-status messages and payloads

The shared codec defines how a client asks about one job and how the coordinator
represents its answer. Encoding, decoding, and validation are implemented and
tested. Coordinator lookup handlers and a CLI `status` command are the next step;
the current coordinator still rejects these message types from incoming headers.

These messages use the existing [12-byte version 1 header](protocol.md). All
integers are unsigned and big-endian. The encoder writes individual fields;
C struct padding, enum storage, pointers, and host byte order are never sent.

## Message types

| ID | Message | Direction | Payload bytes | Whole frame bytes |
| --- | --- | --- | --- | --- |
| 12 | `JOB_STATUS_REQUEST` | Client → coordinator | 8 | 20 |
| 13 | `JOB_STATUS_RESPONSE` | Coordinator → client | 36 + result length | 48–1072 |
| 14 | `JOB_STATUS_NOT_FOUND` | Coordinator → client | 8 | 20 |

IDs 1–11 and their wire layouts are unchanged. This is an additive version 1
extension; older binaries reject unknown types. There is no capability or
version negotiation. Query-capable peers will need matching implementations.

## Request and unknown ID

Both REQUEST and NOT_FOUND carry a single nonzero, 8-byte `job_id` at payload
offset 0. The request asks for that job's current snapshot. The reply echoes
the exact requested ID, allowing the client to check which job the answer names.
The initial query flow will use one outstanding request per connection; there
is no separate request identifier or subscription stream.

NOT_FOUND means the coordinator's current job store has no record for that ID.
It contains no state, worker, retry counters, result, or failure reason. It does
not claim the job failed or prove a previous submission was never accepted.
A known terminal FAILED job gets a normal RESPONSE with state FAILED.
Job ID zero is malformed and rejected by the codec, rather than answered with
NOT_FOUND. Querying an absent ID must not create or reserve a job.

A complete request for job 42 is:

```text
46 4c 49 4e | 00 01 | 00 0c | 00 00 00 08
      FLIN |    v1 | type 12 | payload length 8
00 00 00 00 00 00 00 2a
                    42
```

If job 42 is absent, the reply has the same payload and length, with message
type `00 0e` (14). No text such as "not found" is carried on the wire.

## Status response layout

Offsets are relative to the payload, after the 12-byte header.

| Offset | Width in bytes | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `job_id` | Nonzero ID matching the request. |
| 8 | 2 | `state` | QUEUED=1, ASSIGNED=2, RUNNING=3, DONE=4, FAILED=5. |
| 10 | 4 | `worker_id` | Current owner, or last owner for a terminal job; zero for QUEUED. |
| 14 | 8 | `attempt` | Latest assignment number; zero before the first assignment. |
| 22 | 4 | `retry_count` | Retry allowances consumed when returning a failed attempt to QUEUED. |
| 26 | 4 | `max_retries` | Allowed retries after the initial attempt. |
| 30 | 2 | `failure` | NONE=0, TASK=1, or WORKER_LOST=2, subject to the state rules below. |
| 32 | 4 | `result_length` | Number of result bytes, 0–1024. |
| 36 | `result_length` | Result bytes | Opaque binary data, present only for DONE. |

The payload length must equal `36 + result_length` exactly. The shared
`FAULTLINE_MESSAGE_MAX_FRAME_SIZE` is now 1072 bytes: header 12 + prefix 36 +
result 1024. Transport arrays use that constant. The maximum assignment is
still 1062 bytes. The generic header's 1 MiB ceiling is unchanged; complete
message validation enforces these smaller limits.

An empty DONE result is valid. Results are not necessarily text, can include
zero bytes, and have no implicit NUL terminator. The decoder copies them into
the message's own array so later receive-buffer reuse cannot change the result.
This response does not carry task type or arguments; a future generic CLI can
display the raw result bytes without assuming their task-specific meaning.

## State and counter consistency

These rules match the existing [job model](jobs.md), including worker-loss and
[startup recovery](persistence.md#startup-and-interruption-accounting). Let `r` mean `retry_count`.
Every snapshot requires `r <= max_retries`.

| State | Worker ID | Attempt | Failure | Result |
| --- | --- | --- | --- | --- |
| Fresh QUEUED | 0 | 0, with r=0 | NONE | Empty |
| Retried QUEUED | 0 | r, with r>0 | TASK or WORKER_LOST | Empty |
| ASSIGNED | Nonzero current owner | r+1 | NONE | Empty |
| RUNNING | Nonzero current owner | r+1 | NONE | Empty |
| DONE | Nonzero last owner | r+1 | NONE | 0–1024 bytes |
| FAILED | Nonzero last owner | r+1, with r=max_retries | TASK or WORKER_LOST | Empty |

Assignment numbers count assignments, not confirmed execution starts. A queued
retry retains the previous attempt number and its failure reason. Its retry
allowance is already consumed even if the next assignment has not happened yet.
QUEUED with `r == max_retries` can therefore still have its final attempt waiting
to run. Assignment clears the previous failure reason. Terminal worker IDs are
history; they do not assert that the worker is still connected or alive.

The codec widens the retry count to 64 bits before adding one: a retry limit of
`UINT32_MAX` allows final attempt `2^32`, which does not fit in 32 bits.
Unknown states and failure codes, contradictory owners/counters, terminal failure
with allowance remaining, and results on a non-DONE state are rejected.

WORKER_LOST can appear here because the coordinator is reporting its own
decision. It remains invalid in a worker's [JOB_FAILED report](job-protocol.md#started-completed-and-failed-reports).
The status failure is a numeric reason, without free-form error text.

For a job with two retries allowed, successive queries might return:

```text
job 42: QUEUED, worker 0, attempt 0, retries 0/2, failure NONE
job 42: RUNNING, worker 7, attempt 1, retries 0/2, failure NONE
job 42: QUEUED, worker 0, attempt 1, retries 1/2, failure WORKER_LOST
job 42: RUNNING, worker 8, attempt 2, retries 1/2, failure NONE
job 42: DONE, worker 8, attempt 2, retries 1/2, failure NONE, result bytes
```

## Codec and handler boundaries

The host representation is `struct faultline_job_status_payload`. The message
union uses `job_status_request`, `job_status_response`, or `job_status_not_found`.
The existing `faultline_message_encode()` and `faultline_message_decode()` APIs
handle all three. New errors INVALID_JOB_STATE and INVALID_RETRY_COUNT identify
status inconsistencies; existing identity, attempt, failure, and length errors
apply as well. These errors are local return values, not wire error responses.

Invalid outer lengths are rejected from the header. With the fixed prefix
available, fields and the inner result length are checked before waiting for
result data. A valid incomplete frame returns BUFFER_TOO_SMALL. Any failure
leaves output buffers/messages and written/consumed counts unchanged. Successful
decoding consumes exactly one frame, leaving following bytes for the caller.

The codec cannot verify that the ID exists, that the snapshot is current, or
that a reply matches an outstanding request. The later coordinator handler must
look up the authoritative job and copy one consistent snapshot; the client must
check the echoed ID. Querying must be read-only: no assignment, retry, heartbeat
renewal, ID allocation, or WAL append. Existing connection-role checks still need
explicit handler integration before requests can be served.

A response describes one instant and may be out of date when received. It grants
no execution lease and does not change the [durability contract](durability.md).
Polling does not provide submission deduplication, cancellation, automatic result
pushes, or an exactly-once execution guarantee.

## Verification

```sh
make test-job-status-protocol
make SANITIZE=1 test-job-status-protocol
```

Seven groups in `tests/test_job_status.c` cover independent literal byte vectors
for requests, not-found replies, and every state; real model transitions through
retries and exhaustion; integer boundaries and owned binary results; every
incomplete prefix/short capacity up to the 1072-byte maximum; inconsistent fields;
outer/inner lengths and null pointers; and mixed streams with a partial last
frame. Error cases check unchanged outputs. Exact-sized input allocations let
AddressSanitizer detect reads beyond the available bytes.

Existing protocol vectors continue to test the original wire layouts. Process
tests also check that the current coordinator rejects all three new types from
the header until query handlers are implemented.
