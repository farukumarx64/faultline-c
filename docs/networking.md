# The first TCP exchange

For a first coordinator launch, explicitly create its WAL with `--init-wal`.
Later launches recover the same file; see [persistence startup](persistence.md#starting-and-restarting).

Faultline can now start a coordinator on `127.0.0.1:9000`, accept a CLI connection,
receive a binary PING header, and send a binary PONG header. The coordinator also
accepts registration and heartbeat frames and owns a [worker registry](workers.md).
The worker executable connects, registers, and sends periodic heartbeats through
the same protocol. The coordinator expires silent workers.
[CLI submission and FIFO scheduling](scheduling.md) now extend this transport to
job payloads; the explanation below covers both lifecycle and job traffic.

## Run it

Build with `make`, then use two terminals:

```sh
# Terminal 1
./build/debug/faultline-coordinator --port 9000

# Terminal 2
./build/debug/faultline ping --coordinator 127.0.0.1:9000
```

The CLI prints `PONG` and exits successfully only after validating the complete
response. Errors go to stderr and produce a nonzero exit status. Both programs
use `127.0.0.1:9000` by default; the address options above are optional.

The coordinator logs listening, connection, PING/PONG, registration, heartbeat,
and worker-death events. The `fd`
field is the operating system's connection descriptor, which can be reused
after closing a connection. It is not a worker ID. Ctrl+C or SIGTERM requests
shutdown, closes all client sockets and the listener, and exits the event loop.

## Files and responsibilities

| File | Responsibility |
| --- | --- |
| `include/net.h` | Networking API, defaults, and receive result codes. |
| `src/common/net.c` | Socket setup, endpoint/port parsing, readiness waits, deadlines, and complete-transfer helpers. |
| `src/coordinator/main.c` | Listener, client slots, poll loop, frame handling, queued replies, and registry integration. |
| `include/worker_registry.h`, `src/coordinator/worker_registry.c` | Coordinator-owned IDs, connections, liveness states, and heartbeat timestamps. |
| `src/cli/main.c` | CLI arguments, one connection, request transmission, and response validation. |
| `src/worker/main.c` | Worker arguments, registration request/ACK, heartbeat timer, and shutdown. |

`protocol.h` and `protocol.c` own the 12-byte header and complete-message formats.
Networking uses their encoders and decoders; it does not send raw C structs.

## Connection establishment

The coordinator creates an IPv4 TCP socket, enables `SO_REUSEADDR`, marks the
socket nonblocking, binds it to loopback and the selected port, and calls
`listen()`. Reuse-address helps with restarting after old TCP connections; it
does not permit a second live coordinator to claim the same listening address.

`accept()` returns a different socket for each established client connection.
The listening socket remains responsible for accepting new connections. Each
accepted socket is explicitly made nonblocking and placed in a client slot.

The CLI and worker use `faultline_parse_endpoint()` for their optional numeric
IPv4:PORT argument. It validates the address and port and leaves both outputs
unchanged on error. Names such as `localhost` and IPv6 addresses are not accepted.

Each creates its own nonblocking socket through `faultline_connect()`. Connection
establishment may still be in progress, reported as `EINPROGRESS`. It waits for
writability with `poll()` and checks `SO_ERROR` before treating the connection
as successful. Writability alone does not establish that connection succeeded.

The socket address's port uses `htons()`. That conversion belongs to the POSIX
socket-address API; the protocol header encoder separately handles the header's
byte order. Numeric IPv4 addresses are parsed with `inet_pton()`.

## Worker registration and waiting

The worker sends an empty WORKER_REGISTER using the complete-message encoder
and `faultline_send_all()`. It then collects the ACK in a fixed 16-byte buffer:
first 12 bytes for the header, followed by four bytes for the worker ID. A wrong
header, message type, or payload length is rejected before waiting for payload
bytes. The complete-message decoder validates the final frame, including the
nonzero ID. Only then does the worker print `worker registered worker_id=...`.

The ACK receive loop keeps its byte count across partial reads. It uses `poll()`
with a maximum 250 ms wait so SIGINT/SIGTERM can stop it promptly, including a
signal arriving just before a wait. The handler only sets a `sig_atomic_t` flag;
ordinary code performs cleanup. This loop has one five-second deadline covering
both header and payload, even if bytes continue arriving slowly.

After registration, the worker retains the socket and ID and waits for a stop
request, coordinator activity, or the next heartbeat deadline. It encodes a
16-byte HEARTBEAT once and sends that frame at the configured interval, using
`faultline_send_all()` to preserve partial-write handling. The first deadline is
one interval after the complete ACK. Later deadlines start at local send completion,
so a delayed worker sends no catch-up burst. A `heartbeat_sent` log records local
send completion; only the coordinator's `heartbeat_received` log proves processing.
EOF, reset, a send failure, or invalid incoming frames produce
an error and a failure exit. Valid JOB_ASSIGN frames are accumulated in a
9744-byte buffer, validated against the registered worker ID, and retained.
A worker executes one assignment in a task thread while its main thread sends
heartbeats and reports results; see [tasks.md](tasks.md). A partial assignment has one five-second receive deadline
from its first byte, independent of the heartbeat deadline.
The worker reads exactly the ACK size so additional bytes cannot be silently
swallowed by the registration receive loop. Local SIGINT/SIGTERM requests close
the socket and produce a successful exit. There is no automatic reconnect loop.

## The coordinator event loop

The single-threaded coordinator uses `poll()` to wait for activity on the
listener and up to 64 clients. A client has two phases:

```text
READING_MESSAGE -> WRITING_REPLY -> READING_MESSAGE -> ...
READING_MESSAGE -> record heartbeat -> READING_MESSAGE -> ...
```

Each client owns 9744-byte input and output buffers, received/sent byte counters,
the expected input size, the actual output size, its phase, and a monotonic
timestamp of its last byte-transfer progress. It also stores its assigned
worker ID, or zero if the connection has not registered.

In the read phase, the loop requests `POLLIN`. A `recv()` call asks for only the
bytes still needed for the header. Positive results advance the receive counter.
Once 12 bytes are present, the complete-message decoder either returns an empty
message, rejects the frame, or reports that a valid payload is incomplete. For
a payload-bearing frame the coordinator raises the expected size to header +
payload length and reads only those remaining bytes before decoding again.
This still gives 16 bytes for a heartbeat; job payloads can be larger.

PING queues PONG. WORKER_REGISTER adds an ALIVE registry entry with a fresh ID
and queues an ACK carrying that ID. A valid HEARTBEAT updates only the sending
worker's registry timestamp and resets the input state without queuing a reply.
Duplicate registration and incorrect heartbeat IDs close the offending connection.
JOB_SUBMIT stores and enqueues a job before queuing its ACK. STARTED, COMPLETED,
and FAILED update the matching active job through the scheduler. A malformed or
unauthorized report closes its sender's connection and triggers normal loss cleanup.

In the write phase, the loop requests `POLLOUT`. A `send()` call uses the remaining
part of the reply buffer and advances the sent counter by the actual result.
After the actual frame length (12 for PONG, 16 for registration ACK, or the
encoded size for job ACKs/assignments) has been
accepted by the local socket, the client returns to reading a new header.
The `pong_sent` and `worker_register_ack_sent` logs record local send completion,
not proof that the remote application received the response.

The loop processes one read or write per ready client per iteration. It accepts
at most 64 connections per iteration and closes excess connections when all
slots are occupied. A client with a partial header therefore cannot hold the
loop waiting for its next byte. The coordinator does not call the CLI's
complete-transfer helpers inside this shared event loop because those helpers
wait for one connection's whole operation.

If several PINGs arrive together, the coordinator reads exactly one header,
responds, and then reads the next. Later bytes remain in the kernel's receive
buffer. The same rule preserves frame boundaries for registration and heartbeat
and job payloads. After processing events, the scheduler can queue an assignment
for an eligible idle worker. It skips connections with pending output or partial
input, and a queued assignment reserves the worker before any send occurs.

## Partial I/O and errors

TCP delivers an ordered byte stream. A `recv()` may return fewer bytes than
requested, and a `send()` may accept only part of the supplied buffer. Neither
operation defines application-message boundaries.

The CLI uses `faultline_send_all()` and `faultline_recv_exact()` to repeat those
operations while retaining the offset already transferred. Both require
nonblocking sockets and use `poll()` between attempts. `EINTR` means a signal
interrupted an operation; `EAGAIN` or `EWOULDBLOCK` means it would currently block.
These conditions are retried without losing progress.

Receive results distinguish a complete read, clean EOF before any bytes,
truncation after some bytes, and a system error. Receiving zero bytes from a
nonzero-length `recv()` request means EOF. It does not mean "try again later."

A `POLLHUP` event may accompany buffered input. The coordinator still attempts
the relevant I/O, so a client that sends PING and then shuts down its write half
can receive PONG. A reset or broken connection closes that client alone.

All three executables ignore SIGPIPE so a send to a closed connection produces an
error that code can handle, rather than terminating the process. This does not
hide the send error. Socket setup failures close any newly created descriptor,
and the CLI closes its connection on both success and failure.

## Deadlines

The CLI allows five seconds each for connecting, sending the whole header, and
receiving the whole response. Each operation keeps one deadline across retries
and partial progress. These are separate operation budgets, not a five-second
budget for the entire invocation.

The worker similarly allows five seconds to connect, five seconds to send the
registration, and one five-second deadline for the whole ACK. Its ACK receive
loop and heartbeat wait are interruptible; the shared connect/send helpers may finish
their current bounded operation before honoring a local stop request.

The coordinator applies a five-second inactivity limit to unregistered clients,
partial incoming messages, and queued replies, measured since acceptance or the
last successful read/write. The poll loop checks this approximately once per
second when idle, so a stalled operation usually closes within five to six
seconds. Successful byte transfers refresh this I/O timer.

Registered workers waiting between complete messages are exempt from this I/O
timer. They have a separate heartbeat deadline, initially measured from registration
and subsequently from the latest complete, valid heartbeat. The default timeout
is 6000 ms, configurable with `--heartbeat-timeout-ms`. PINGs and partial frames
cannot renew it, even while successful transfers refresh the separate I/O timer.

The coordinator caps each `poll()` wait at the nearest worker deadline as well
as its usual one-second ceiling, allowing subsecond timeout settings. After
waking, it reads the monotonic clock again and expires workers before processing
socket events. A complete heartbeat must be processed before expiry; even buffered
bytes cannot revive an expired ID. Scheduling delays can postpone the check, so
this is not a hard real-time guarantee. An expired client follows the same cleanup
path as a disconnect: mark DEAD, clear its descriptor, then close its socket.
Its death log uses `reason=heartbeat_timeout`.
The preceding `heartbeat_timeout` event includes `timeout_ms`, `detected_at_ms`,
and `silence_ms`. Detection time is the coordinator's monotonic clock value;
silence is that value minus the worker's retained `last_heartbeat_ms`. These
fields make the timeout decision inspectable without comparing clocks across processes.

All elapsed-time calculations use `CLOCK_MONOTONIC`, which avoids wall-clock
adjustments affecting timeouts.

## Current boundary and tests

PING, PONG, and WORKER_REGISTER require empty payloads. Registration ACK and
HEARTBEAT require exactly four ID bytes. Incorrect types, malformed frames,
wrong lengths, and invalid registration/heartbeat state close the affected
connection without an error frame. Every registered-client close path marks its
worker DEAD and detaches the descriptor before calling `close()`, including
EOF, I/O errors, protocol rejection, stalled transfers, heartbeat expiry, and coordinator shutdown.

The coordinator currently listens only on IPv4 loopback. The CLI and worker accept a
numeric IPv4 address. There is no hostname resolution, IPv6, automatic worker
reconnection yet. [Coordinator persistence](persistence.md) now supplies durable
ACK/assignment/result ordering without changing the network frame format.
Logs provide basic event
visibility; full timestamped structured logging remains future work.

`make test` runs protocol unit tests, socket unit tests, and TCP integration
tests. `make test-sanitize` instruments the C binaries for the same checks. The
test guide in `tests/README.md` explains the cases and the optional port 9000 run.
