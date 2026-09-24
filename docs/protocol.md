# Faultline wire protocol

The protocol header defines how a receiver identifies a Faultline message and
determines the number of payload bytes that follow it. Header encoding,
validation, complete-message encoding/decoding, and a TCP exchange for
empty-payload PING/PONG are implemented. Worker registration, its acknowledgment,
and heartbeat formats are defined and tested, including their payload bytes.
The coordinator accepts registrations, returns assigned IDs, and records valid
heartbeats. The worker executable sends WORKER_REGISTER, validates the ACK header
and ID payload, and uses its assigned ID in periodic HEARTBEAT messages. Timing
configuration and expiration policy are described in [workers.md](workers.md);
they add no fields to the wire format. Integration tests also use independent TCP peers.
Job submission, acknowledgment, assignment, started, completed, and failed
messages are also defined and tested in the shared codec. CLI submission,
coordinator scheduling/report handlers, and worker assignment reception now use
them; see the [job message specification](job-protocol.md) and
[scheduling guide](scheduling.md). Workers execute the [built-in tasks](tasks.md)
and send actual start, result, or failure reports.
The [job-status codec](job-status-protocol.md) also defines requests, snapshots,
and explicit not-found replies; coordinator query handlers and CLI output are pending.

## Header layout

Every frame starts with exactly 12 bytes. Offsets are zero-based. All integer
fields use unsigned, big-endian encoding, also called network byte order.

| Offset | Size | Field | Accepted value |
| --- | --- | --- | --- |
| 0 | 4 bytes | Magic | `0x464c494e`, the ASCII bytes `FLIN` |
| 4 | 2 bytes | Version | `1` |
| 6 | 2 bytes | Message type | IDs `1` through `14`, listed below |
| 8 | 4 bytes | Payload length | `0` through `1,048,576` bytes inclusive |

```text
byte offset   0            4      6      8           12
              +------------+------+------+------------+-----------------+
              |   magic    | ver  | type | length     | payload ...     |
              +------------+------+------+------------+-----------------+
field size         4          2      2        4          length bytes
```

The payload length excludes the 12-byte header. A zero-length payload is valid
at the header layer; its whole frame is 12 bytes. The maximum frame allowed by
the header's length limit is 1,048,588 bytes: a 1 MiB payload plus the header.
Current message formats impose much smaller fixed lengths or bounded variable lengths. The 1 MiB limit is an initial
project policy that bounds individual messages even though the length field can
represent much larger values. It does not bound total buffered data across
connections; that is a later transport concern.

The magic identifies the expected protocol; it is not a checksum or
authentication mechanism. The version selects the interpretation of the
protocol and is independent of the application's eventual `v0.1.0` release tag.
An unsupported version is rejected; version negotiation is not implemented.

Zero and IDs outside the table below are rejected. Existing IDs must
not be renumbered. Enum storage layout is never used as the wire representation.

## Messages and payloads

| ID | Message | Direction | Payload | Whole frame |
| --- | --- | --- | --- | --- |
| 1 | `PING` | CLI to coordinator | Empty: 0 bytes | 12 bytes |
| 2 | `PONG` | Coordinator to CLI | Empty: 0 bytes | 12 bytes |
| 3 | `WORKER_REGISTER` | Worker to coordinator | Empty: 0 bytes | 12 bytes |
| 4 | `WORKER_REGISTER_ACK` | Coordinator to worker | `worker_id`: 4-byte unsigned big-endian integer | 16 bytes |
| 5 | `HEARTBEAT` | Worker to coordinator | `worker_id`: 4-byte unsigned big-endian integer | 16 bytes |
| 6 | `JOB_SUBMIT` | CLI to coordinator | Task, retry limit, argument length and bytes | 22–1046 bytes |
| 7 | `JOB_SUBMIT_ACK` | Coordinator to CLI | Nonzero 8-byte job ID | 20 bytes |
| 8 | `JOB_ASSIGN` | Coordinator to worker | Job/worker/attempt identity, task, arguments | 38–1062 bytes |
| 9 | `JOB_STARTED` | Worker to coordinator | Job/worker/attempt identity | 32 bytes |
| 10 | `JOB_COMPLETED` | Worker to coordinator | Identity, result length and bytes | 36–1060 bytes |
| 11 | `JOB_FAILED` | Worker to coordinator | Identity and TASK failure code | 34 bytes |
| 12 | `JOB_STATUS_REQUEST` | Client to coordinator | Nonzero 8-byte job ID | 20 bytes |
| 13 | `JOB_STATUS_RESPONSE` | Coordinator to client | Job ID, state, worker, attempt, retries, failure, result length and bytes | 48–1072 bytes |
| 14 | `JOB_STATUS_NOT_FOUND` | Coordinator to client | Echoed nonzero 8-byte job ID | 20 bytes |

See [job-protocol.md](job-protocol.md) for every job field's byte offset, acceptance
and report semantics, retry identity, and validation rules. See
[job-status-protocol.md](job-status-protocol.md) for query layouts and state
consistency. Types 12–14 currently have codecs only; incoming query types remain
rejected by the coordinator's handler whitelist. The following worker
lifecycle discussion describes the currently active TCP handlers.

`WORKER_REGISTER` requests an identity. It does not propose an ID or carry
metadata: MVP workers all execute one job at a time, so capacity negotiation is
not needed for this first format. `WORKER_REGISTER_ACK` represents successful
registration and carries the coordinator-issued ID. There is no rejection
payload or separate registration-error message in this step.

Both worker ID payloads have one field at payload offset 0 (frame offset 12):
`worker_id`, exactly 4 bytes, with accepted values `1` through `4,294,967,295`.
Zero is reserved for an unregistered worker and is invalid in an ACK or
heartbeat. Worker IDs identify registrations; they are not socket descriptors,
process IDs, or permanent machine identities.

The intended registration sequence is:

```text
Worker                             Coordinator
  | ------ WORKER_REGISTER ----------> |
  | <----- WORKER_REGISTER_ACK(12) ---- |
  | ------ HEARTBEAT(12) -------------> |
  | ------ HEARTBEAT(12) -------------> |
```

The worker must wait for the ACK before sending heartbeats, then use the
assigned ID on that connection. The coordinator assigns distinct,
nonzero IDs, associates each with its registered connection, and checks heartbeat
IDs against that association. Reconnection requires registration again; this
format makes no promise of preserving IDs across connections or coordinator
restarts. `HEARTBEAT` has no reply and carries no sender timestamp. The registry
uses the coordinator's own monotonic receive time, avoiding cross-machine clock
comparisons. Registration supplies the initial liveness timestamp; only complete,
valid heartbeats update it afterward. Workers send every two seconds and the
coordinator expires a registration after six seconds without a valid heartbeat
by default. Both durations are configurable locally; scheduling comes later.

The coordinator and its [worker registry](workers.md) enforce connection ownership
and reject duplicate registration on an already registered connection. The codec
itself only validates byte structure and ID range. Disconnects and timeouts mark a registered
worker dead and clear its descriptor. A heartbeat cannot revive an old registration.

The header functions only check that a type is recognized and a length is
bounded. The complete-message functions additionally enforce each fixed length
or variable-length formula in the table. For example, a header declaring a 3-byte ACK is rejected at the
message layer immediately; an ACK declaring 4 bytes with only 3 received is
incomplete and needs more input.

The coordinator accepts empty PING and WORKER_REGISTER messages, and HEARTBEAT
with the sending connection's assigned ID. PING is also allowed on registered
connections for diagnostics, but does not update heartbeat time. PONG and
WORKER_REGISTER_ACK are replies and are rejected as incoming requests. Wrong
lengths, malformed frames, duplicate registration, and invalid heartbeat ownership
close the offending connection without a protocol error response. The CLI still
expects one empty PONG for `ping`, or a JOB_SUBMIT_ACK for `submit`. Nonzero PING/PONG lengths in header tests
exercise generic length encoding, not valid complete messages.

The coordinator also accepts JOB_SUBMIT from unregistered clients and STARTED,
COMPLETED, and FAILED from registered workers. Job reports must match the
connection, job owner, attempt, and allowed state transition. Job ACKs and
assignments are outbound-only at the coordinator. A connection that has submitted
a job cannot switch to being a registered worker. See the scheduling guide for
acceptance, capacity, report rejection, and retry behavior.

## Byte order and examples

Big-endian encoding puts the most significant byte first. The numeric value
`0x00010203` is sent as `00 01 02 03`, regardless of the sender's CPU byte order.
The four bytes are binary values, not the text characters `"00010203"`.

A version 1 PING header with an empty payload is:

```text
46 4c 49 4e | 00 01 | 00 01 | 00 00 00 00
   FLIN    |   v1  |  PING |    0 bytes
```

An acknowledgment assigning worker ID 12 is a complete 16-byte frame:

```text
46 4c 49 4e | 00 01 | 00 04 | 00 00 00 04 | 00 00 00 0c
   FLIN    |   v1  |  ACK  |    4 bytes  | worker ID 12
```

Its heartbeat has the same payload and length, with message type `00 05`.
The length field is `4`, not `16`: it counts only the payload. The ID's `0c` is
hexadecimal for decimal 12. Neither the ID nor its digits are sent as text.

## C API

`include/protocol.h` declares constants, message IDs, error results, the
`struct faultline_header` host representation, and two header functions:

```c
enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header);

enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header);
```

For example, prepare a PING header for transmission:

```c
struct faultline_header header = {
    .magic = FAULTLINE_PROTOCOL_MAGIC,
    .version = FAULTLINE_PROTOCOL_VERSION,
    .message_type = FAULTLINE_MSG_PING,
    .payload_length = 0
};
uint8_t wire[FAULTLINE_HEADER_SIZE];
enum faultline_protocol_result result =
    faultline_header_encode(wire, sizeof(wire), &header);
/* Only use wire for transmission if result == FAULTLINE_PROTOCOL_OK. */
```

Struct fields are ordinary host-order numbers. Callers must not apply `htonl()`
or `htons()` themselves before encoding: the encoder already writes network
order. Likewise, decoded fields need no further byte-order conversion.

Never transmit `sizeof(struct faultline_header)` bytes from the struct. Its
padding, alignment, and memory byte order are separate from the protocol.
`src/common/protocol.c` explicitly reads and writes individual bytes at the
specified offsets using unsigned shifts. It makes no typed pointer casts into
the wire buffer and does not require that buffer to be aligned for integers.

### Complete-message API

`struct faultline_message` holds the host-order message type and a tagged union
named `payload`. Its `worker_id` member must be zero for empty messages and
nonzero for worker ACK and HEARTBEAT. Job types select their own union members;
see [the job representation](job-protocol.md#c-representation-and-validation).
The complete-message encoder derives magic, version, and payload length; callers
do not fill those fields or encode the header separately.

```c
enum faultline_protocol_result faultline_message_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_message *message,
    size_t *written);

enum faultline_protocol_result faultline_message_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_message *message,
    size_t *consumed);
```

For example, encode an acknowledgment with an already-assigned ID:

```c
struct faultline_message ack = {
    .message_type = FAULTLINE_MSG_WORKER_REGISTER_ACK,
    .payload.worker_id = 12
};
uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE];
size_t written = 0;
enum faultline_protocol_result result =
    faultline_message_encode(wire, sizeof(wire), &ack, &written);
/* On OK, wire contains exactly the example above and written == 16. */
```

These functions work on caller-owned byte buffers. They perform no socket I/O,
allocate no heap memory, and do not assign IDs. Variable argument/result bytes
are copied into the message's own bounded arrays on decode; the caller can then
reuse the receive buffer. Only the member selected by the type should be read.

## Validation and buffer ownership

All four functions require non-null pointers to valid, non-overlapping storage.
The caller owns all buffers; none of the functions allocate memory or perform I/O.
The encoder's size is writable capacity; the decoder's size is the number of
input bytes actually available.

Header validation proceeds in the following order and returns the first error:

| Result | Meaning |
| --- | --- |
| `FAULTLINE_PROTOCOL_INVALID_ARGUMENT` | A required pointer is null. |
| `FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL` | Fewer than 12 input bytes or output bytes of capacity. |
| `FAULTLINE_PROTOCOL_BAD_MAGIC` | Magic does not match `FLIN`. |
| `FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION` | Version is not 1. |
| `FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE` | Type is not one of the fourteen defined message IDs. |
| `FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE` | Declared payload exceeds 1 MiB. |
| `FAULTLINE_PROTOCOL_OK` | Header was successfully encoded or decoded. |

On any error the output stays unchanged. Encoding validates before writing any
bytes. Decoding uses a temporary header and copies it to the caller only after
validation succeeds.

On success, encoding writes exactly 12 bytes and leaves any extra capacity
untouched. Decoding reads only the first 12 bytes and ignores trailing input.
It does not consume a stream buffer or report payload completion.

The complete-message functions have these additional errors:

| Result | Meaning |
| --- | --- |
| `FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH` | The declared length differs from the required fixed length or prefix + data length, or a non-DONE status carries result bytes. |
| `FAULTLINE_PROTOCOL_INVALID_WORKER_ID` | Worker ACK/HEARTBEAT or job identity has ID zero, an empty message supplies a nonzero ID, or a status owner contradicts its state. |
| `FAULTLINE_PROTOCOL_INVALID_JOB_ID` | A job ACK, job identity, or status request/reply has job ID zero. |
| `FAULTLINE_PROTOCOL_INVALID_ATTEMPT` | Assignment/report attempt is zero, or a status attempt contradicts its state/retry count. |
| `FAULTLINE_PROTOCOL_INVALID_TASK_TYPE` | Submission/assignment task type is unknown. |
| `FAULTLINE_PROTOCOL_INVALID_FAILURE` | A worker failure report contains a code other than TASK=1, or a status failure contradicts its state. |
| `FAULTLINE_PROTOCOL_INVALID_JOB_STATE` | A status state is outside QUEUED through FAILED. |
| `FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT` | Status retries exceed the limit, or FAILED has not exhausted it. |

Message encoding checks pointers, type, payload fields, byte counts, and output
capacity before writing. It returns `BUFFER_TOO_SMALL` if the entire frame will
not fit, without writing even a partial header. Message decoding checks the
header and outer bounds, waits for the fixed prefix, validates its fields and
inner length, then waits for any remaining variable data. Job argument/result
sizes above 1024 also return `PAYLOAD_TOO_LARGE`, even below the header's 1 MiB
ceiling when the other fields are valid. See the job and status specifications
for exact validation behavior.

On any failure, message outputs and the `written`/`consumed` counters stay
unchanged. On success, encoding reports the full frame size and leaves extra
capacity untouched. Decoding reports exactly one frame's size in `consumed`,
ignores following bytes, and sets `payload.worker_id` to zero for empty messages.
The caller advances its own buffer only after a successful decode. A partial
valid header or payload returns `BUFFER_TOO_SMALL`; a declaration already known
to be invalid is rejected without waiting for more bytes.

## Relationship to TCP framing

TCP provides a stream of bytes, not one complete message per `recv()` call.
The general receiving flow is:

1. Accumulate at least 12 bytes without discarding earlier partial input.
2. Decode the header and reject invalid field values.
3. Accumulate exactly the declared payload length before dispatching the message.
4. Retain any bytes belonging to following frames for subsequent parsing.

A short header is incomplete input, not necessarily a malformed message. The
current coordinator keeps partial headers per connection, and the CLI uses
`faultline_recv_exact()` to collect a response. Both detect EOF during a header.
The coordinator first reads only the bytes remaining in one 12-byte header.
For a supported payload, it collects the declared bytes in its bounded 1072-byte
buffer before dispatching. Following frames stay in the socket's receive buffer
until the coordinator is ready for them. Invalid outer lengths and disallowed
message directions are rejected from the header. The worker uses the same maximum
frame capacity for assignments while maintaining a separate heartbeat deadline.

The header encoding/decoding functions remain independent of transport: they
do not keep partial-read state, retry sends, or close connections. See
[the networking walkthrough](networking.md) for that implementation.

The complete-message decoder is also stateless. If only 14 bytes of an ACK
are buffered, its header is complete but two ID bytes are missing. It returns
`BUFFER_TOO_SMALL`; the caller keeps those 14 bytes, appends the remaining two,
and tries again. If two ACK frames are buffered together, a successful decode
reports `consumed == 16`, leaving the next 16 bytes for a second call.
The coordinator now uses this decoder for incoming frames. Tests exercise both
its buffer behavior and the live registration/heartbeat receive path.

## Verification

Run `make test` for the normal build and `make test-sanitize` for AddressSanitizer
and UndefinedBehaviorSanitizer. The seven header test groups cover literal wire vectors,
payload boundaries, all header truncation lengths, invalid fields, invalid
encoder inputs, null arguments, and unaligned buffers with trailing bytes.

Literal expected byte sequences verify conformance independently of round-trip
tests. Failure cases also check that outputs remain unchanged. Socket unit tests
and process integration tests additionally exercise partial transfers,
timeouts, concurrent clients, malformed messages, and disconnected peers.

Seven additional complete-message test groups cover literal frames for all five
lifecycle types, worker ID boundaries, every incomplete frame prefix (including payload
truncation), short output buffers, wrong lengths, invalid IDs/headers, null
arguments, unaligned buffers, untouched outputs, and consecutive complete frames
followed by a heartbeat completed in fragments.

Eight job-message test groups additionally cover all six job formats, bounded
variable data, attempt identity, and mixed streams; see [their coverage](job-protocol.md#verification).
Seven status-message groups cover request/not-found frames, every job state,
model transitions, counter consistency, results, and partial frames; see
[their coverage](job-status-protocol.md#verification).
