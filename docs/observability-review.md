# CLI and observability phase review

Review date: 2026-09-29. This review covers `status`, `jobs`, `workers`,
`stats`, runtime logging, and inspection across execution, failure, and restart.

The MVP acceptance target is that a reviewer can inspect the system without
reading source. The four commands expose published coordinator state; timestamped
logs explain how it changed. Read the [logging guide](logging.md) for event fields,
[status guide](status.md) for individual outcomes, [listing guide](listings.md)
for tables, and [statistics guide](stats.md) for counter definitions.

## What the commands actually prove

| Observation | Meaning and boundary |
| --- | --- |
| QUEUED | Retained job waiting in the FIFO. Worker ID is zero; a retry can retain a prior attempt number and failure reason. |
| ASSIGNED | A worker/attempt lease was durably recorded. This does not prove the assignment reached the worker. |
| RUNNING | A matching STARTED report was durably accepted. The worker sends this before creating its task thread; it does not prove ongoing computation. |
| DONE | The coordinator durably accepted the saved result for the matching attempt. It does not prove exactly-once execution or external effects. |
| FAILED | The job is terminal under its retry policy. An individual failed attempt may instead produce QUEUED. |
| ALIVE worker | Its registration is live and its heartbeat deadline has not passed at snapshot time. Heartbeats do not prove task progress. |
| EXPIRED worker | Its heartbeat deadline has passed, but normal event-loop cleanup has not yet marked it DEAD. Queries do not perform that cleanup. |
| DEAD worker | The coordinator revoked that registration. The old process could still exist or compute. |
| Worker ID on a terminal job | Historical owner of its accepted final attempt. It need not be present in the current worker listing. |
| Unknown job ID | `status` reports "job ID not found" on stderr, leaves stdout empty, and exits 2. A known FAILED job still exits 0: lookup succeeded. |

Worker registrations start empty after coordinator restart. Durable identity
counters continue, so a newly registered worker does not reuse an old identity.
Dead registry entries can also disappear through slot reuse during one session.

Each command returns one snapshot. Separate commands can observe different moments
while jobs run. Queries do not append WAL records, consume job/worker identities,
or change retry limits. Scheduler and heartbeat activity can continue independently.

## Counter and clock scopes

- **Retained job totals:** submitted, state counts, attempts, and consumed retries
  are reconstructed from retained job records. Terminal outcomes survive restart.
- **Worker gauges:** current registry entries, liveness, idle/busy counts, and
  active leases describe this coordinator process.
- **Session activity:** begins after replay and interrupted-attempt reconciliation.
  Restoring old DONE jobs does not count as completing them again this session.
- **Startup diagnostics:** report recovered jobs, interrupted attempts, repaired
  tail bytes, and startup duration separately from live session changes.
- **Completion latency:** uses the persisted logical job timeline. It includes
  queue/retry time, excludes coordinator downtime, and is not task CPU time.
- **Throughput:** accepted completions in the current session divided by session
  uptime, subject to the fixed-point definition in the statistics guide.

UTC log timestamps describe calendar time. Local monotonic log timestamps support
elapsed-time comparisons within a process lifetime. Neither replaces the WAL's
logical job timestamps or sequence numbers.

Recovery is observable through heartbeat silence/timeout diagnostics, worker-loss,
reassignment and completion events, and `startup_duration_ms`. Differences between
coordinator monotonic event times can show observed detection-to-reassignment or
reassignment-to-completion intervals. These are not an aggregate end-to-end
recovery-latency metric. Controlled recovery-latency experiments and benchmark
reporting remain part of the later benchmark phase.

## Acceptance evidence

The new `tests/integration/test_observability.py` runs six process scenarios.
Its independent WAL decoder checks record checksums and compares the latest job
snapshots with all four commands. Comparisons occur at deliberately stable points;
they do not assume separate live queries form one atomic multi-command snapshot.

| Case | Verified observations |
| --- | --- |
| All five job states | Full job/status fields, job/worker counts, active leases, attempt/retry totals, and logical completion latency agree with WAL records. |
| Binary result and unknown ID | Exact byte length and escaped status/log result survive; unknown lookup uses exit 2. |
| Worker loss and task failure | Requeue preserves the previous owner's ID in logs; retries and terminal failures have distinct outcomes. A task retry can later complete. |
| Wrong-owner/outdated report | Rejection logs claimed/current identity and state. The targeted job stays unchanged; closing the sender can independently fail its own different job. |
| Coordinator SIGKILL/restart | DONE/FAILED outcomes remain intact; eligible active jobs requeue, exhausted ones fail, queued work retains order, and fresh workers finish eligible work. |
| Recovered counters and logs | Session activity resets; worker registry is empty; old outcomes appear as RESTORED snapshots, not new completion/failure events. |
| Silent worker with open TCP | Heartbeat expiry revokes the worker and requeues its job; commands, WAL, reason, and timeout evidence agree. |
| Real worker execution/stop | Successful sleep, invalid Fibonacci input, and an interrupted long sleep agree with command state. Local sends remain explicitly unconfirmed. |
| Invalid traffic and startup failure | Connection close reasons, structured system errors, missing-WAL failure, and exit status remain visible. |
| Escaping and read-only queries | Control bytes cannot split log lines or inject terminal controls; querying stable state leaves WAL bytes identical. |

Three C logging groups additionally check byte escaping and buffer boundaries,
UTC/PID/monotonic metadata, errno preservation, invalid arguments, and output
failure. Existing protocol, execution, heartbeat, recovery, persistence, and
inspection suites remain part of the full regression. The execution/recovery
test parsers now accept the enriched log format while retaining exact state,
identity, and result assertions.

## Verification record

The CLI and basic observability acceptance checks pass. Both normal and
AddressSanitizer/UBSan builds passed all 131 C groups and 143 integration scenarios,
with zero skips, compiler warnings, or sanitizer diagnostics. Port 9000 enabled
the default-endpoint checks.

| Verification | Result |
| --- | --- |
| Normal full regression, followed by the final unit-target recheck | All 143 integration scenarios and all 131 C groups passed. |
| Full AddressSanitizer/UBSan regression | All 131 C groups and 143 integration scenarios passed. |
| Documentation/diff checks | Changed Markdown file links resolve, code fences are balanced, and `git diff --check` is clean. |

Commands used:

```sh
make test INTEGRATION_ARGS='--port 9000'
make test-unit
make SANITIZE=1 test INTEGRATION_ARGS='--port 9000'
```

Environment: macOS Darwin 25.6.0 arm64, Apple Clang 21.0.0, Python 3.9.6.
The current suite contains 131 C groups and 143 integration scenarios.

These are local process/network tests, including process crashes and injected
storage failures. They do not establish Linux CI results, physical power-loss
survival, network-partition behavior across machines, or production performance.
The [persistence review](persistence-review.md) defines the storage assumptions.

## Remaining limits and handoff

The MVP retains at most 256 jobs and 64 worker registrations; the coordinator
also has a 64-client connection limit. There is no terminal-job eviction or
historical worker event database. Logs are best-effort diagnostics, not a durable
audit trail: stdio flushing is not WAL synchronization, and slow output can delay
the event loop. There is no asynchronous logging, rotation, remote metrics export,
or JSON command mode.

Assignment leases and bounded at-least-once retries still permit repeated task
execution. Missing submission acknowledgments remain ambiguous;
[request deduplication](request-deduplication.md) is documented for later work.
Workers require fresh connections after restart; automatic reconnect is not
implemented.

This completes the CLI and basic observability phase. The plan moves to
**Testing & chaos**: Linux CI
and a reproducible random worker-kill/restart harness, building on the existing
unit, integration, and sanitizer checks. That harness should account for every
submitted job as completed or terminally failed, then drain eligible work after
fault injection stops. Scaling and controlled failure-recovery benchmarks follow.
