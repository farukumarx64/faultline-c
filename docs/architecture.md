# Faultline MVP architecture

Faultline will distribute independent jobs across worker processes and recover
unfinished work when a worker fails. The v0.1 goal is a small C11 system whose
networking, scheduling, failure handling, and recovery behavior can be explained
and demonstrated with reproducible experiments.

This note describes the intended MVP. The current implementation supports a
tested TCP PING/PONG exchange between the CLI and a coordinator using `poll()`.
The coordinator also registers workers, returns assigned IDs, validates heartbeat
ownership, and records connection state and heartbeat times in a worker registry.
Disconnected or timed-out workers are marked dead. The worker executable connects,
registers, validates its assigned ID, and sends periodic heartbeats. Worker interval
and coordinator timeout are configurable, defaulting to two and six seconds.
The [job model](jobs.md) defines records and validated state transitions, including
attempt identity and bounded requeue/failure rules. The [FIFO queue](queue.md)
holds pending IDs in insertion order independently of full job records. The
[job message codec](job-protocol.md) defines submission, acknowledgment, assignment,
started, completed, and failed payloads. [CLI submission and scheduling](scheduling.md)
now connect the job store and FIFO to live dispatch, validate reports, and apply
bounded retries on failure or worker loss. Workers now execute all four
[built-in tasks](tasks.md) and report results while heartbeating. The
[recovery contract](recovery.md#assignment-lease) defines assignment leases tied
to worker liveness, stale-attempt protection, and bounded at-least-once retries.
[Coordinator persistence](persistence.md) now provides durable transitions and
restart recovery. Result queries and execution deadlines independent of
heartbeats remain future work.

## Components and ownership

```text
CLI client ───── submit over TCP ────────> Coordinator
                                              │
                                     assign jobs over TCP
                                              │
                                      ┌───────┴───────┐
                                   Worker A        Worker B
                                      │               │
                                      └── heartbeats ─┘
                                          and results
                                      to coordinator
```

The **CLI** submits a supported task and its arguments and receives a job ID.
Queries for jobs, workers, and statistics are planned. It does not decide which
worker runs a job.

The **coordinator** owns the authoritative job state, FIFO queue, worker registry,
and current assignments. It gives the oldest queued job to an idle worker,
monitors worker liveness, and applies retry limits. Each accepted transition is
appended and synced to the WAL before its prepared state is published in memory.

Each **worker** connects to the coordinator, registers for an ID, sends periodic
heartbeats, and executes one assigned job at a time. It reports job start,
completion, or failure. Workers may run on the same computer or across a LAN.
Built-in tasks are `sleep`, `prime_count`, `fibonacci`, and `hash`.

## Job lifecycle and recovery

The normal lifecycle is `QUEUED → ASSIGNED → RUNNING → DONE`. An assignment is a
lease tied to the registered worker's connection and heartbeat liveness. The
coordinator revokes it on connection loss or heartbeat expiry. It keeps at most
one authoritative active attempt per job, validating `(job_id, worker_id, attempt)`
on reports. Unfinished jobs return to the FIFO tail while retry allowance remains;
exhausted jobs enter terminal `FAILED`. Reported task errors use the same retry
allowance. Retrying starts the task again from the beginning.

The lease has no independent job deadline. Heartbeats keep a long-running task's
assignment live, even if the task itself stops making progress. Revoking the
lease does not prove the old worker stopped executing. The
[recovery guide](recovery.md) documents expiry, overlap, and the acceptance checks.

The WAL records job creation and meaningful transitions. Restart replay restores
state, terminal outcomes, and queued work. Recovered ASSIGNED/RUNNING attempts
use the worker-loss retry policy, with each recovery outcome synced before the
coordinator listens. Already-queued jobs do not consume another retry just because
startup runs again. See the [durability contract](durability.md) and
[implementation guide](persistence.md).

## Guarantees and limits

- Multiple workers can execute independent jobs concurrently.
- Disconnects and missed heartbeats make workers unavailable for scheduling;
  unfinished work is automatically retried within a configured limit.
- Execution uses bounded at-least-once retry semantics. If completion is
  unconfirmed or a worker is suspected dead while still executing, a task may run
  more than once. Tasks should be idempotent. Retry limits can produce terminal
  failure, even before any computation begins. Eventual execution or success
  requires available workers and continued coordinator/task/network progress.
- Submission ACKs require a complete creation record and successful WAL sync.
  Job identity, input, counters, queue order, and terminal outcomes recover from
  the retained WAL after a coordinator process restart. A missing ACK leaves
  the outcome uncertain; the job may already be durable.
- There is one coordinator and no automatic failover. Scheduling is unavailable
  while it is down. Existing worker computations may continue,
  and restart recovery must account for their uncertain outcomes.

A heartbeat timeout is evidence of unavailability, not proof that a process has
stopped. An expired assignment can overlap with a later retry. The implementation
distinguishes attempts so a late result cannot overwrite a newer attempt or a
terminal result. This protects coordinator state, not external task effects.
See the [full guarantee and its limits](recovery.md#at-least-once-execution-and-its-limits).

The MVP excludes coordinator consensus, exactly-once execution, task
checkpointing, arbitrary shell execution, a web dashboard, Kubernetes
integration, and production authentication or TLS.

## Implementation direction

Use C11, POSIX TCP sockets, and `poll()` for portable event multiplexing on macOS
and Linux, with pthreads where necessary. TCP messages require an explicit wire
format and buffering for partial reads and writes; raw C structs must not be sent
as the protocol. The implemented 12-byte header and its encoding are documented
in [the protocol specification](protocol.md). PING/PONG, registration, and
heartbeat handlers support partial headers and the fixed worker ID payload; see
[the networking walkthrough](networking.md). The coordinator's
[worker registry](workers.md) owns IDs independently of reusable socket descriptors.
The shared message representation supports bounded variable job payloads.
Coordinator transport buffers now fit 1062-byte frames. Runtime handlers accept
submissions and worker reports, and workers receive assignments with partial-frame
buffering while keeping heartbeats active.

Workers use one task pthread while the main thread owns the socket and heartbeats.
Atomic completion/cancellation flags coordinate the two threads; shutdown joins
active work. The [durability contract](durability.md) defines persisted IDs and
counters, append/sync/publication ordering, logical job time across restarts,
and treatment of incomplete trailing records. The [WAL format](wal-format.md)
now encodes those decisions with versioned headers, full job snapshots, sequence
numbers, and header/payload checksums. The [WAL writer](wal-writer.md) creates and
locks new logs, fully writes each encoded record, and syncs before reporting
success. Storage failures permanently disable its handle. [WAL replay](wal-replay.md)
validates complete histories, rebuilds jobs/results/retries/FIFO and ID counters,
repairs incomplete final records, and resumes the same locked writer after sync.
The [coordinator store](persistence.md) prepares changes on a scratch scheduler,
commits them through this writer, then publishes. Startup recovery precedes the
listening socket, and fatal storage errors stop all further work and WAL appends.

## Evidence required for v0.1

Demonstrate multiple workers completing independent jobs, heartbeat-based failure
detection, reassignment after killing a busy worker, and state recovery after
restarting the coordinator. Unit tests, integration tests, controlled failure
experiments, and worker churn tests should show that no accepted durable job
silently disappears. Benchmark 1, 2, 4, and 8 workers and report the workload,
machine specifications, throughput, latency, and recovery costs.
