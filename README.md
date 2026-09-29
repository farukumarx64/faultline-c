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
the locked writer. [Coordinator persistence](docs/persistence.md) now connects
these modules to live operations: sync before ACK/dispatch/result publication,
and reconcile interrupted attempts before listening after restart.
[Coordinator crash checks](docs/coordinator-crashes.md) now kill the process
around WAL writes and flushes, recover with the same log, and execute restored
work through real CLI/worker scenarios.

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
The CLI now [queries job status](docs/status.md) by ID, displaying state, worker,
attempt, retries, failure reason, and escaped result bytes. Read-only coordinator
lookup uses the [job-status protocol](docs/job-status-protocol.md), including an
explicit not-found reply, and works with jobs recovered from the WAL.
[`jobs` and `workers`](docs/listings.md) now list retained job summaries and
worker liveness, heartbeat age, and active assignments in ID order.
[`stats`](docs/stats.md) reports retained job totals, current worker gauges,
session activity, throughput, and completion latency with explicit restart scopes.

## Build and run

Requirements: Make, POSIX threads, and a C11 compiler such as Clang or GCC. The project targets
macOS and Linux. Sanitizer builds also require the compiler's AddressSanitizer
and UndefinedBehaviorSanitizer runtimes. The integration tests require Python 3
(standard library only).

```sh
make
```

Initialize a new coordinator log on the first launch:

```sh
./build/debug/faultline-coordinator --port 9000 --wal faultline.wal --init-wal
```

On subsequent launches, omit `--init-wal` to recover the same file:

```sh
./build/debug/faultline-coordinator --port 9000 --wal faultline.wal
```

The default path is `faultline.wal`. Initialization refuses to overwrite an
existing log, and ordinary startup refuses a missing log. See
[persistence startup and guarantees](docs/persistence.md).

Then send a PING from a second terminal:

```sh
./build/debug/faultline ping --coordinator 127.0.0.1:9000
# PONG
```

All three programs default to `127.0.0.1:9000`, so `faultline-coordinator`,
`faultline ping`, and `faultline-worker` work without address options. Use the executable paths
above unless you have added their directory to PATH. The coordinator also requires
its existing WAL unless `--init-wal` is supplied. Stop the coordinator with
Ctrl+C. It closes active connections and its listening socket before exiting.

With the coordinator running, start a worker in each of two additional terminals:

```sh
./build/debug/faultline-worker
```

Each prints its assigned ID and stays running. On a fresh coordinator, the first
two registrations get IDs 1 and 2 (process scheduling determines which gets 1).
These excerpts omit the UTC timestamp and process/monotonic metadata:

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
Submission prints an acceptance ID. Query it with `./build/debug/faultline status 1`.
See [task arguments, algorithms, and execution](docs/tasks.md).

Arguments are passed through as text or hex-decoded bytes, up to 1024 bytes.
Retry allowance defaults to zero. The store retains 256 total jobs, including
terminal records; full stores reject further submissions, and restarting preserves
these records and the capacity limit. See [the scheduling guide](docs/scheduling.md) for CLI options,
acceptance guarantees, worker eligibility, retries, and the current limits.

For example, after job 1 completes on worker 1:

```text
job_id=1
state=DONE
worker_id=1
attempt=1
retries=0/1
failure=NONE
result_bytes=13
result="slept_ms=1000"
```

`status` accepts `--coordinator IPv4:PORT` and reads one snapshot per invocation.
It exits 0 for a known job (including FAILED), 2 for an unknown ID, and 1 on an
argument, network, protocol, or output error. See [the status guide](docs/status.md)
for queued/retried states, result escaping, and restart behavior.

Inspect all retained jobs and worker registrations:

```sh
./build/debug/faultline jobs
./build/debug/faultline workers
```

Both accept `--coordinator IPv4:PORT` and exit 0 for a valid listing, including
an empty one. Jobs include completed and failed records. Workers include retained
dead registrations until slot reuse; the live registry starts empty after restart.
See [the listing guide](docs/listings.md) for table columns and snapshot semantics.

Read aggregate statistics:

```sh
./build/debug/faultline stats
```

It also accepts `--coordinator IPv4:PORT`. Job totals are reconstructed from the
WAL; current worker gauges and session measurements reset on restart. Startup
recovery outcomes are included in retained totals and reported separately from
runtime session activity. See [counter definitions and performance limits](docs/stats.md).

Coordinator and worker logs include UTC time, severity, component, event, and
identity fields. Job events distinguish requeued attempts, terminal failures,
durably accepted results, and restored state. Worker sends explicitly leave
coordinator acceptance unconfirmed. See [the logging guide](docs/logging.md) and
[CLI and observability phase review](docs/observability-review.md) for how to
interpret commands and logs together.

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
do not prove task progress. Submission ACKs now follow a successful WAL sync;
missing an ACK still leaves an uncertain client outcome. See [the lease and execution guarantees](docs/recovery.md#assignment-lease).
The [fault-tolerance phase review](docs/recovery.md#phase-review) records the
verified recovery cases and the boundary before persistence.

## Persistence guarantees

- A received submission ACK means the job is recoverable after a coordinator
  process crash using the same retained WAL and working local storage.
- Job inputs, IDs, retry accounting, queue order, and saved terminal outcomes
  survive. DONE and FAILED jobs receive no new attempts.
- Interrupted ASSIGNED/RUNNING jobs retry from the beginning if their allowance
  permits. Work can execute more than once; finite retries can end in failure.
- Workers need fresh connections and registrations. A missing submission ACK
  remains uncertain; manual resubmission can create another job.
- This covers process crashes under the storage assumptions, not storage loss,
  universal power-loss survival, exactly-once effects, or automatic failover.

The [persistence phase review](docs/persistence-review.md) records the full
verification results, acceptance boundaries, retry rules, and exclusions.

## Tests

```sh
make test
make test-sanitize
```

Both commands build and run C unit tests and Python integration tests against
the real executables. `test-sanitize` instruments all C programs under test.
The [Linux CI workflow](.github/workflows/linux-ci.yml) runs the full suite on
Ubuntu 24.04 for pushes and pull requests: GCC 13 for a normal build and Clang 18
with AddressSanitizer/UBSan. Both treat compiler warnings as errors and include
the port 9000 default-endpoint checks. See [the CI guide](docs/ci.md) for triggers,
logs, reproduction commands, and the distinction between local validation and
GitHub-hosted results.
The [batch baseline harness](docs/batch-testing.md) now runs five workers and
100 jobs with exact result/ID accounting, shared deadlines, and process cleanup:
`make test-batch` (or `make SANITIZE=1 test-batch`). Use
`BATCH_ARGS='--workers 3 --jobs 9 --sleep-ms 25'` for a small run and
`make test-batch-harness` for the harness's own regression checks. These are
separate opt-in targets. The [seeded chaos harness](docs/chaos-testing.md) now
runs with `make test-chaos` (also `SANITIZE=1`), killing busy workers and starting
replacements while retaining the plan, actions, and recovery evidence. Use
`CHAOS_ARGS` to configure it and `make test-chaos-harness` for its regression suite.
Each run saves its drain cohort and a per-job `accounting.json` report, proving
that the acknowledged IDs partition into completed and terminally failed jobs,
with exact results and valid retry histories. Matching totals alone cannot pass.
Use `make test-unit` or `make test-integration` to run either layer separately.
Use `make test-job-status-protocol` for status payloads and validation, or add
`SANITIZE=1` for AddressSanitizer/UBSan.
Use `make test-status` for live CLI queries, read-only checks, and recovered results;
add `SANITIZE=1` for instrumentation.
Use `make test-list-protocol` for listing codecs and `make test-listings` for live
job/worker snapshots; add `SANITIZE=1` for instrumentation.
Use `make test-stats-protocol` for statistics codecs and aggregation, and
`make test-stats` for live counter/restart checks; both accept `SANITIZE=1`.
Use `make test-logs` for log formatting/escaping and `make test-observability`
for command, WAL, and log agreement across execution/failure/restart; both accept
`SANITIZE=1`.
Use `make test-wal` for WAL format checks, or `make SANITIZE=1 test-wal`
for the same checks with AddressSanitizer/UBSan.
Use `make test-wal-writer` for real-file appends, sync ordering, storage-error
injection, locking, and writer-crash checks; add `SANITIZE=1` for instrumentation.
Use `make test-wal-replay` for history validation, reconstructed state, tail
repair, and append resumption; add `SANITIZE=1` for instrumentation.
Use `make test-persistence` for coordinator commit ordering and restart checks;
add `SANITIZE=1` for instrumentation.
Use `make test-startup-recovery` for restored job states, interrupted-attempt
retry accounting, and fresh registration after restart; add `SANITIZE=1` for instrumentation.
Use `make test-coordinator-crashes` for SIGKILL at selected WAL boundaries and
real CLI/worker restart scenarios; add `SANITIZE=1` for instrumentation.
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
├── .github/workflows/linux-ci.yml
├── Makefile
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   ├── job-protocol.md
│   ├── job-status-protocol.md
│   ├── status.md
│   ├── listings.md
│   ├── stats.md
│   ├── logging.md
│   ├── observability-review.md
│   ├── ci.md
│   ├── chaos.md
│   ├── batch-testing.md
│   ├── chaos-testing.md
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
│   ├── wal-replay.md
│   ├── persistence.md
│   ├── coordinator-crashes.md
│   ├── persistence-review.md
│   └── request-deduplication.md
├── include/             Shared C headers
├── src/
│   ├── common/          Shared protocol, networking, and logging code
│   ├── coordinator/     Event loop, registry, job store, scheduler, and WAL modules
│   ├── worker/          Registration, heartbeats, and assignment reception
│   └── cli/             PING, submission, status, jobs, workers, and stats
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
The [durability contract](docs/durability.md) defines WAL acceptance,
restart, and retry rules. The [WAL format specification](docs/wal-format.md)
defines exact file/record bytes and validation. The [WAL writer guide](docs/wal-writer.md)
explains complete appends, sync boundaries, and storage failures. The
[WAL replay guide](docs/wal-replay.md) explains historical validation, state
reconstruction, incomplete-tail repair, and safely resuming appends. The
[coordinator persistence guide](docs/persistence.md) connects these operations
to durable live transitions, startup, retry accounting, and failure shutdown.
The [coordinator crash guide](docs/coordinator-crashes.md) describes the test-only
crash harness, verified boundaries, and process-crash guarantees.
The [persistence phase review](docs/persistence-review.md) consolidates the
durable-state promise, repeat-execution rules, evidence, and remaining scope.
The [deferred request-deduplication proposal](docs/request-deduplication.md)
records a future enhancement for safely repeating submissions after a lost ACK.
It is not implemented. The [CLI and observability review](docs/observability-review.md)
records this phase's command semantics, logging guarantees, verification, and
handoff to testing and chaos experiments.
