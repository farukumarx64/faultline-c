# Tests

Run all tests from the project root:

```sh
make test
make test-sanitize
```

These run 131 protocol, registry, job, queue, scheduler, task, socket, logging, and WAL C test groups plus 143 process integration
tests using Python 3's standard library. A loopback-capable environment is
required. You can select the Python interpreter with `PYTHON=/path/to/python3`.
Use `make test-unit` or `make test-integration` to run one layer separately.
The [Linux CI workflow](../.github/workflows/linux-ci.yml) runs both layers on
Ubuntu 24.04 with GCC 13 and with Clang 18 + AddressSanitizer/UBSan. Both jobs
enable the default-port checks and reject compiler warnings. Available build/test
logs and chaos evidence are uploaded after success or failure. See
[Linux CI](../docs/ci.md) for reproduction commands and validation scope.
Use `make test-batch` for the [batch baseline harness](../docs/batch-testing.md):
five workers, 100 three-second sleep jobs, exact per-ID results, artifacts, and
bounded cleanup. `BATCH_ARGS='--workers 3 --jobs 9 --sleep-ms 25'` selects a small
run. `make test-batch-harness` checks the harness's success and failure paths.
Both accept `SANITIZE=1`; both remain outside `make test` and the suite counts
above. Use `make test-chaos` for the [seeded crash/replacement experiment](../docs/chaos-testing.md)
and `make test-chaos-harness` for its 35 regression checks, including per-ID
terminal accounting, exact results, retry histories, and drain failures.
Both accept `SANITIZE=1`;
`CHAOS_ARGS` configures the experiment. These targets also remain outside
the default suite. CI explicitly runs the 21 baseline harness checks, the 35 chaos
harness checks, and one full seed-42 experiment per compiler, with deadlines and
evidence uploads. The full no-fault `test-batch` experiment stays manual.
`make test-ci-deadlines` runs seven Linux-only checks of the shared CI runner,
including a command that exits zero on timeout, forced termination, and log-write
failure. It requires GNU coreutils and runs before the harness suites in CI.
The three newest chaos fixtures verify exit 1 for a wrong same-length result,
excess retries, and cleanup failure after valid accounting and recovery. See
[CI rejection conditions](../docs/ci.md#conditions-that-fail-ci).
Completed process fixtures also verify evidence retention on success and failure:
manifest/seed, summary and final trace agreement, every process log, and a WAL
after acknowledgment. Harness stdout/stderr now use `.log` names so CI includes
early diagnostics. The additional baseline check injects a JSON publication error
and requires both the prior artifact and unpublished `.json.tmp` to survive.
The [contract](../docs/chaos.md) defines recovery coverage.
The [phase review](../docs/chaos-review.md) records the three-seed normal/sanitizer
matrix, harness regression results, reproducible commands, and evidence limits.
The [Linux harness verification](../docs/chaos-testing.md#linux-harness-regression-verification)
records both suites passing with GCC and Clang sanitizers in local Ubuntu ARM64
containers, with leak detection enabled. The subsequent
[CI integration validation](../docs/ci.md#chaos-ci-integration-validation) checks
the new workflow commands. The later
[GitHub-hosted validation](../docs/ci.md#github-hosted-linux-validation) passed
the complete workflow on Ubuntu x86-64 in both builds and verified all four
uploaded artifacts after downloading them.
The [full Linux seed-42 experiment](../docs/chaos-testing.md#linux-fixed-seed-experiment-verification)
also passed in both builds, with all 100 jobs completed after worker crashes.
Use `make test-job-status-protocol` for status codecs, or
`make SANITIZE=1 test-job-status-protocol` for instrumented binaries.
Use `make test-status` for the live status command, or `make SANITIZE=1 test-status`
for instrumented binaries. `INTEGRATION_ARGS='--port 9000'` also checks its default endpoint.
Use `make test-list-protocol` for listing codecs and `make test-listings` for
live job/worker inspection. Both accept `SANITIZE=1`; the process target accepts
`INTEGRATION_ARGS='--port 9000'` to check both commands' default endpoint.
Use `make test-stats-protocol` for statistics codecs/aggregation, and `make test-stats`
for live counts, restart scopes, and CLI behavior. Both accept `SANITIZE=1`;
the latter accepts `INTEGRATION_ARGS='--port 9000'` for its default endpoint.
Use `make test-logs` for the shared logger and `make test-observability` for
command/log/WAL agreement through execution, failures, and restart. Both accept
`SANITIZE=1`; the process target also accepts `INTEGRATION_ARGS='--port 9000'`.
The [CLI and observability review](../docs/observability-review.md#verification-record)
records the current full regression and the interpretation limits of each command.
Use `make test-wal` for the WAL format suite, or `make SANITIZE=1 test-wal`
for the same checks with AddressSanitizer/UBSan.
Use `make test-wal-writer` for the file writer suite, or
`make SANITIZE=1 test-wal-writer` for instrumented binaries.
Use `make test-wal-replay` for existing-file recovery, or
`make SANITIZE=1 test-wal-replay` for instrumented binaries.
Use `make test-persistence` for durable coordinator transactions and restart
scenarios, or `make SANITIZE=1 test-persistence` for instrumented binaries.
Use `make test-startup-recovery` for focused recovery checks, or add `SANITIZE=1`
for instrumented binaries. `test-persistence` includes that process suite too.
Use `make test-coordinator-crashes` for controlled SIGKILL boundaries and real
CLI/worker recovery; add `SANITIZE=1` for instrumentation. This suite is also
included in `test-persistence` and `test-integration`.
Use `make test-recovery` for SIGKILL, SIGSTOP/heartbeat recovery, resumed-worker
old-attempt protection, and retry exhaustion, or `make SANITIZE=1 test-recovery`
for instrumented binaries.
The [phase review](../docs/recovery.md#phase-review) records the recovery outcomes,
supporting test coverage, and the limits of those checks.
The [persistence phase review](../docs/persistence-review.md#verification-record)
records the full normal/sanitizer regression with default-endpoint checks enabled,
and maps durable admission, replay, retry accounting, and crash recovery to tests.
Use `make test-execution` for built-in results, heartbeats during computation,
worker reuse, and cancellation, or `make SANITIZE=1 test-execution` for instrumented binaries.
Use `make test-scheduling` for CLI submission and scheduling, or
`make SANITIZE=1 test-scheduling` for instrumented binaries.
Use `make test-failures` for the dedicated failure-detection suite, or
`make SANITIZE=1 test-failures` for the same checks against instrumented binaries.

`test_protocol.c` has seven test groups:

- Exact encoded and decoded bytes for PING and PONG, using literal reference
  headers so matching bugs in both functions cannot hide behind a round trip.
- Payload-length round trips at byte boundaries and the exact 1 MiB maximum.
- Every incomplete header size from 0 through 11 bytes, checking that output
  stays unchanged. Truncated decode inputs use exact-sized allocations so ASan
  can detect out-of-bounds reads.
- Malformed magic, unsupported versions, unknown message types, excessive
  payload lengths, and a header encoded in the wrong byte order.
- Invalid encoder field values, including maximum integer values, without
  modifying the destination buffer.
- Null argument handling.
- Header reads and writes at an unaligned address, preservation of surrounding
  bytes, and acceptance of buffers with trailing data.

The test program exits unsuccessfully at the first failed check and reports the
test, source line, and expression. Its checks remain active with `NDEBUG` set.

`test_messages.c` adds seven complete-message test groups:

- Literal complete frames for PING, PONG, WORKER_REGISTER, WORKER_REGISTER_ACK,
  and HEARTBEAT, including exact payload lengths, unaligned buffers, and guards.
- Worker IDs 1, 12, `0x01020304`, and `UINT32_MAX` in ACK and HEARTBEAT payloads.
- Every incomplete prefix of each frame and every insufficient output capacity,
  including 12 through 15 bytes of messages carrying a 4-byte ID. Truncated inputs
  use exact-sized allocations for ASan; failed calls preserve outputs and counts.
- Wrong declared payload lengths, oversized payloads, zero IDs, and invalid
  headers. Wrong lengths fail before waiting for any payload bytes.
- Invalid encoder message types and IDs without partial writes.
- Null required pointers with unchanged remaining outputs.
- Consecutive registration and ACK frames followed by a partial heartbeat,
  checking byte-consumption counts and decoding again as the remaining ID bytes
  arrive. This is a buffer-level test, not a live worker registration exchange.

`test_job_messages.c` adds eight job codec test groups:

- Literal complete frames for all six job types, including unaligned 64-bit
  fields, exact lengths, and untouched surrounding bytes.
- Every incomplete prefix and insufficient capacity for each sample frame and
  the maximum 1062-byte assignment; exact allocations expose overreads to ASan.
- Empty, 1/255/256/1024-byte binary data, embedded zeroes, decoded ownership after
  receive-buffer reuse, and rejection of 1025-byte/`SIZE_MAX` host lengths.
- All task IDs, retry limits zero/`UINT32_MAX`, job/worker ID boundaries, and
  64-bit attempts above `UINT32_MAX` through `UINT64_MAX`.
- Zero identity fields, unknown tasks, and invalid failure codes, including a
  worker trying to report the coordinator-only WORKER_LOST reason.
- Too-short/too-large outer sizes and inconsistent or oversized inner lengths,
  rejected from the available prefix without waiting for data.
- Coalesced job messages and a heartbeat followed by a fragmented final report.
- A decoded STARTED report accepted by the model, then rejected as stale after
  reassignment to the same worker under a newer attempt number.

Rejected codec calls preserve all output bytes and written/consumed counts.
These are buffer/model tests; separate process tests cover live execution. The
binary also links the job model for the final identity check.

`test_job_status.c` adds seven [status codec](../docs/job-status-protocol.md) groups:

- Literal request/not-found frames and responses for all states, including fresh
  and retried QUEUED, with unaligned buffers and independent expected bytes.
- Snapshots of real model transitions through assignment, execution, completion,
  task/worker loss, requeueing, and exhausted retry budgets.
- Maximum IDs/counters, attempt `2^32`, empty and 1/255/256/1023/1024-byte binary
  results, and result ownership after overwriting the receive buffer.
- Every incomplete prefix and insufficient capacity for all examples and the
  maximum 1072-byte status frame, using exact-sized allocations for ASan.
- Invalid state/owner/attempt/retry/failure/result combinations, on encode and
  decode, with invalid metadata rejected before missing result bytes arrive.
- Invalid outer/inner lengths, `SIZE_MAX`, null pointers, and unchanged outputs.
- Mixed PING/status/not-found/request frames with exact consumption and a final
  request completed in fragments.

These C checks exercise codecs and the model. The process suite below covers
live status queries; coordinator rejection tests retain outbound-only replies.

`integration/test_status.py` adds fifteen [CLI status](../docs/status.md) scenarios:

- Queued/unknown lookup, default endpoint when port 9000 is selected, exact output
  and exit codes, unchanged WAL bytes, and preserved job/worker ID allocation.
- Assigned/running/done snapshots, a maximum binary result, escaping, and the last
  worker's identity after disconnection.
- Retried TASK failure and terminal TASK/WORKER_LOST, including the final allowance.
- Worker-loss requeue followed by an empty successful result.
- Every split of a request, coalesced query/not-found/PING frames, and submission
  and queries on the same connection with serialized replies.
- Invalid/truncated requests, worker query rejection, and preventing a querying
  connection (including NOT_FOUND) from becoming a worker.
- Queries with all 256 job slots full, preserving WAL bytes and FIFO dispatch.
- Real workers producing all four built-in results and invalid-input failure.
- Repeated queries while a silent worker's heartbeat expires independently.
- CLI lookup of replayed DONE/FAILED/QUEUED jobs and a reconciled interrupted job
  after coordinator SIGKILL, without query-induced WAL changes.
- Full 64-bit job IDs/attempts and fragmented maximum/not-found replies.
- Rejected arguments and ID overflow before connection attempts.
- Malformed headers/prefixes and mismatched reply IDs, rejected without output.
- Truncated replies at header/prefix/result boundaries and connection refusal.
- One five-second response deadline shared by header, fixed prefix, and result.

`test_list_messages.c` adds seven [listing protocol](../docs/listings.md#verification) groups:

- Independent literal bytes for job/worker summaries, unaligned buffers and owned copies.
- Empty request/reply frames with valid zero counts.
- Maximum 256-job/64-worker lists, integer limits, every incomplete prefix of
  both maximum frames, and insufficient output capacities.
- All job states with valid retry/ownership combinations.
- Early rejection of count overflow, length mismatch, invalid timeout and null arguments.
- Invalid row fields, duplicate/descending IDs, and unchanged outputs on error.
- Mixed request/list streams with an incomplete trailing frame.

`integration/test_listings.py` adds thirteen live scenarios: empty/default-endpoint
views; mixed retained states and active worker jobs; retries/FIFO/WAL immutability;
heartbeat age and sorted slot reuse; 256 retained jobs and stable encoded snapshots;
fragmented/coalesced requests and connection roles; real worker busy/idle changes;
heartbeat expiry despite listing polls; recovered jobs and reset registry after a
restart; fragmented maximum replies and integer limits; malformed replies; invalid
options, truncation and refused connections; and one deadline per full response.

`test_stats.c` adds seven [statistics](../docs/stats.md#verification) groups:

- Independent literal bytes, unaligned buffers, empty counts, and owned snapshots.
- Every incomplete response prefix, short outputs, invalid lengths, and mixed streams.
- Invalid partitions, counts, durations, timeouts, and unchanged outputs on failure.
- Durable aggregates and session baselines after startup reconciliation.
- Maximum retained counts, wide attempt/retry totals, and overflow-safe latency means.
- Exact heartbeat expiry boundaries and separate busy/idle activity.
- Invalid time, store, and pointer inputs without output mutation.

`integration/test_stats.py` adds eleven scenarios: empty/default-endpoint queries,
all job states and worker activity, retries versus terminal failure, full-store
rejection, fragmented/coalesced requests and roles, expiry despite polling,
repeated crash recovery and session reset, fragmented large values, malformed
replies, CLI options/EOF/refused connections, and a whole-response deadline.

`test_log.c` adds three [logging](../docs/logging.md) groups:

- All 256 byte values, embedded NUL/control bytes, quotes/backslashes, capacity
  and size overflow, and unchanged output on rejected escaping calls.
- UTC millisecond timestamp, PID, monotonic metadata, errno preservation,
  and a record without additional fields.
- Output failure and invalid required arguments.

`integration/test_observability.py` adds six scenarios combining all four
inspection commands with independent CRC-checked WAL decoding and runtime logs:

- All job states, binary results, unknown IDs, rejected reports, and owner leases.
- Worker-loss/task-error requeueing, exhausted retries, and eventual completion.
- Mixed-state SIGKILL/restart, durable outcomes, fresh workers, and session resets.
- Escaped WAL paths, malformed clients, structured system errors, and failed startup.
- Heartbeat expiry while a peer's TCP connection stays open.
- Real execution, invalid input, interrupted sleep, local-send uncertainty, and
  orderly worker cancellation/join cleanup.

The suite compares exact states, identities, attempts, retries, results, worker
gauges, job counters, and logical completion latency with durable records at
stable observation points. Querying those snapshots leaves WAL bytes unchanged.
Recovery snapshots use RESTORED rather than announcing new terminal outcomes.

`test_wal.c` adds ten [WAL format](../docs/wal-format.md) groups:

- Literal file header and all seven record types, independently generated with
  Python `struct.pack` and `zlib.crc32`; unaligned buffers and trailing bytes.
- Every incomplete input prefix and insufficient output capacity, including the
  maximum 2164-byte record. Exact input allocations expose overreads to ASan.
- Every single-bit flip across the file header and seven record fixtures: 6216
  corruptions. Corrupted lengths fail from the fixed header alone.
- Bad magic, version, flags, type, sequence, and outer lengths with repaired
  header CRCs, proving field validation independently of checksum rejection.
- Invalid job IDs, enums, counters, times, reserved bytes, inner lengths, and
  state/record combinations with valid recalculated header/payload CRCs.
- Invalid encoder inputs, preserving all output bytes and the written count.
- Empty/maximum binary payloads; integer, retry/attempt, and timestamp boundaries,
  including the unset marker, equal times, and `INT64_MAX`.
- Consecutive records and expected-sequence checks for gaps, duplicates, ordering,
  zero, and maximum sequence values. This is not cross-record state replay.
- Snapshots from the existing model across all four tasks, budgets 0–2, failure
  before/after STARTED, both failure reasons, retries, success, and exhaustion.
- Null arguments, owned decoded payloads after input reuse, and exclusion of
  unused C array capacity from the serialized bytes.

Rejected calls preserve outputs and byte counts. The test-only reference CRC
uses an MSB-first calculation distinct from production's reflected calculation;
fixed valid fixtures do not depend on either C implementation. These tests do
not open a WAL file, sync storage, replay a history, or restart the coordinator.

`test_wal_writer.c` adds eleven [WAL writer](../docs/wal-writer.md) groups:

- Real files containing all seven record types, exact encoded bytes, maximum
  binary payloads, 17-byte partial writes, and record decoding. Verify the full
  file length at each sync and that sequence publication follows synchronization.
- Relative paths, existing-file preservation, symlinks/directories/FIFOs,
  missing parent directories, owner-only permissions, append and close-on-exec flags.
- Exclusive locking across separate opens and child processes; release on close.
- Every split of a 36-byte record with EINTR at the split, interrupted locks and
  syncs, and a header written one byte at a time. No bytes are skipped/duplicated.
- ENOSPC, EIO, EDQUOT, EFBIG, and zero progress at five append offsets. Preserve
  prior records and exact partial bytes; refuse further I/O after failure.
- A failed record sync with its complete bytes still present. Confirmed sequence
  stays unchanged; a failed sync is not proof that the record is absent.
- Lock, partial-header write, header-sync, and parent-directory-sync failures.
  No READY state, retained file prefix, and cleanup preserving the first error.
- Invalid arguments, malformed records, duplicate/out-of-order sequences, and
  invalid handle states without writes or syncs.
- Final sequence exhaustion without wrapping or additional I/O.
- File/directory close errors and an immediately reused descriptor, ensuring
  close is not retried and the original storage error is not overwritten.
- SIGKILL after a child successfully syncs an allocation record, before close.
  The parent validates surviving bytes and verifies the kernel released the lock.

Successful injected operations use real syscalls on private temporary files;
selected calls instead return controlled short counts or errors. No actual disk
is filled or disrupted. These tests exercise storage ordering and process-crash
survival, not power loss, existing-log replay, or coordinator restart recovery.

`test_wal_replay.c` adds twelve [WAL replay](../docs/wal-replay.md) groups:

- Empty histories, worker allocations, resumed appends, and replaying again.
- Mixed DONE/FAILED/QUEUED/ASSIGNED/RUNNING jobs, binary inputs/results, retries,
  exact timestamps, FIFO ordering, restored allocators, and an empty live worker
  registry. Repeated replay never increments counters or restores connections.
- Every incomplete prefix of a maximum 2164-byte record, repair to the exact
  preceding boundary, complete-record recovery, and appends after repair.
- Every incomplete file-header prefix plus complete invalid headers; no repair
  or implicit initialization is allowed.
- Complete bad record checksums, fields, lengths, versions, and sequences,
  including corruption before later valid records and a trailing partial record.
- Twenty-five checksummed but impossible histories: duplicate/decreasing IDs,
  uncreated jobs, wrong owners/attempts/counters/times, changed immutable fields,
  invalid transitions, unallocated workers, busy workers, and FIFO violations.
- Sparse historical worker allocations beyond registry capacity, range growth,
  old worker membership, maximum job/worker IDs and times, and exhaustion.
- A store filled with 256 terminal jobs; a 257th creation fails without repair
  even when followed by an incomplete tail.
- One-byte reads, EINTR, interrupted truncation, and publication only after file
  and directory sync. Appends are refused throughout RECOVERING.
- Read errors at nine offsets and truncate/file-sync/directory-sync failures.
  Caller state stays byte-for-byte unchanged; read errors never cause truncation;
  a completed truncate is not undone when later synchronization fails.
- Invalid arguments, missing files, symlinks, directories, FIFOs, competing
  handles, and unchanged output/file bytes on validation failure.
- SIGKILL after one synced record and seven bytes of the next append; recover,
  remove only those seven bytes, append again, and verify a second replay.

These checks use real private temporary files with controlled I/O faults where
needed. They verify standalone recovery and preserve active-job snapshots.
Startup reconciliation and coordinator restart/ACK ordering are exercised by
the separate tests below. These checks do not establish power-loss survival.

`test_coordinator_store.c` adds seven [transaction groups](../docs/persistence.md):

- Inspect published jobs, registry, output values, clock, and sequence during
  writes and syncs for registration, submission, assignment, STARTED, COMPLETED,
  and task/worker-loss outcomes with remaining or exhausted retries.
- Fail each of those nine mutations with a partial write or failed sync. Verify
  unchanged live state/outputs, no later mutations or cleanup writes, and correct
  subsequent recovery of a partial versus complete uncertain record.
- Reject invalid inputs, duplicate registration, busy/unknown workers, and stale
  reports without WAL I/O.
- Restore mixed queued/active jobs, reconcile in ID order, preserve FIFO and retry
  budgets on repeated startup, restore IDs, and continue a logical job clock from
  timestamps greater than the new raw clock.
- Fail the first or second reconciliation sync, then recover without charging a
  durable interruption twice. Public mutations are blocked during every startup
  sync; only complete success enables the store's live operations.
- Recover 27 histories across all five job states, retry budgets 0–2, and every
  applicable used-retry count. Compare all persisted fields and binary results
  across two restarts; verify empty connections/heartbeats, fresh worker identity
  despite descriptor reuse, preserved pending attempts, and terminal retention.
- Stop on job-clock overflow/regression and WAL sequence exhaustion without
  publishing or writing more state.

`integration/test_persistence.py` adds seven process scenarios:

- SIGKILL after submission ACK and accepted binary completion; independently
  decode WAL records and verify completion precedes the next assignment.
- Restart mixed queued/RUNNING/ASSIGNED jobs, preserve FIFO and budgets, avoid
  repeated retry increments, and complete the recovered attempts.
- Enforce a child-process file-size limit at nine live write boundaries: no
  success publication for the failed transition, no extra cleanup records, and
  safe repair/restart afterward. Submission ACK delivery can remain uncertain
  when a later assignment fails before an already-queued ACK is transmitted.
- Fail reconciliation writing and require startup refusal before listening.
- Reject missing, already-existing initialization targets, competing writers,
  and corrupt logs; repair an incomplete final record only.
- Retain idle-worker allocations and reconcile work after graceful shutdown.
- Exercise the default WAL path, recovery mode, help, and invalid options.

The file limits affect private temporary WALs only; diagnostics use pipes. The
coordinator has no production fault-injection switches. Existing process fixtures
now explicitly create a temporary WAL per coordinator. Neither test layer
simulates power loss or exhaustively kills the coordinator at every instruction.

`integration/test_startup_recovery.py` adds four focused process scenarios:

- Recover a ten-job mixed history, preserving DONE/FAILED/QUEUED exactly while
  reconciling ASSIGNED and RUNNING at zero/remaining/exhausted retry allowances.
  Verify six new outcomes in job-ID order, no changes on an extra restart, and
  subsequent FIFO dispatch of existing queued jobs before recovered retries.
- Restart at the same host/port; old sockets cannot be restored, and old worker
  heartbeats/results on unregistered connections change no WAL bytes. Fresh
  registration gets a new ID and completes the next attempt.
- Repeated SIGKILL during ASSIGNED exhausts two retries at attempt 3. Extra
  restarts before assignment consume no retry. New work still executes afterward.
- Repeat the same coordinator-crash exhaustion check for RUNNING jobs.

The startup suite shares the persistence fixture and independent Python WAL
decoder. It uses controlled worker peers and ephemeral initial ports, reusing
each coordinator's exact endpoint on restart. `test-integration` runs these suites
once; `test-startup-recovery` selects these four scenarios plus the C store suite.

`integration/test_coordinator_crashes.py` adds ten test methods (47 cases):

- Five boundaries for each of seven WAL record types: before writing, after a
  partial header, after a partial payload, before fsync, and after successful
  fsync before publication. Verify no premature ACK/assignment/event log,
  preserve prior terminal snapshots, repair incomplete suffixes, retain complete
  valid records, restore counters, and complete eligible work with a fresh peer.
- Ten cases interrupting either the first or second startup requeue at those
  boundaries. No listener opens early; each retryable interruption consumes one retry;
  existing pending work keeps its place ahead of recovered retries.
- Submit all four built-in tasks through the real CLI, crash after ACKs, restart,
  and execute them with a real worker; crash again and retain terminal outcomes.
- Crash during a real running sleep, restart, and complete its next attempt on
  a fresh worker. Preserve the result on another restart.

The separate `tests/crash-coordinator` binary compiles the production main with
only its store-open call redirected into `tests/crash_coordinator_io.c`. It uses
the existing I/O seam and normal production objects; `make all` does not build
it. Python kills the paused test process with SIGKILL, then always recovers with
the normal coordinator on the same endpoint/log. There are no production crash
switches. See the [crash guide](../docs/coordinator-crashes.md) for boundaries
and the distinction between process crashes and power loss.

`test_net.c` has six socket test groups and two parsing groups. Socket
tests use local stream socket pairs and child processes to verify fragmented
receives, clean EOF versus truncation,
one total receive deadline despite progress, a 256 KiB send through a constrained
send buffer, a stalled-send deadline, and handling a disconnected peer without
SIGPIPE terminating the process. The bulk-send receiver checks every byte
independently using raw `recv()` calls.

Endpoint tests cover numeric IPv4 addresses, port boundaries, malformed inputs,
unsupported hostname/IPv6 inputs, null arguments, and insufficient host buffers.
Failed parsing leaves the host and port outputs unchanged. Both the CLI and
worker use this shared parser.

Duration tests check positive decimal milliseconds, leading zeros, `INT_MAX`,
overflow, malformed input, null pointers, and unchanged output on failure.

`test_worker_registry.c` has seven groups covering registration and lookup,
heartbeat ownership and monotonic time, disconnect and descriptor reuse,
capacity/duplicate registration/churn, ID exhaustion, invalid arguments, and
heartbeat expiration. Expiry checks cover just before, exactly at, and after a
deadline, heartbeat renewal, dead/unused records, invalid inputs, a one-millisecond
timeout, and timestamps near `INT64_MAX` without overflowing an added deadline.
These tests supply descriptor numbers and timestamps directly; no real sockets
or sleeps are needed. Reusing descriptor 7 is deliberate and deterministic.
Stale IDs cannot update or kill its replacement worker. Churn exceeds the
64-slot capacity without replacing live records; exhaustion never wraps to ID 1.

`test_jobs.c` adds nine in-memory model test groups:

- All four task identifiers, initial fields, argument ownership, zero-length
  arguments, and exact maximum-length payloads.
- A successful assignment/start/completion lifecycle, timestamp updates, and
  result ownership including embedded zero bytes and maximum-sized results.
- Every pair of the five states, plus unknown states, against an explicit graph.
- Every named operation from every state with zero and nonzero retry allowance;
  includes rejected self-transitions and terminal-state changes.
- Requeue/reset semantics, retry followed by success, assignment back to the same
  worker, and rejection of old STARTED, COMPLETED, and failure reports.
- Zero retries, exhausted retries, and failure before the worker reports starting.
- Null pointers, zero IDs, unknown tasks/failure reasons, negative time, oversized
  arguments/results (including SIZE_MAX), and unchanged records on errors.
- Wrong worker IDs, wrong attempt numbers, reversed timestamps, and equal-time events.
- Maximum job/worker IDs and timestamps, and the final retry/assignment boundary
  beyond UINT32_MAX attempts, without billions of iterations or counter wrap.

These tests need no sockets or sleeps. They verify model-level transitions only;
runtime submission and retries are now covered by the scheduler tests below.
The job model links into the coordinator and the job/queue/job-message/scheduler/WAL test
binaries; the CLI and worker do not link its transition implementation.

`test_job_queue.c` adds seven FIFO test groups:

- Empty initialization, empty peek/pop, null arguments, and unchanged output IDs.
- Insertion order using unsorted IDs/timestamps and UINT64_MAX; push/pop leave
  the complete job records and their payloads unchanged.
- Repeated peeks while waiting and rejected assignments leave the front intact;
  successful model assignment followed by pop removes the intended ID.
- Duplicate IDs from the same or different records; rejection of assigned,
  running, done, failed, and malformed job records.
- Exactly 256 pending IDs; overflow preserves every existing entry and the
  proposed job; duplicate checking also works when full.
- Repeated half-drain/refill cycles, array wraparound, duplicate detection across
  the boundary, complete FIFO draining, and reuse after becoming empty.
- A model-level retry retains its ID and joins the back after explicit enqueue.

These tests use no sockets, sleeps, or scheduler. Most FIFO-only fixtures create
temporary job records to verify that the queue copies IDs and retains no pointers.
Application code must keep authoritative records in its own store. Queue code
links into the coordinator and its test binary; the queue tests also link the
job model to exercise assignment and retry ordering.

`test_scheduler.c` adds seven groups covering atomic store/queue acceptance,
unique IDs and owned arguments, immediate busy reservation, FIFO dispatch,
validated reports and result retention, worker release, retry ordering and old
attempt rejection, worker loss, full-store rejection with guaranteed retry room,
and ID exhaustion. Rejected operations preserve scheduler/output snapshots.

The old-report group injects attempt-1 STARTED, COMPLETED, and FAILED reports
while the job is QUEUED, ASSIGNED to attempt 2, RUNNING on attempt 2, or DONE.
It tests retries on the same worker after a task error and on a different worker
after worker loss. Each of the 24 rejected reports must preserve the full
scheduler snapshot, including the queue, timestamps, retries, and accepted result.
Old/new completion payloads differ so a result overwrite cannot pass silently.

`integration/test_scheduling.py` adds 15 scenarios, with a fresh coordinator per
test to isolate retained job records:

- Jobs accepted without workers, FIFO assignments to two workers, busy exclusion,
  and next-job dispatch after a controlled worker reports completion.
- Concurrent real CLI submissions receiving unique IDs in enqueue order.
- Fragmented/coalesced submissions with maximum-sized binary arguments.
- An incoming partial heartbeat preserved while a job waits for dispatch.
- Task failure/disconnect retry order and terminal failure after exhaustion.
- Spoofed worker IDs and stale reports cannot complete another/current attempt.
- Exactly 256 retained records, rejected overflow without ACK, and preserved older jobs.
- Real workers retain one assignment, keep heartbeating, and receive no second job.
- Assignment fragments do not suppress real-worker heartbeats.
- Invalid/truncated submissions consume no IDs; registered workers cannot submit.
- Heartbeat expiry on an open assigned connection reassigns to a healthy idle worker.
- Wrong assignment identity and partial-frame timeout cause worker failure exit.
- Independent peers verify CLI task/argument/retry bytes and fragmented 64-bit ACKs.
- CLI option validation and malformed/truncated/zero-ID ACK rejection.
- A single five-second CLI ACK deadline across header and payload.

Controlled peers exercise arbitrary reports; real workers now execute long sleep
tasks in the busy-worker scenarios.
The scheduler links only into the coordinator and its test binary.

`test_tasks.c` adds seven executor groups:

- Sleep returns the requested milliseconds and waits until its monotonic deadline.
- Known inclusive prime counts, including 1,000,000 producing 78,498.
- Fibonacci base cases, F(92), and the largest supported value F(93).
- Known FNV-1a vectors, empty input, and embedded zero bytes.
- Bad numeric syntax, per-task limits, overflow, null pointers, oversized data,
  and unchanged output after rejected calls.
- Pre-cancelled calls for all four tasks.
- Running sleep and prime-count tasks stop cooperatively after atomic cancellation.

The task implementation links only into the worker and its executor test binary.
Those binaries link POSIX threads; the coordinator remains single-threaded.

`integration/test_execution.py` adds 11 scenarios against fresh coordinators:

- Sleep completes and releases its worker for the next FIFO job.
- CLI submissions execute all four tasks and expose expected coordinator results.
- A controlled peer verifies literal STARTED/COMPLETED reports, maximum binary
  input, fragmented assignments, maximum IDs, and a 64-bit attempt number.
- Invalid task arguments fail while the worker remains available.
- Failed attempts retry at the queue tail and eventually exhaust their budget.
- CPU work keeps heartbeats active and stops promptly when cancelled.
- Two workers execute concurrently and drain queued work.
- An interrupted sleep is retried and completed by a replacement worker.
- Coordinator disconnect, SIGINT, and SIGTERM cancel long sleep and CPU tasks.
- Literal FAILED reports contain the TASK reason and do not prevent later success.
- Binary results are escaped in the coordinator log without changing raw bytes.

These tests use a 500 ms heartbeat timeout and 60–80 ms worker intervals. Sleep
and CPU tasks remain active beyond that timeout, so continuing heartbeats are
required to keep their assignments alive. Algorithm/input contracts are in
[the task guide](../docs/tasks.md).

`integration/test_recovery.py` contains six acceptance scenarios, each with a
fresh coordinator and real workers. In the hard-crash check, both register and heartbeat
before CLI submission. After a three-second sleep job starts and its owner sends
another heartbeat, the harness kills that owner with SIGKILL. It verifies prompt
transport-based detection, one requeue, the same job ID assigned to the connected
survivor under attempt 2, and the expected completed result. The survivor then
completes a new Fibonacci job and continues heartbeating; the CLI still gets PONG.

The complete original job transition sequence is checked for missing/duplicate
events, and the survivor keeps its original registration. The coordinator uses
its default six-second heartbeat timeout; attempt 2 must start within 2.5 seconds
of the kill. This is distinct from the existing graceful SIGTERM retry scenario
and idle-worker failure detection. See [the recovery guide](../docs/recovery.md).

The heartbeat recovery check uses SIGSTOP on the busy worker and confirms the
OS stopped state with `waitpid(WUNTRACED | WNOHANG)`. Before expiry, it verifies
that the worker remains present, its job remains RUNNING, and there is no timeout
or reassignment. The other worker and CLI remain responsive. Exactly one
heartbeat timeout and worker death must then occur, with at least 6000 ms and
less than 7500 ms of silence measured from the last accepted heartbeat.
The test checks timeout/death descriptor and timestamp consistency.

The connected survivor completes attempt 2 with the expected result, then a new
Fibonacci job. The original worker remains paused through both completions and
sends no terminal report. A finally block kills and reaps that child after the
checks or on failure; this cleanup cannot trigger a passing recovery. The test
does not resume the old attempt. The scenarios share log parsing, bounded
attempt-aware waits, and exact transition-sequence assertions in `RecoveryTestCase`.

Two additional scenarios resume the original worker with SIGCONT after expiry:
one while attempt 2 is RUNNING and one after it is DONE. They require the same old
process to exit with a connection error, without changing the original job's
event history or renewing its expired heartbeat. The survivor's identity, result,
heartbeats, and ability to complete follow-up work remain intact. Cleanup kills
and reaps the old child only if it is still present, including after a failed test.

Local completion-send logs from the resumed worker are diagnostic, not acceptance
evidence: the closed connection may fail before a report is generated or before
it reaches the coordinator. The C scheduler matrix above independently forces
old-report validation and checks that state remains unchanged.

Two retry-exhaustion scenarios submit a 60-second sleep with allowances of two
and zero retries. All workers register and heartbeat before submission: one per
allowed attempt, plus a healthy spare. The harness waits for each attempt's
RUNNING event and a fresh owner heartbeat, then kills that owner with SIGKILL.
Every lost owner must be distinct, with one transport-related death and no
heartbeat timeout or worker-sent terminal report. The six-second coordinator
timeout stays unchanged; loss events have a two-second observation deadline.

With two retries, the first two losses requeue the same job; the third produces
FAILED with `attempt=3`, `retry_count=2`, the last owner ID, no result, and no
pending job. With zero retries, the first loss produces FAILED with `attempt=1`
and `retry_count=0`. Both scenarios compare the entire expected transition
sequence. The spare must complete a new Fibonacci job with result `55`, keep
heartbeating, and never receive the failed job. The failed job's event history
must stay unchanged while this follow-up work completes and the CLI gets PONG. These
assertions detect extra retries despite available capacity and a blocked queue.
An `ExitStack` cleans up every worker fixture, including after failed assertions.

`integration/test_ping.py` starts the real coordinator and invokes the real CLI.
Its original 15 scenarios cover a successful exchange and sequential clients, default
port behavior, fragmented PING and PONG, repeated/coalesced frames, concurrent
clients alongside idle/partial peers, half-close handling, truncated requests,
invalid requests and replies, resets, receive timeout, connection refusal,
port conflicts, invalid arguments, and a complete frame followed by a partial
frame. Cleanup checks coordinator exit status
after SIGTERM and captures its logs for failure diagnostics.

Nine additional worker scenarios exercise the real coordinator using independent
Python TCP peers: simultaneous registered connections, fragmented registration,
all 15 two-part heartbeat splits, registration plus a coalesced PING and half-close,
70 successive registrations/disconnects, heartbeat ownership and stale IDs,
duplicate registration, truncated/invalid worker frames, and an idle registered
worker alongside a stalled payload. The stalled payload times out and marks its
worker dead; the idle worker expires under the default six-second heartbeat timeout.

One additional scenario rejects job replies/assignments sent to the coordinator,
and worker reports sent by unregistered clients. Valid JOB_SUBMIT is now handled
by the runtime and is covered by the scheduling suite. A healthy worker remains
usable while incorrect-direction frames are rejected.

Logs verify state transitions and timestamp updates, including that PING
and incomplete heartbeat bytes do not count as heartbeats. Positional log reads
avoid moving the file offset shared with the coordinator's output stream.

Fragmentation checks cover 13 delivery patterns in each direction: one byte at
a time, an uneven `3 + 5 + 4` split, and all 11 possible two-piece splits of a
12-byte header. After every non-final fragment, the test withholds the suffix
and checks that the receiver neither replies nor closes the connection. This
tests pending input without assuming that separate sends map to separate TCP
packets or receive calls. Coordinator cases reuse the connection to check that
receive state resets between frames.

Disconnect cases cover every incomplete header length from 0 through 11 bytes
for both requests and responses. Another case sends a complete PING plus five
bytes of the next PING together, expects exactly one PONG, and completes the
second header in two more pieces before checking a third exchange.

`integration/test_worker.py` adds eight scenarios for the real worker executable:

- Two workers running simultaneously with distinct coordinator-issued IDs;
  stopping one leaves the other registered and the CLI usable.
- The default worker endpoint on port 9000.
- Seventeen ACK fragment patterns: one byte at a time, `3 + 5 + 4 + 4`, and all
  15 two-part splits. The worker must not report an ID before the full ACK arrives.
- All 16 incomplete ACK prefixes, malformed headers, wrong types/lengths, and
  ID zero. Invalid headers are tested while the peer remains open, proving early rejection.
- A single five-second ACK deadline across delayed header and payload fragments.
- SIGINT/SIGTERM during an incomplete ACK.
- Coordinator EOF/reset and unexpected bytes coalesced after a valid ACK.
- Help, invalid arguments, and an unavailable endpoint (which may consume the
  existing five-second connect budget instead of refusing immediately).

The worker suite also verifies decoding/printing `UINT32_MAX`. Integration
suites share coordinator setup, cleanup, and protocol helpers through the
`CoordinatorTestCase` base class; worker process helpers live in
`WorkerProcessTestCase`. Test methods remain in their own subclasses and run only once.
Worker subprocesses are always cleaned up, and their stderr is checked for
AddressSanitizer/UBSan reports.

`integration/test_heartbeat.py` contains seven timing scenarios:

- Default two-second heartbeat cadence keeps a real worker alive past six
  seconds while an open, silent registration expires after the default timeout.
- A controlled peer independently checks repeated exact 16-byte heartbeat frames
  at a 120 ms interval; a partial ACK must not start heartbeat sending.
- A valid heartbeat renews the deadline beyond the original registration deadline.
- PING traffic and trickled heartbeat header/payload bytes cannot renew liveness.
- A worker configured to send more slowly than the timeout expires before its
  first heartbeat.
- A heartbeat queued while the coordinator is paused is rejected after expiry
  when the coordinator resumes; it cannot revive the old registration.
- Invalid/missing/duplicate duration arguments, overflow, help output, and worker
  options in reverse order.

Timing assertions allow scheduling slack; exact deadline boundaries are covered
by deterministic C tests. Every paused process is resumed in a `finally` block
before normal cleanup. The heartbeat suite uses both default timings and a
shorter coordinator timeout for focused scenarios; each fixture owns its processes.

`integration/test_failure_detection.py` contains five failure scenarios:

- A registered real worker exits successfully on SIGTERM; EOF marks it dead.
- A registered real worker is terminated by SIGKILL; its exit status confirms
  forced termination and EOF marks it dead without waiting for heartbeats.
- A registered Python peer closes with zero SO_LINGER, causing a TCP reset;
  the coordinator marks its registration dead with `reason=recv_error`.
- A registered Python peer sends two valid heartbeats, then remains open without
  sending more data. The coordinator must close it after six seconds of silence.
- The real-worker SIGSTOP case, moved from the heartbeat suite, keeps two workers
  alive for multiple timeout periods, pauses one, and requires only that ID to
  expire. The paused process must still be present when detection occurs.

Transport failures must be detected within 2.5 seconds of fault injection with
no `heartbeat_timeout` event. Timeout cases check the coordinator's monotonic
`detected_at_ms` and `silence_ms` diagnostics against its last recorded heartbeat
and configured timeout. Each case verifies one death event for the failed ID,
continuing heartbeat progress by a healthy worker, and a usable CLI. Real-worker
exit/pause cases check that freed slots accept fresh IDs. The peer silence case
does not call `close()` or `shutdown()` before coordinator-initiated closure.
SIGSTOP cleanup always resumes the process before trying to stop it.

The focused command prints each detected reason and elapsed observation time or
heartbeat silence duration. The full integration target includes this suite once.

By default, suites select available ports. The original coordinator and worker
suites each skip one default-endpoint check. To run all 143 scenarios, stop any existing
coordinator on port 9000 and run:

```sh
make test-integration INTEGRATION_ARGS='--port 9000'
# Or run the complete sanitizer suite, including the default endpoints:
make test-sanitize INTEGRATION_ARGS='--port 9000'
```

When 9000 is selected, the harness starts the coordinator without `--port` and
also invokes `faultline ping` and `faultline-worker` without `--coordinator`
to test all defaults. Processes started by the harness are stopped afterward.
With automatic port selection, the suite discovers 143 scenarios: 141 run and two
default-port checks are skipped. The 21 persistence/startup/crash scenarios always use
automatic ports independently of the legacy suites' optional port 9000 checks.
