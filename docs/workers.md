# Workers and the coordinator registry

For a first coordinator launch, explicitly create its WAL with `--init-wal`.
Later launches recover the same file; see [persistence startup](persistence.md#starting-and-restarting).

The coordinator accepts WORKER_REGISTER, assigns a worker ID, returns
WORKER_REGISTER_ACK, and records HEARTBEAT messages from that connection.
Its registry is in `src/coordinator/worker_registry.c`, with the public interface
in `include/worker_registry.h`. The wire format is documented in
[protocol.md](protocol.md); the event loop is described in [networking.md](networking.md).

The worker executable connects and registers, reads its assigned ID from
the acknowledgment, and sends periodic heartbeats. The coordinator expires
registrations that miss their heartbeat deadline. The [scheduler](scheduling.md)
now assigns queued jobs to idle workers and retries interrupted assignments.
Workers execute one [built-in task](tasks.md) while heartbeating and report its
result or failure. Persistent recovery remains future work. Integration tests cover both real workers
and controlled peers.

## Run two workers

Build with `make`, then start the coordinator and two workers in separate terminals:

```sh
# Terminal 1
./build/debug/faultline-coordinator

# Terminal 2
./build/debug/faultline-worker

# Terminal 3
./build/debug/faultline-worker
```

The first two registrations on a fresh coordinator receive distinct IDs:

```text
[INFO] worker registered worker_id=1 coordinator=127.0.0.1:9000 heartbeat_interval_ms=2000
[INFO] worker registered worker_id=2 coordinator=127.0.0.1:9000 heartbeat_interval_ms=2000
```

The IDs are assigned by registration order, not by terminal or process number.
Both workers remain connected. Stop either with Ctrl+C; the coordinator marks
that registration dead while the other stays connected. Each program also handles
SIGTERM. Output is line-buffered so the registration line appears immediately
when a worker's output is redirected to a file or captured by a test.

Use `faultline-worker --coordinator IPv4:PORT` to override the default endpoint,
or `--help` for usage (with the executable path above unless it is on PATH).
The CLI and worker share endpoint parsing and accept numeric IPv4 addresses
with port numbers 1 through 65535.

## Worker startup

`src/worker/main.c` performs these steps:

1. Parse arguments and configure signal handling.
2. Connect through the shared nonblocking socket helper.
3. Encode and send the 12-byte WORKER_REGISTER frame.
4. Collect and validate the 12-byte ACK header, then its four ID bytes.
5. Decode the complete ACK and retain the assigned ID.
6. Print registration success and start the periodic heartbeat loop.
7. Close the socket on a local stop, disconnection, or error.

The worker accepts only WORKER_REGISTER_ACK with a four-byte payload and nonzero
ID. It does not announce registration after only receiving the header or a
partial ID. Its receive loop preserves fragments and has one five-second deadline
for the complete ACK. Invalid headers fail immediately; EOF during the header
or payload is a failed registration. Connect and send each have their own
five-second budget.

SIGINT/SIGTERM interrupt the ACK or heartbeat wait promptly; connect/send may finish
their bounded operation first. A local stop exits successfully. Coordinator
disconnection, invalid ACKs, or unexpected data after registration exit with
failure. Valid JOB_ASSIGN frames start a task thread while heartbeats continue.
Results and failures are reported over the same connection. All shutdown paths
cancel and join active work. Automatic reconnect remains future work.
Its retained ID applies only to this connection.

## Heartbeat interval and timeout

| Setting | Program | Default | Option |
| --- | --- | --- | --- |
| Time between heartbeat sends | Worker | 2000 ms (2 seconds) | `--heartbeat-interval-ms MS` |
| Maximum time without a valid heartbeat | Coordinator | 6000 ms (6 seconds) | `--heartbeat-timeout-ms MS` |

These are runtime arguments; recompilation is unnecessary. They accept decimal
integers from 1 through `INT_MAX` (2147483647 on the target macOS/Linux builds).
Zero, negatives, signs, fractions, whitespace, overflow, missing values, and
duplicate options are rejected. Address/port and timing options may appear in
either order. `faultline_parse_duration_ms()` checks before multiplying to avoid
integer overflow and leaves the output unchanged on failure.

For example, use a one-second interval and a three-second timeout:

```sh
# Coordinator terminal
./build/debug/faultline-coordinator --port 9000 --heartbeat-timeout-ms 3000

# Each worker terminal
./build/debug/faultline-worker --coordinator 127.0.0.1:9000 --heartbeat-interval-ms 1000
```

Each process owns its setting. Values are not negotiated in registration or sent
on the wire. Choose a timeout comfortably longer than every worker's interval,
allowing for network and scheduling delays. A worker whose interval exceeds the
coordinator's timeout can expire before its first heartbeat. Very small positive
values are accepted for experiments; they do not promise real-time scheduling.

After validating the complete registration ACK, `worker_loop()` encodes one
16-byte frame: the existing 12-byte HEARTBEAT header and its four-byte assigned ID,
all in big-endian order. The frame is reused because the ID does not change on
this connection. No timestamp or heartbeat acknowledgment is added to the protocol.

The first send is due one interval after ACK processing. `wait_for_input()` uses
`poll()` to wait for connection activity, a stop signal, or that deadline. A due
heartbeat is a normal timer event; a deadline reached while waiting for an ACK
remains a registration error. The poll wait is capped at 250 ms for prompt signal
handling; the worker sleeps in the kernel between events instead of spinning.

`faultline_send_all()` sends the entire frame despite partial writes. Failure
ends the worker connection. After local send completion, the next deadline is
the current monotonic time plus the interval. This avoids catch-up bursts after
a pause or delayed send. Connect, registration send, and each heartbeat send
retain the existing five-second operation budget; these bounded helpers may
finish before a pending local stop is honored. Heartbeats stay on the main
thread; a separate task pthread handles computation. While busy, the main loop
also checks completion at most every 50 ms.

The coordinator uses the following rule for every ALIVE worker:

```text
elapsed = now_ms - last_heartbeat_ms
expire when elapsed >= heartbeat_timeout_ms
```

Registration supplies the first baseline, so a worker that never sends a
heartbeat still expires. A valid heartbeat renews it. The coordinator shortens
its `poll()` wait to the nearest heartbeat deadline, reads the clock again after
waking, and checks expiry before handling socket events. Thus an expired worker
cannot recover its old identity by sending a late heartbeat, even if those bytes
are already buffered in the socket. OS scheduling delays can postpone detection.

For example, with the defaults:

```text
time 0s: registration establishes the initial baseline
time 2s: heartbeat received; deadline becomes 8s
time 4s: heartbeat received; deadline becomes 10s
time 6s: no heartbeat; worker remains ALIVE
time 8s: no heartbeat; worker remains ALIVE
time 10s: six seconds since the last heartbeat; worker becomes DEAD
```

Timeout closes that connection through the existing cleanup path: mark DEAD,
detach the descriptor, and close the socket. Other workers and the CLI continue
to run. A new connection must register and receives a fresh ID. The scheduler
handles an active job through WORKER_LOST: requeue at the back when retries
remain, or mark FAILED when they are exhausted.

A timeout means the coordinator considers this registration unavailable. It does
not prove the process crashed: a pause, network delay, or overloaded machine can
produce the same observation. Attempt identity prevents a late report from
updating a newer assignment. Built-in tasks are safe to repeat; workers cancel
active computation once connection loss is detected, but attempts can overlap.

## Verify failure detection

For busy-worker SIGKILL or SIGSTOP/heartbeat recovery, with the job completed by
another connected worker, run `make test-recovery`; see [worker recovery](recovery.md).
That suite also resumes the expired worker with SIGCONT during and after the retry,
checking that the original process cannot change current job state or the saved result.
The detection scenarios below focus on registration/connection liveness.

Failure detection has two inputs: a socket reporting disconnection and a worker
missing its heartbeat deadline. Both mark the registration DEAD and close its
connection through the same coordinator cleanup path.

| Experiment | What the coordinator observes | Expected death reason in the tests |
| --- | --- | --- |
| Stop a real worker with SIGTERM | Worker handles the signal, closes its socket, and exits successfully | `eof` |
| Kill a real worker with SIGKILL | OS terminates the process and closes its sockets | `eof` |
| Reset a registered test peer's connection | TCP receive fails | `recv_error` |
| Stop heartbeats while leaving the test peer's socket open | No disconnect; heartbeat deadline elapses | `heartbeat_timeout` |
| Pause a real worker with SIGSTOP | Process remains present and socket stays open; heartbeats cease | `heartbeat_timeout` |

Run these cases together:

```sh
make test-failures
make SANITIZE=1 test-failures
```

They are also included in `make test` and `make test-sanitize`. The new suite
adds explicit graceful-exit, forced-exit, TCP-reset, and stopped-heartbeat cases;
the earlier real-worker pause test now lives there with stronger assertions.

Connection-loss cases require detection within 2.5 seconds of triggering the
failure, well before the default six-second heartbeat timeout, and assert that
no heartbeat-timeout event occurred. These are local acceptance bounds with
scheduling slack, not a production latency guarantee. An application exit does
not imply a TCP reset: even SIGKILL can appear as orderly EOF because the OS
closes the socket. The explicit reset case exercises the receive-error path.

For heartbeat-based detection, a controlled peer first sends two valid heartbeats
and then sends nothing. It never closes or shuts down its connection before
the coordinator closes it. The test verifies the connection stays open before
expiry, its last heartbeat time stops changing, and detection occurs after the
configured six seconds of silence. The separate SIGSTOP experiment exercises
the real worker executable with a 750 ms timeout and confirms that it has not
exited when the coordinator marks it dead.

Each case keeps a second real worker running. Tests require a new heartbeat from
that surviving worker after failure detection and a successful CLI PING/PONG.
They check the failed ID has exactly one death event. Worker-exit and pause cases
also verify that a replacement can register with a fresh ID. Paused processes
are resumed in `finally` blocks so test cleanup can stop them reliably.

Timeout diagnostics now include:

| Field | Meaning |
| --- | --- |
| `timeout_ms` | Configured silence limit. |
| `detected_at_ms` | Coordinator's monotonic time when it checks expiry. |
| `silence_ms` | Time since the latest accepted heartbeat or initial registration. |

The tests check `silence_ms >= timeout_ms` and that subtracting the death event's
`last_heartbeat_ms` from `detected_at_ms` reproduces `silence_ms`. Logs distinguish
the reason for failure without claiming the remote process necessarily crashed.

## Observe a worker timeout manually

With a coordinator already running, use a worker terminal:

```sh
./build/debug/faultline-worker &
worker_pid=$!
sleep 5
kill -STOP "$worker_pid"
sleep 7
kill -CONT "$worker_pid"
wait "$worker_pid"
```

`SIGSTOP` pauses the worker without closing its TCP socket. After six seconds
since its latest valid heartbeat, the coordinator logs `heartbeat_timeout` and
`worker_dead ... reason=heartbeat_timeout`. `SIGCONT` resumes the worker so it can
observe the closed connection and exit with failure; `wait` therefore returns a
nonzero status. A second worker should continue sending heartbeats throughout.
By contrast, stopping a worker with Ctrl+C closes its socket, allowing immediate
disconnect handling without waiting for the heartbeat timeout.

## Identity and storage

The registry contains 64 records. Each record holds:

| Field | Meaning |
| --- | --- |
| `id` | Coordinator-issued unsigned 32-bit worker ID. Zero is unassigned. |
| `fd` | Current connection's socket descriptor; -1 for unused/dead records. |
| `state` | UNUSED, ALIVE, or DEAD. |
| `last_heartbeat_ms` | Registration's initial liveness baseline, then the coordinator's monotonic time of the latest accepted heartbeat. |

Registration takes the first unused or dead slot and assigns a fresh ID starting
at 1. IDs increase independently of descriptors and array positions. A request
rejected by the registry does not consume an ID. Once assigned, an ID stays spent
even if sending the acknowledgment fails. Assigning `UINT32_MAX` exhausts the counter;
subsequent registrations fail instead of wrapping to zero or reusing an old ID.

Consider this sequence:

```text
register A:       ID 1, fd 7, ALIVE
disconnect A:    ID 1, fd -1, DEAD
register B:       ID 2, fd 7, ALIVE
```

The operating system reused descriptor 7, but worker B did not inherit worker
A's identity. Heartbeat and death operations require a matching live ID and
descriptor pair. A stale operation for ID 1 cannot modify ID 2.

Dead records retain their ID and last timestamp until a new registration reuses
the slot. This is a bounded current registry, not an unlimited history. A live
slot is never replaced. Look up workers by ID each time; pointers into dead slots
must not be retained as identities because those slots may later hold new workers.

The registry is owned by the coordinator's single event loop. It allocates no
heap memory, opens/closes no sockets, and needs no locking in this usage. Only the
coordinator and registry/persistence test binaries link its implementation.
Registry connections reset on coordinator restart, while the ID allocator is
restored from the WAL. Old connections must register again after reconnecting;
IDs are never reused within the same retained WAL history.

## Lifecycle and timestamps

An empty client connection has worker ID zero and no registry entry. Receiving a
complete WORKER_REGISTER prepares an ALIVE entry, appends/syncs the new ID
allocation, then publishes the entry before queuing its ACK. If sending
that ACK fails, normal connection cleanup marks the new entry dead.

A complete heartbeat must contain the ID assigned to the sending connection.
The coordinator and registry both check ownership. An unregistered sender,
duplicate registration, a wrong ID, or a malformed message causes that connection
to close; an offending worker cannot update another worker's heartbeat time.

ALIVE means registered and not yet marked unavailable, rather than proof that
the remote process is healthy. DEAD means the registration's connection has
closed, been rejected, or exceeded its heartbeat timeout. All registered-client close paths mark the entry dead
and clear its descriptor before the OS can reuse it. The last timestamp remains
available until the slot is replaced.

Times use `CLOCK_MONOTONIC` milliseconds, not wall-clock dates or worker-supplied
timestamps. Registration initializes the heartbeat timeout's baseline before
the first heartbeat arrives. Later, only complete,
validated heartbeats advance it. PINGs, replies, and partial messages only update
the separate I/O progress timestamp. The registry rejects a heartbeat timestamp
older than the one already stored; equal timestamps are allowed.

The heartbeat timeout applies to all registered workers, including those with
partial input or pending replies. The separate five-second transport inactivity
limit still protects unregistered clients, partial incoming frames, and pending
replies. Successful byte transfers can renew that transport timer while the
heartbeat deadline continues approaching. Neither PINGs nor a trickle of an
incomplete heartbeat can keep a registration alive indefinitely.

## API

| Function | Operation |
| --- | --- |
| `faultline_worker_registry_init()` | Initialize an empty registry and ID counter once. |
| `faultline_worker_register()` | Add an ALIVE record and return a fresh ID. |
| `faultline_worker_find()` | Find an ALIVE or retained DEAD record by ID. |
| `faultline_worker_timed_out()` | Read-only check for an ALIVE record whose heartbeat deadline has elapsed. |
| `faultline_worker_heartbeat()` | Verify the ID/connection pair and update heartbeat time. |
| `faultline_worker_mark_dead()` | Verify the ID/connection pair, mark DEAD, and detach the descriptor. |

The caller supplies monotonic timestamps, making the registry testable without
sleeping. Registry errors distinguish bad arguments, duplicate connections,
capacity, ID exhaustion, unknown IDs, dead workers, and connection mismatches.
Failed operations leave records and registration output unchanged. Socket
ownership stays with `main.c`; marking a worker dead does not itself close a socket.

## Manual wire-format peer

Build and start the coordinator in one terminal:

```sh
make
./build/debug/faultline-coordinator
```

For protocol inspection, this optional Python peer registers, prints its assigned
ID, sends one heartbeat, and disconnects. It uses only the Python standard library;
the real worker executable handles periodic sending automatically.

```sh
python3 - <<'PY'
import socket
import struct

register = bytes.fromhex('46 4c 49 4e 00 01 00 03 00 00 00 00')
ack_header = bytes.fromhex('46 4c 49 4e 00 01 00 04 00 00 00 04')
heartbeat_header = bytes.fromhex('46 4c 49 4e 00 01 00 05 00 00 00 04')

with socket.create_connection(('127.0.0.1', 9000), timeout=2) as connection:
    connection.sendall(register)
    reply = bytearray()
    while len(reply) < 16:
        chunk = connection.recv(16 - len(reply))
        if not chunk:
            raise RuntimeError('Coordinator closed before the complete ACK')
        reply.extend(chunk)
    if reply[:12] != ack_header:
        raise RuntimeError('Unexpected acknowledgment header')
    worker_id = struct.unpack('!I', reply[12:])[0]
    if worker_id == 0:
        raise RuntimeError('Coordinator returned an unassigned ID')
    print(f'Registered as worker {worker_id}')
    connection.sendall(heartbeat_header + struct.pack('!I', worker_id))
PY
```

The coordinator logs `worker_registered`, `worker_register_ack_sent`,
`heartbeat_received`, `heartbeat_timeout`, and `worker_dead`. Registration/death events include state;
registration/heartbeat/death events include the last heartbeat timestamp. Each
event carries both `worker_id` and `fd` to distinguish identity from connection.
The descriptor on a death log identifies the socket being closed; the registry
record's descriptor has already been cleared to -1.

Startup logs show the effective timeout/interval. Workers log `heartbeat_sent`
with their ID after a complete local send. This alone does not prove coordinator
receipt; the coordinator's `heartbeat_received` event confirms processing.

Run `make test` and `make test-sanitize` for registry unit tests, real coordinator
worker-message tests, and the existing protocol and TCP regression tests.
