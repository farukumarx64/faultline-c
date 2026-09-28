# Persistence phase review

Review date: 2026-09-24. Runtime and tests reviewed at commit
`8f6ec3b8cc0414d3cadf55e50e7a060c73ceefed` on macOS arm64, with Apple Clang 21.0.0
and Python 3.9.6. This review changes documentation only.

The persistence phase's exit criterion is met under the process-crash contract:
kill and restart the coordinator without losing acknowledged jobs. The
implementation provides an append-only WAL, durable transition ordering, replay,
and recovery of interrupted attempts. Both full regression runs passed; no
runtime or test-code fixes were needed. This completes the persistence phase,
not the entire MVP or the later testing/chaos and benchmarking phases.

## The recovery promise

After a client receives a valid `JOB_SUBMIT_ACK`, the job's identity, definition,
retry allowance, and latest recoverable state survive a coordinator process
crash when the same retained WAL is reopened. A saved DONE or FAILED outcome
stays terminal. Unfinished active attempts are resolved under the existing retry
policy before service resumes.

This promise assumes the WAL remains available without external alteration and
the storage system honors successful writes and synchronization calls. It applies
to one coordinator and one retained log history. Losing, deleting, replacing, or rolling
back that history falls outside the promise. A log copy used by another
coordinator does not create a globally coordinated identity namespace.

## What is durable

| Retained information | Recovered meaning |
| --- | --- |
| Job identity and definition | Original job ID, task type, exact argument bytes/length, and maximum retries. |
| Latest job state | QUEUED, ASSIGNED, RUNNING, DONE, or FAILED as established by the valid history; startup resolves active states before listening. |
| Results and failures | Exact binary DONE result and terminal metadata; failure reason for a failed/requeued attempt. |
| Attempt and retry accounting | Latest attempt number and retries already consumed; historical owner where the state retains one. Requeueing clears its live owner. |
| Pending order | FIFO order from creation/requeue records. Existing queued jobs precede retries added during startup. |
| Identity allocation | Job and worker allocation high-water marks, including terminal jobs and idle-worker registrations. Allocators never wrap or reuse recovered identities. |
| Time and log ordering | Recorded job timestamps and unset markers, plus a contiguous WAL sequence. The logical job timeline continues across sessions; it excludes coordinator downtime and is not calendar time. |

Durability does not restore worker sockets, heartbeat deadlines, partial network
messages, threads, or a task's execution position. The worker registry starts
empty; fresh registration issues a new ID. A terminal job's saved worker ID is
historical metadata, not permission to submit new reports.

## When acceptance becomes durable

All durable transitions follow the same order:

```text
Validate and prepare → append complete WAL record → fsync
    → publish the prepared state → expose ACK/assignment/accepted outcome
```

A submission ACK confirms recoverable acceptance, not completion. Assignment
records are synced before their messages are sent. Completion records, including
the full result, are synced before completion is published or the worker is
given subsequent work. Initialization also syncs the file header and parent
directory before service begins.

There are two distinct uncertainties:

- **Missing submission ACK:** the coordinator may already have saved the job.
  Retrying the CLI submission can create a second job ID; no client request
  deduplication key exists. A client disconnect does not cancel an accepted job.
- **Worker finished or sent its result:** that alone does not prove durable
  acceptance. The worker protocol has no completion ACK. If no valid completion
  record survives, startup can still recover an active attempt and retry it.

Replay accepts the complete valid prefix, including a record whose previous sync
or ACK was unconfirmed. The WAL has no marker proving that an old fsync returned
or a peer received a message. Recovery syncs the surviving prefix before use.

## What can execute again

| State recovered from the WAL | Startup behavior | Can the coordinator dispatch it? |
| --- | --- | --- |
| QUEUED | Keep the job and FIFO position; consume no additional retry. | Yes, its pending attempt. This includes a queued job whose retry count already equals its allowance. |
| ASSIGNED or RUNNING, retry available | Save WORKER_LOST and QUEUED, charge one retry, clear owner and attempt timestamps, and append to the FIFO tail. | Yes, under a fresh worker and next attempt number. |
| ASSIGNED or RUNNING, allowance exhausted | Save terminal FAILED with WORKER_LOST and retain the final attempt metadata. | No. |
| DONE | Preserve the exact result and terminal metadata. | No new attempt. |
| FAILED | Preserve the failure and terminal metadata. | No new attempt. |

A retry runs the task from its beginning using the saved arguments. There is no
checkpoint or instruction-level resumption. A synced assignment counts as an
attempt even if its message was never sent, so repeated crashes can exhaust a
job's budget before it begins computing. A queued zero-retry job still gets its
original attempt; an interrupted active zero-retry job becomes FAILED.

For two permitted retries, interrupted attempts 1 and 2 each requeue once;
interruption of attempt 3 produces FAILED. Restarting an already queued job
does not spend another retry. Crashing during reconciliation also cannot charge
the same interrupted attempt twice: complete recovery records are replayed,
while an incomplete outcome is derived again from the prior active snapshot.
An orderly coordinator shutdown uses the same active-attempt recovery policy.

Execution uses **bounded at-least-once retry semantics**. An old task may have
finished before its result was saved, or may still be computing while a retry
starts. Old-attempt checks protect accepted coordinator state; they cannot undo
external side effects or force every old worker to stop. Even a terminal record
does not stop an already-running obsolete computation. Tasks with effects need
their own idempotency mechanism. Finite retries and unavailable workers mean
there is no unconditional promise of eventual execution or successful completion.

## Failure handling and exclusions

Only a structurally incomplete final record after a valid prefix is repaired
automatically. A complete invalid record, checksum failure, impossible history,
or unsupported format stops startup. Recovery never skips damaged records or
silently starts an empty store. Missing logs and competing writers also refuse
startup. This detects corruption; it does not restore damaged acknowledged data.

An unrecoverable write/sync/truncation error stops admission and scheduling and
causes an unsuccessful exit. Failed commits are not published, and shutdown
does not attempt more job-loss writes. An uncertain complete record may still
be adopted on restart. Startup enables live operations and opens the listener
only after replay, repair, synchronization, and reconciliation all succeed.

| Outside this phase's guarantee | Practical consequence |
| --- | --- |
| Universal OS-crash/power-loss survival, storage loss, corrupted or rolled-back WALs | The verified failure model is coordinator process crash with retained, working local storage; fsync is used but hardware/power failure is not simulated. |
| Exactly-once execution and external effects | A task or effect can occur more than once; attempt identity validation protects coordinator records only. |
| Submission deduplication and worker completion acknowledgments | Lost responses can leave clients/workers uncertain about acceptance. |
| Automatic coordinator failover, replication, and worker reconnect | An operator restarts the coordinator and launches workers with fresh connections; there is no consensus or replicated fallback. |
| Task checkpoints, guaranteed progress, or eventual success | Retries begin again, and finite allowances can end in FAILED. Heartbeats show liveness, not task progress. |
| Unlimited retention and bounded recovery time | The 256-job limit includes terminal jobs. Restart frees no slots; there is no compaction/eviction, and the append-only log can grow. |
| Throughput/latency promises and complete operational observability | Synchronous fsync can delay the event loop. Logical timestamps exclude downtime; status/jobs/workers/stats commands and metrics are the next planned phase. |

## Verification record

The full regression commands for this review use port 9000 so the CLI and worker
default-endpoint checks also run. The three persistence suites independently use
temporary WALs and available loopback ports. The two builds run sequentially so
they do not compete for port 9000.

| Command | Result |
| --- | --- |
| `make test INTEGRATION_ARGS='--port 9000'` | 107 C groups and all 98 integration scenarios passed; zero skips. |
| `make SANITIZE=1 test INTEGRATION_ARGS='--port 9000'` | The same 107 C groups and all 98 integration scenarios passed with AddressSanitizer/UBSan; zero skips and no sanitizer diagnostics. |

Each full run includes all 21 persistence/startup/crash test methods. The 47 crash
cases below are contained in ten of those methods, not additional integration
scenarios. The 27 state/budget histories are contained in a C store test group.

| Guarantee reviewed | Existing evidence |
| --- | --- |
| Durable admission, dispatch, and result publication | Seven [store test groups](../tests/test_coordinator_store.c) inspect live state during writes/syncs and inject failures across nine mutation variants. |
| Recover acknowledged jobs and exact terminal results | [Persistence](../tests/integration/test_persistence.py), [startup](../tests/integration/test_startup_recovery.py), and [crash](../tests/integration/test_coordinator_crashes.py) suites compare saved fields/results and execute work after restart. |
| Retry limits, FIFO, identities, and fresh connections | The store's 27 state/budget histories, mixed startup histories, repeated coordinator crashes, and reused-endpoint checks. |
| Writes interrupted before/after durable acceptance | The [crash matrix](coordinator-crashes.md): 35 live-operation cases, ten interrupted-startup cases, and two real CLI/worker scenarios, grouped into ten test methods. |
| Partial I/O, synchronization, locking, and storage errors | Eleven [writer groups](../tests/test_wal_writer.c) and twelve [replay groups](../tests/test_wal_replay.c), plus process-level file-size-limit failures. |
| Corruption/history validation and tail repair | Ten [format groups](../tests/test_wal.c) and the replay suite, including every incomplete prefix of a maximum-sized record. |
| Old-attempt protection and continued worker recovery | Scheduler/job tests and the six [worker-recovery scenarios](../tests/integration/test_recovery.py), rerun as part of the full regression. |

Passing these checks establishes the tested process-crash contract. It is not
an exhaustive kill at every instruction or byte offset, a multi-host partition
test, a power-loss experiment, a Linux validation run, or a performance benchmark.
Linux CI and broader chaos testing remain later roadmap work.

## Restart and phase handoff

Restart the coordinator with the same `--wal PATH` and omit `--init-wal`.
The relative default `faultline.wal` is resolved from the current working
directory, so use the same directory or an explicit absolute path. Wait for
successful recovery/listening, then launch workers to register again. Existing
eligible work is dispatched automatically; do not resubmit acknowledged jobs
just because the coordinator restarted.

The following phase is **CLI and observability**: status, jobs, workers, and
stats commands, clearer logs, and basic metrics. At persistence phase completion,
the CLI exposed ping and submission. The [status command](status.md) has since
added read-only access to saved state and results, including recovered jobs.
The [jobs/workers listings](listings.md) now expose retained job summaries and
the current worker registry. [Stats](stats.md) adds retained job totals, current
gauges, and session measurements with explicit restart scopes.
The verification record above remains the historical
persistence-phase result.
[Request deduplication](request-deduplication.md) is recorded
as an optional later enhancement, with proposed semantics, open decisions, and
acceptance checks; it is not an unfinished persistence requirement.
