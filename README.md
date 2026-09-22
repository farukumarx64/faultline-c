# Faultline

Faultline is a distributed job execution engine being built in C11. Its planned
MVP distributes independent jobs across workers, detects worker failures,
retries interrupted work, and recovers coordinator state after a restart.

The coordinator and CLI now exchange framed PING/PONG messages over TCP. The
coordinator handles multiple clients with nonblocking sockets and `poll()`, and
the shared networking code handles partial transfers and deadlines. Protocol
unit tests, socket tests, and process integration tests cover the exchange.
The coordinator also accepts worker registration, assigns IDs, and tracks each
worker's connection, liveness state, and last heartbeat time in a bounded registry.
It checks incoming heartbeat IDs against their connections and marks workers
dead on disconnect or heartbeat expiry. The worker executable connects, registers,
prints its assigned ID, and sends a heartbeat every two seconds. The coordinator
expires a worker after six seconds without a valid heartbeat. Both durations are
configurable. Built-in task execution is implemented. The
[durability contract](docs/durability.md) defines the persistence phase. The
[WAL format](docs/wal-format.md) now has versioned headers, explicit job payloads,
checksums, and tested byte codecs. The [WAL writer](docs/wal-writer.md) now creates
and locks new logs, handles partial writes, and syncs each complete record before
success. [WAL replay](docs/wal-replay.md) now reconstructs saved jobs, results,
retry counts, FIFO order, and ID counters, repairs incomplete tails, and resumes
the locked writer. Coordinator integration and startup reconciliation remain
pending; live coordinator state remains in memory.

Dedicated failure tests distinguish worker exit and TCP reset from missed
heartbeats on an open connection. A healthy worker and the CLI must remain usable
in every case; timeout logs report the measured silence duration.

An in-memory job model now defines task types, owned arguments/results, states,
worker assignments, attempt numbers, timestamps, and bounded retry transitions.
Its operations reject invalid transitions and stale reports. A coordinator FIFO
module now holds up to 256 pending job IDs, preserves insertion order, and rejects
duplicates or overflow. The shared protocol codec now encodes and validates job
submission, acknowledgment, assignment, started, completed, and failed messages,
including bounded arguments/results and attempt identity. The CLI now submits
jobs and receives IDs. The coordinator retains up to 256 full job records and
assigns the oldest queued job to an alive, idle worker. Reports update job state;
worker loss and task failures apply bounded retries. Workers execute `sleep`,
`prime_count`, `fibonacci`, and `hash` while heartbeating, report real results,
and take the next job. See [built-in tasks](docs/tasks.md) for inputs and examples.

## Build and run

Requirements: Make, POSIX threads, and a C11 compiler such as Clang or GCC. The project targets
macOS and Linux. Sanitizer builds also require the compiler's AddressSanitizer
and UndefinedBehaviorSanitizer runtimes. The integration tests require Python 3
(standard library only).

```sh
make
```

Start the coordinator in one terminal:

```sh
./build/debug/faultline-coordinator --port 9000
```

Then send a PING from a second terminal:

```sh
./build/debug/faultline ping --coordinator 127.0.0.1:9000
# PONG
```

All three programs default to `127.0.0.1:9000`, so `faultline-coordinator`,
`faultline ping`, and `faultline-worker` work without address options. Use the executable paths
above unless you have added their directory to PATH. Stop the coordinator with
Ctrl+C. It closes active connections and its listening socket before exiting.

With the coordinator running, start a worker in each of two additional terminals:

```sh
./build/debug/faultline-worker
```

Each prints its assigned ID and stays running. On a fresh coordinator, the first
two registrations get IDs 1 and 2 (process scheduling determines which gets 1):

```text
[INFO] worker registered worker_id=1 coordinator=127.0.0.1:9000 heartbeat_interval_ms=2000
[INFO] worker registered worker_id=2 coordinator=127.0.0.1:9000 heartbeat_interval_ms=2000
```

Use `./build/debug/faultline-worker --coordinator 127.0.0.1:9000` to specify an
endpoint. Ctrl+C or SIGTERM stops the worker and closes its socket. Unexpected
coordinator disconnection makes the worker report an error and exit; automatic
reconnection is not implemented yet.

To change the heartbeat timings, start the programs with these options:

```sh
# Coordinator terminal: expire after 3 seconds without a valid heartbeat
./build/debug/faultline-coordinator --port 9000 --heartbeat-timeout-ms 3000

# Worker terminal: send every 1 second
./build/debug/faultline-worker --coordinator 127.0.0.1:9000 --heartbeat-interval-ms 1000
```

Durations are positive decimal milliseconds. Configure the timeout comfortably
above every worker's interval; the separate processes do not negotiate these
values. The [worker guide](docs/workers.md) explains timing, timeout logs, and a
pause/resume experiment that demonstrates failure detection with an open socket.

The coordinator currently binds only to IPv4 loopback. The CLI and worker accept numeric
IPv4 addresses and ports from 1 through 65535. Hostname resolution, IPv6, and a
coordinator bind-address option are not implemented yet.

Warnings are enabled for common defects, conversions, shadowed variables,
function prototypes, and format strings. Dependency files ensure changes to
included headers trigger recompilation.

Build with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make sanitize
```

Run the same examples using `build/sanitize/` in place of
`build/debug/`.

Debug symbols and frame pointers make sanitizer reports easier to investigate.
Sanitizer builds stop on detected undefined behavior. Normal and sanitizer
outputs live in separate directories. Use `make clean` to remove both; also
clean before changing compilers or flags within the same build configuration.

## Submit and execute jobs

With the coordinator running, submit jobs before or after starting workers:

```sh
./build/debug/faultline submit sleep --args 1000 --max-retries 1
# job_id=1
./build/debug/faultline submit prime_count --args 100
# job_id=2
./build/debug/faultline submit fibonacci --args 10
# job_id=3
./build/debug/faultline submit hash --args hello
# job_id=4
```

The coordinator queues jobs until an idle worker is available, then assigns them
in FIFO order. Each worker executes one job at a time while continuing heartbeats.
Jobs move through ASSIGNED, RUNNING, and DONE. The coordinator stores and logs
results: the examples above produce `slept_ms=1000`, `25`, `55`, and
`a430d84680aabd0b`. Invalid task inputs report failure and follow the retry policy.
The CLI prints an acceptance ID; result/status queries are still pending.
See [task arguments, algorithms, and execution](docs/tasks.md).

Arguments are passed through as text or hex-decoded bytes, up to 1024 bytes.
Retry allowance defaults to zero. The store retains 256 total jobs, including
terminal records; full stores reject further submissions, and restarting loses
all in-memory jobs. See [the scheduling guide](docs/scheduling.md) for CLI options,
acceptance guarantees, worker eligibility, retries, and the current limits.

[Worker recovery checks](docs/recovery.md) kill a busy worker with SIGKILL or
pause it with SIGSTOP until its heartbeat expires. In both cases, an
already-connected worker completes the same job on attempt 2 and remains
available for another job.
Additional checks resume the expired worker while attempt 2 is running or after
it completes, verifying that the current assignment and accepted result stay intact.
Retry-exhaustion checks kill successive owners until the allowance runs out,
verify FAILED without another assignment, and confirm a healthy worker can still
complete a new job. Two retries allow three attempts; zero retries allow one.

Assignments are revocable leases tied to worker connection/heartbeat liveness.
An expired worker can still be computing while another starts the retry.
Job/worker/attempt validation protects accepted coordinator state, while tasks
must be safe to repeat under the bounded at-least-once retry policy. Heartbeats
do not prove task progress, and submission ACKs currently confirm in-memory
acceptance only. See [the lease and execution guarantees](docs/recovery.md#assignment-lease).
The [fault-tolerance phase review](docs/recovery.md#phase-review) records the
verified recovery cases and the boundary before persistence.

## Tests

```sh
make test
make test-sanitize
```

Both commands build and run C unit tests and Python integration tests against
the real executables. `test-sanitize` instruments all C programs under test.
Use `make test-unit` or `make test-integration` to run either layer separately.
Use `make test-wal` for WAL format checks, or `make SANITIZE=1 test-wal`
for the same checks with AddressSanitizer/UBSan.
Use `make test-wal-writer` for real-file appends, sync ordering, storage-error
injection, locking, and writer-crash checks; add `SANITIZE=1` for instrumentation.
Use `make test-wal-replay` for history validation, reconstructed state, tail
repair, and append resumption; add `SANITIZE=1` for instrumentation.
Use `make test-scheduling` for CLI submission and scheduling scenarios.
Use `make test-execution` for task results, concurrent workers, and cancellation.
Use `make test-recovery` for crash/heartbeat recovery, resumed-worker protection,
and retry exhaustion, or `make SANITIZE=1 test-recovery` for the sanitizer build.
Use `make test-failures` to run only the five failure-detection scenarios, or
`make SANITIZE=1 test-failures` to run them with AddressSanitizer/UBSan.

Integration tests normally choose an available loopback port, leaving the
default-endpoint checks skipped. To also exercise all three programs' port 9000
defaults, first stop any existing coordinator and run:

```sh
make test-integration INTEGRATION_ARGS='--port 9000'
```

See [the test guide](tests/README.md) for coverage and failure diagnostics.

## Project layout

```text
faultline/
├── Makefile
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   ├── job-protocol.md
│   ├── networking.md
│   ├── workers.md
│   ├── jobs.md
│   ├── queue.md
│   ├── scheduling.md
│   ├── tasks.md
│   ├── recovery.md
│   ├── durability.md
│   ├── wal-format.md
│   ├── wal-writer.md
│   └── wal-replay.md
├── include/             Shared C headers
├── src/
│   ├── common/          Shared protocol, networking, and logging code
│   ├── coordinator/     Event loop, registry, job store, scheduler, and WAL modules
│   ├── worker/          Registration, heartbeats, and assignment reception
│   └── cli/             PING and job submission
└── tests/               C unit/storage tests and TCP integration tests
```

Read [the architecture note](docs/architecture.md) for component responsibilities,
MVP guarantees, and design decisions still to be resolved. Read
[the protocol specification](docs/protocol.md) for byte offsets, network byte
order, validation rules, and the C API. The [networking walkthrough](docs/networking.md)
explains the PING/PONG exchange, connection state, partial I/O, and deadlines.
The [worker guide](docs/workers.md) describes the registration exchange, worker
IDs, connection ownership, and heartbeat timing. The [job guide](docs/jobs.md)
defines the record, state transitions, attempt identity, and retry limits.
The [queue guide](docs/queue.md) explains FIFO ordering, capacity, and job ownership.
The [job message specification](docs/job-protocol.md) defines payload offsets,
message semantics, and validation. The [scheduling guide](docs/scheduling.md)
connects those pieces to CLI submission and live FIFO dispatch. The
[task guide](docs/tasks.md) covers built-in execution and result reporting; the
[recovery guide](docs/recovery.md) records worker-failure guarantees and checks.
The [durability contract](docs/durability.md) defines the upcoming WAL acceptance,
restart, and retry rules. The [WAL format specification](docs/wal-format.md)
defines exact file/record bytes and validation. The [WAL writer guide](docs/wal-writer.md)
explains complete appends, sync boundaries, and storage failures. The
[WAL replay guide](docs/wal-replay.md) explains historical validation, state
reconstruction, incomplete-tail repair, and safely resuming appends.
