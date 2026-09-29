# Runtime logging

Coordinator and worker runtime records use the shared logger in
`include/log.h` and `src/common/log.c`. CLI command output and argument/help
text retain their command-specific formats.

## Record format

Each complete runtime record is one line:

```text
2026-09-28T12:00:00.125Z [WARN] coordinator job_worker_lost job_id=7 state=QUEUED worker_id=0 attempt=1 retry_count=1 pending=1 result_bytes=0 task=sleep max_retries=2 failure=WORKER_LOST outcome=REQUEUED previous_worker_id=3 durable=1 wal_sequence=18 pid=4200 monotonic_ms=9100125
```

The date, IDs, and times above are illustrative.

- Timestamp: UTC calendar time with millisecond precision, from `CLOCK_REALTIME`.
- Level: INFO for ordinary activity and recovery snapshots; WARN for failed
  attempts, worker unavailability, and rejected clients/reports; ERROR for
  process/storage/internal failures. A worker that cannot continue reports ERROR.
- Component and event: stable text identifiers such as `coordinator job_completed`.
- Fields: explicit `key=value` metadata; text containing spaces is quoted.
- `pid`: OS process ID for correlating records within one process lifetime.
- `monotonic_ms`: local monotonic clock at log emission, useful for elapsed times
  within that process lifetime.

PIDs are host-local and reusable. Worker identity remains the coordinator-issued
worker ID, and an execution lease is still identified by job, worker, and attempt.

Calendar clocks can change and different hosts may disagree. Do not infer
cross-host elapsed time or exact causal order from UTC stamps. Compare monotonic
values within one process lifetime, and use WAL sequences for durable ordering.
Logging clocks do not drive scheduling, heartbeat expiry, or persisted job time.

If the calendar clock cannot be read/formatted, the prefix is `time=unavailable`.
If the monotonic clock cannot be represented, the field is `monotonic_ms=-1`.
The logger never fabricates a valid timestamp in those cases.

Lifecycle records go to stdout, including WARN worker-loss/attempt outcomes.
Diagnostics go to stderr, including protocol/registration/submission warnings.
Level and stream are separate choices. To capture both streams together:

```sh
./build/debug/faultline-coordinator --wal faultline.wal > coordinator.log 2>&1
./build/debug/faultline-worker > worker.log 2>&1
```

These commands require the existing log for the coordinator. Use `--init-wal`
only when creating a new log.

## Durable job events

Job records include:

| Field | Meaning |
| --- | --- |
| `job_id`, `worker_id`, `attempt` | Published job identity, current/historical owner, and attempt. Queued jobs have worker ID zero. |
| `task`, `state` | Task name and current job state. |
| `retry_count`, `max_retries` | Consumed retry allowance and configured limit. |
| `failure` | NONE, TASK, or WORKER_LOST. |
| `outcome` | The meaning of this event, listed below. |
| `previous_worker_id` | Owner whose task failure or disconnect triggered this live failure event; zero for other events. |
| `pending` | Coordinator FIFO length at this event. |
| `result_bytes`, `result` | Original result length; escaped result text only for DONE. |
| `durable=1` | The displayed job state was published after the required WAL synchronization. |
| `wal_sequence` | The coordinator's synced WAL sequence at this event. |

The log line itself is not durable. `durable=1` describes job state in the WAL.

| Event | Outcome | What it proves |
| --- | --- | --- |
| `job_submitted` | ACCEPTED | Job creation was durably accepted, even if the submit ACK is later lost. |
| `job_assigned` | ASSIGNED | Ownership/attempt was durably assigned. The worker may not have received the frame yet. |
| `job_started` | RUNNING | A matching STARTED report was durably accepted. This does not prove continuing task progress. |
| `job_completed` | COMPLETED | A matching result was durably accepted as DONE. |
| `job_failed` | REQUEUED or FAILED | A task-failure report was accepted. Inspect the outcome/state to distinguish another retry from terminal failure. |
| `job_worker_lost` | REQUEUED or FAILED | Connection/heartbeat loss produced a durable retry or terminal failure. |
| `job_recovered` | RESTORED | A retained job snapshot after replay and startup reconciliation. This is not a new completion/failure event. |

For live transitions, the sequence identifies the just-committed record.
For recovered snapshots, it is the final synced startup watermark; multiple
jobs can share it. It is not each recovered job's last individual record number.

`previous_worker_id` preserves the failed owner's identity after requeue clears
`worker_id`. A retry keeps the old attempt number until its next assignment.
This makes worker-loss and task-failure histories readable without mistaking a
queued retry for a fresh job.

A syntactically valid but unauthorized/outdated report produces
`job_report_rejected`, with its claimed job/worker/attempt and the current
job's worker/attempt/state (or NOT_FOUND). It does not announce acceptance.
Under the existing protocol policy, closing the sender can separately trigger
worker-loss handling for a different job that sender actually owns.

## Worker execution and local sends

Worker `job_assigned` means the worker received and validated the assignment.
It is distinct from the coordinator's durable assignment event.

`job_started_sent`, `job_completed_sent`, and `job_failed_sent` carry
`acceptance=unconfirmed`. A complete local socket send does not prove the
coordinator received, accepted, or persisted a report; result acknowledgments
are not part of the protocol.

The worker sends STARTED before creating its task thread. `task_thread_started`
means thread creation succeeded; it still does not measure CPU progress.
If thread creation fails, the worker reports a task failure or exits according
to its existing cleanup rules.

`task_joined` with `cancel_requested=1 terminal_report_sent=0` records shutdown
cleanup for retained task ownership. It means cancellation was requested and
the thread was joined, not that a result or cancellation was durably accepted.
A task might already have finished locally before cleanup requested cancellation.

`heartbeat_sent`, `pong_sent`, and `worker_register_ack_sent` likewise describe
local sends. A coordinator `heartbeat_received` is the evidence that a heartbeat
was processed. Neither proves task progress.

## Failure, recovery, and shutdown

`heartbeat_timeout` retains timeout, silence, and detection-time fields.
The following `worker_dead` includes the close reason; `client_closed` explains
why any admitted connection was closed, including malformed headers and
wrong-direction messages.

`wal_ready` reports the quoted WAL path, synced sequence, recovered job/queue
counts, interrupted attempts reconciled, and incomplete-tail bytes repaired.
Recovered jobs then appear as RESTORED snapshots before the listener serves work.

`shutdown` reports its reason, signal number, and count of active durable
attempts. `policy=reconcile_on_restart` makes clear that stopping the coordinator
does not declare all its jobs finished or append synthetic loss transitions.
SIGKILL cannot emit shutdown records. `stopped` includes the actual exit code.

`system_error` diagnostics include the operation, numeric errno/pthread code,
and readable message. Persistence failures retain their operation, error offset,
format/history errors, and writer/replay codes. A storage failure still stops
further WAL mutations.

## Escaping and output limits

Result bytes and WAL paths are quoted and escaped. Printable ASCII is copied
except quotes and backslashes; other bytes use lowercase `\xHH`. Newlines,
NULs, terminal escapes, quotes, backslashes, and non-ASCII bytes therefore cannot
create extra records or inject terminal controls.

For example, the bytes `41 00 0a 22 5c ff` appear as
`result="A\x00\x0a\x22\x5c\xff"`. `result_bytes` remains 6.
Escaping uses explicit lengths and verifies capacity before touching output.
Result storage accommodates the complete 1024-byte maximum, even if every byte
expands to four characters. An unexpectedly oversized WAL path is displayed as
`<omitted>` rather than partially printed.

The logger locks its FILE for the complete record, appends a newline, flushes
stdio, and preserves the caller's errno. This does not make writes from separate
processes sharing a destination atomic, and flushing stdout is not WAL fsync.
Output errors are best effort and do not change accepted job state. A blocked
output destination can delay the event loop. There is no asynchronous sink,
rotation, filtering, remote export, or reliable audit-log guarantee.

## Verification

```sh
make test-logs
make test-observability INTEGRATION_ARGS='--port 9000'
make SANITIZE=1 test-observability
```

Three C groups verify escaping/capacity, UTC/PID/monotonic metadata, errno
preservation, and output failure. Six process scenarios compare command snapshots,
WAL records, and lifecycle logs through task execution, retries, terminal failure,
heartbeat expiry, rejected reports, cancellation, and coordinator crash/restart.
See the [phase review](observability-review.md) for the complete acceptance record.
