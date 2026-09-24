# CLI submission and FIFO scheduling

The CLI now submits jobs over TCP, and the coordinator stores them, returns job
IDs, and assigns the oldest queued job to an available worker. The scheduler is
in `src/coordinator/scheduler.c`, with its public API in `include/scheduler.h`.
It combines the existing [job model](jobs.md), [FIFO](queue.md), and
[job message formats](job-protocol.md) without embedding sockets or clocks.

Real workers execute one assignment at a time while continuing heartbeats. They
send STARTED, then COMPLETED with a result or FAILED with the TASK reason. The
coordinator stores and logs completed results and schedules the next queued job.
See [built-in task execution](tasks.md) for algorithms, inputs, and thread ownership.
CLI result/status queries remain later work. [Persistence](persistence.md) now
wraps scheduler operations with durable commit ordering.

## Try it

Build with `make`. Initialize the WAL once with `--init-wal`, as described in
[persistence startup](persistence.md#starting-and-restarting). Start the coordinator
using that existing log, then submit two jobs before starting
workers to see that acceptance does not require an available worker:

```sh
# Terminal 1
./build/debug/faultline-coordinator --port 9000

# Terminal 2
./build/debug/faultline submit sleep --args 10000
# job_id=1
./build/debug/faultline submit sleep --args 10000
# job_id=2

# Terminal 3
./build/debug/faultline-worker

# Terminal 4
./build/debug/faultline-worker
```

A fresh coordinator assigns job 1 first and job 2 second. Which process becomes
the first worker depends on registration timing. The coordinator logs submission
and assignment; each worker logs its job ID, worker ID, attempt, task type,
and argument byte count. Both sleep for ten seconds while sending heartbeats.
A third job waits while both are busy, then runs after a worker reports its result.
The coordinator logs `result="slept_ms=10000"` for each completed sleep. Stopping a
busy worker marks its assignment failed or requeues it according to the retry allowance.

Use `--coordinator 127.0.0.1:PORT` to change the CLI or worker endpoint. Defaults
remain `127.0.0.1:9000`. Existing `faultline ping` behavior is unchanged.

## CLI contract

```text
faultline submit TASK [--args TEXT | --args-hex HEX]
                     [--max-retries N] [--coordinator IPv4:PORT]
```

- TASK is `sleep`, `prime_count`, `fibonacci`, or `hash`.
- `--args` copies the shell argument's bytes, without a trailing NUL. The CLI
  does not parse numbers, JSON, or task-specific semantics. The shell/process
  environment determines text encoding.
- `--args-hex` supplies raw bytes, including embedded zeroes. For example,
  `--args-hex 00aaff` sends three bytes. Hex must have an even number of valid
  hexadecimal digits; upper- and lowercase are allowed, without a `0x` prefix.
- Either argument option can appear once; they are mutually exclusive. Omitting
  both sends zero argument bytes. The maximum is 1024 bytes (2048 hex digits).
- `--max-retries` is a decimal integer in 0..4294967295, default 0. It counts
  additional attempts, so 2 permits at most three assignments. Negative values,
  signs, whitespace, fractions, overflow, duplicate options, and missing values
  are rejected before connecting.

All valid option pairs may appear in any order after TASK. The worker validates
[task-specific arguments](tasks.md); acceptance of a task identifier and bounded
bytes does not guarantee success. Invalid numeric inputs become TASK failures.

The CLI sends one JOB_SUBMIT and waits for JOB_SUBMIT_ACK. It validates the type,
exact eight-byte payload length, and nonzero 64-bit job ID before printing
`job_id=N` and exiting with status 0. Header and payload share one five-second
ACK deadline; fragments do not restart the budget. Connect and send each retain
their existing separate five-second budgets.

Malformed, truncated, missing, or timed-out ACKs yield status 1 and no success
output. After attempting a send, the CLI reports an unconfirmed submission if
it cannot validate an ACK. The coordinator may already have accepted that job.
There is no automatic resubmission, deduplication key, or structured rejection
message in this protocol version. A lost ACK is an uncertain outcome.

## Store ownership and acceptance

The coordinator allocates one scheduler/store at startup and frees it on shutdown.
It retains up to 256 full job records, including QUEUED, active, and terminal jobs.
The FIFO stores pending IDs separately. No terminal records are evicted or reused
yet: after 256 successful submissions, further submissions are rejected even
if jobs have completed. Restart preserves those records and does not free capacity.

The [durability contract](durability.md#submission-acknowledgment-boundary)
is now implemented by `coordinator_store`: prepare a submission on a scratch
scheduler, append and sync its complete creation record, then publish it and
queue its ACK. The scheduler functions below remain pure in-memory operations,
but the coordinator invokes them on scratch before committing their results.

`faultline_scheduler_submit()` prepares a validated QUEUED record, pushes its ID
to the FIFO, stores the owned record, and advances the ID counter. No failing
operations occur after enqueue. Invalid/full/exhausted submissions leave the
store, queue, and output ID unchanged. IDs start at 1 and never repeat within this
retained WAL history; after UINT64_MAX, allocation fails instead of wrapping.

The coordinator queues a success ACK only after WAL sync and publication of
the prepared job and enqueue succeed.
Full-store or invalid submissions close the connection without a success ACK.
Closing a submitter connection after acceptance does not cancel its job or remove
it from the queue. A submitter may send subsequent submissions, with serialized
ACKs, but cannot switch that connection into a registered worker. Registered
workers cannot submit jobs. PING remains available to either role.

## Available workers and FIFO dispatch

Liveness and availability are separate. The worker registry supplies the live
ID/connection and heartbeat deadline. The job store determines whether that
worker already owns an ASSIGNED or RUNNING job. Terminal records retain historical
worker IDs, but do not make a worker busy. There is no second mutable busy flag
that can drift away from job state.

After processing client events, the coordinator considers workers in client-slot
order. A worker is eligible only when it is registered, ALIVE, unexpired, has no
active job, has no pending output, and is between complete input messages. This
ensures its registration ACK is sent first and protects a partial heartbeat or
report from being overwritten. Worker selection is not round-robin or load
balancing; FIFO applies to job selection.

For each eligible worker:

1. Peek the oldest pending ID and locate its full record.
2. Prepare an ASSIGNED copy using the model, incrementing its attempt number.
3. Build a JOB_ASSIGN containing that identity, task, and copied arguments.
4. Remove the same FIFO entry and publish the assigned record.
5. Queue the encoded frame in the worker connection's output buffer.

The job becomes active before the first send, reserving that worker immediately.
A slow/partial send cannot allow another assignment to the same worker. Assignment
means reservation and dispatch, not proof of receipt or execution. The
`job_assigned` coordinator log records this reservation; the worker log records
receipt of the full valid assignment.

No available worker means the queue stays unchanged. Ordering follows successful
coordinator acceptance/enqueue order, not client launch time, socket descriptor,
worker ID, or job completion order. Concurrent CLI requests receive unique IDs
and are queued in the order the single event loop accepts their complete messages.

## Reports, worker release, and retries

The coordinator accepts STARTED, COMPLETED, and FAILED only from registered
worker connections. `faultline_scheduler_report()` verifies the payload worker
ID against that connection, finds the job, and uses the model's owner, attempt,
state, and timestamp checks on a temporary copy. Rejected reports do not change
the record. The coordinator closes an offending connection; if it owns an active
job, normal worker-loss cleanup then handles that assignment separately.

A valid STARTED makes the job RUNNING while the worker remains busy. A valid
COMPLETED copies the result, makes it DONE, and frees the worker for the oldest
waiting job. FAILED follows the retry policy and also releases the current
assignment. STARTED must precede COMPLETED; FAILED is allowed before STARTED.
Reports do not refresh liveness: workers must continue sending HEARTBEAT.

Disconnect, reset, heartbeat expiry, or send failure uses the same cleanup path
and calls `faultline_scheduler_worker_lost()` for the worker's active job. With
retries remaining, it returns to QUEUED and its ID joins the back. Otherwise it
becomes terminal FAILED with reason WORKER_LOST. A replacement registration gets
a new worker ID, and any reassignment gets a new attempt number. Dead workers
are excluded from scheduling; late reports cannot update newer attempts.

The store holds at most 256 jobs and the FIFO holds 256 IDs. Every active job is
absent from the FIFO, so it always has space to requeue. A compile-time assertion
protects this capacity relationship. Requeue publishes the changed job only after
its queue insertion succeeds. The scheduler refreshes its clock snapshot after
connection cleanup so a newly timestamped retry cannot be assigned at an older time.

This implements an assignment lease tied to worker connection/heartbeat liveness
and bounded at-least-once retries while the coordinator remains alive. The
[recovery contract](recovery.md#assignment-lease) explains revocation, overlapping
executions, and the conditions for progress. Independent execution deadlines,
submission deduplication and exactly-once execution remain outside the current
implementation. [Durable recovery](persistence.md) now extends job and retry
state across coordinator restart using the same retained WAL.

## Transport and the worker loop

Coordinator input/output buffers now each hold the largest codec-defined frame,
1072 bytes, including the [status response](job-status-protocol.md) whose handler
is still pending. They use nonblocking reads/writes, retain partial progress, and
handle one frame at a time. Headers are checked for allowed direction/connection
role, and the codec rejects invalid outer bounds before payload collection.
Replies and assignments share the existing per-connection output phase, so one
frame cannot overwrite another. Assignment output gets a fresh progress timestamp;
a worker idle for more than five seconds does not inherit an expired send budget.

After registration, a worker accumulates a bounded JOB_ASSIGN while independently
tracking its next heartbeat. A partial assignment has a total five-second deadline
from its first received byte. Incoming data cannot postpone heartbeats. The worker
checks type, lengths, payload validity, its assigned worker ID, and that it is not
already holding a job before retaining the assignment. Unexpected frames, invalid
identity, a second assignment while busy, EOF, or a stalled partial frame cause
failure exit. Local SIGINT/SIGTERM still closes the socket and exits successfully.

After validation, the worker sends STARTED and launches its task thread. The main
thread keeps heartbeats active, joins completed work, and sends the terminal
report before clearing local ownership. All exit paths cancel and join active
work; [tasks.md](tasks.md) explains synchronization and result formatting.

## Tests

`tests/test_scheduler.c` adds seven deterministic groups: atomic acceptance and
ownership, FIFO reservation and busy rejection, valid reports and worker reuse,
retry order and stale attempts, old-report rejection across retry states and after
completion, capacity with guaranteed retry room, and ID exhaustion/invalid inputs.
Failed operations are checked against snapshots. The [recovery tests](recovery.md)
also resume expired workers before and after their replacement completes the job.

`tests/integration/test_scheduling.py` uses a fresh coordinator for each of its
15 scenarios. It covers real CLI and worker processes, concurrent submissions,
no-worker waiting, FIFO dispatch, completion-driven reuse, fragmented/coalesced
binary submissions, partial worker frames, failures/retries, spoofed/stale reports,
capacity, worker heartbeat expiry and reassignment, CLI ACK validation/deadlines,
and real-worker assignment buffering with continuing heartbeats.

Run `make test-scheduling` for the focused TCP suite, or
`make SANITIZE=1 test-scheduling` for instrumented binaries. The suite and C tests
also run through `make test` and `make test-sanitize`.
