# Coordinator statistics

`faultline stats` reads one coordinator snapshot and exits. Use the built binary:

```sh
./build/debug/faultline stats
./build/debug/faultline stats --coordinator 127.0.0.1:9100
```

The default endpoint is `127.0.0.1:9000`. Output is one `name=value` per line.
Integer measurements use decimal notation; `session_completed_per_second` has
three decimal places. A valid response, including an empty system, exits 0.
Argument, connection, protocol, and output errors exit 1 with diagnostics on
stderr. No automatic polling or retry is performed.

## Counters and scopes

The command separates three kinds of measurement:

- **Retained job totals:** reconstructed from published job records. They survive
  restart through the existing WAL; no separate statistics records are needed.
- **Current gauges:** how many jobs/workers are in each category at snapshot time.
  These can increase or decrease.
- **Current-session measurements:** activity after startup recovery and listener
  setup, plus a report of that startup. They reset on each coordinator launch.

### Retained job counters and gauges

| Output | Definition |
| --- | --- |
| `jobs_submitted_total` | Number of retained, durably accepted jobs, including terminal jobs. |
| `jobs_queued` | Jobs in QUEUED, including retries awaiting reassignment. |
| `jobs_assigned` | Jobs in ASSIGNED; their current start report has not been accepted. |
| `jobs_running` | Jobs in RUNNING; their current start report has been accepted. |
| `jobs_completed_total` | Jobs in terminal DONE. Each job contributes once. |
| `jobs_failed_total` | Jobs in terminal FAILED after exhausting their retry allowance. |
| `job_attempts_total` | Sum of retained `attempt` values: durable assignments, including assignments whose bytes never reached a worker. |
| `job_retries_total` | Sum of retained `retry_count` values: consumed retry allowances, including jobs still waiting for their next assignment. |
| `completed_latency_avg_ms` | Floored mean of `finished_at_ms - created_at_ms` across retained DONE jobs. Zero with no completed samples. |

The five state counts sum to `jobs_submitted_total`. Because the MVP retains all
accepted jobs without eviction, submitted/completed/failed totals are cumulative
within the same retained WAL history. They are not calculated from the largest
job ID, and are not lifetime totals across different logs. A new log starts a
new history. Future deletion or compaction that drops job history would require
revisiting these definitions.

A durably accepted job can count even if its submission acknowledgment was lost.
A client retry can create another job because [submission deduplication](request-deduplication.md)
is deferred. These counters do not measure client-confirmed acknowledgments.

A failed attempt that is requeued increases `job_retries_total`, not
`jobs_failed_total`. The latter increases only when a job becomes terminally
FAILED. Task errors, worker disconnects, heartbeat expiry, and interrupted
attempts found during coordinator recovery follow the same existing retry policy.
Reaching the final failed attempt consumes no additional retry allowance.

For example, a job allowed two retries can receive three assignments:
`attempts=3`, `retries=2`. If its third attempt fails, it contributes one
failed job. If it succeeds instead, it contributes one completed job.

### Current worker gauges

| Output | Definition |
| --- | --- |
| `workers_retained` | Nonempty registry slots, including retained dead registrations. |
| `workers_alive` | ALIVE registry entries whose heartbeat deadline has not passed. |
| `workers_expired` | ALIVE registry entries whose deadline has passed, pending normal event-loop cleanup. |
| `workers_dead` | Registrations already marked DEAD and not yet replaced. |
| `workers_busy` | A subset of `workers_alive` that owns an ASSIGNED or RUNNING job. |
| `workers_idle` | The remaining `workers_alive` entries with no active job. |
| `heartbeat_timeout_ms` | The coordinator's configured timeout for this launch. |

`alive + expired + dead = retained`, and `busy + idle = alive`.
Expired workers are excluded from busy/idle, even if their job still awaits
normal worker-loss cleanup. No expiry or requeue is performed by the query.
These categories match the [worker listing's displayed liveness](listings.md#workers).

Worker counts describe registrations, not OS processes or all TCP connections.
Heartbeats demonstrate contact, not task progress. Dead slots can be reused,
so `workers_dead` is not a cumulative death counter. All worker gauges start
at zero after coordinator restart; the durable worker-ID allocator still
prevents identity reuse. Busy does not promise that computation has started,
and idle does not guarantee immediate dispatch (a partial frame or buffered
reply can temporarily prevent assignment).

### Current-session and startup measurements

| Output | Definition |
| --- | --- |
| `session_uptime_ms` | Monotonic elapsed time since the event loop's baseline was captured, after successful startup recovery and listener setup. |
| `session_jobs_submitted` | Jobs durably accepted after that baseline. |
| `session_jobs_completed` | Jobs reaching DONE after that baseline, including jobs recovered from an earlier launch. |
| `session_jobs_failed` | Jobs reaching terminal FAILED after that baseline. |
| `session_job_retries` | Retry allowances consumed after that baseline. |
| `startup_jobs_recovered` | Retained jobs at the baseline: all states, not just unfinished work. Zero for a fresh log. |
| `startup_interrupted_jobs` | Previously ASSIGNED/RUNNING jobs reconciled by this startup, whether requeued or terminally failed. |
| `startup_duration_ms` | Elapsed monotonic time from the beginning of WAL open/create through replay, recovery writes, startup logging, and listener setup, to the baseline. |
| `session_completed_per_second` | `1000 × session_jobs_completed / session_uptime_ms`; zero when uptime is zero. Calculated by the CLI. |

The baseline is captured **after** interrupted-job reconciliation. Replayed
submissions/completions and startup retry/failure outcomes therefore do not
become runtime session events. A recovered job that completes later does count
as a session completion; session completions can exceed session submissions.

Startup measurements describe this launch, not a historical accumulation.
`startup_duration_ms` includes fresh-log creation when `--init-wal` is used.
It does not measure coordinator downtime, failure-detection delay, or the time
until a replacement worker completes recovered work.

## Performance interpretation

The throughput value is an average of accepted successful outcomes over the
whole serving session, including idle periods and synchronous storage delays.
It is not an instantaneous rate, a benchmark, or a count of every execution
that occurred on workers. Old attempts whose results are rejected do not count.

Completion latency uses the existing durable logical job timeline. It includes
recorded queue time and earlier attempts/retries, and is not just CPU execution
time. Coordinator downtime and portions of time never recorded before a crash
are excluded by that timeline. See [logical time across restart](persistence.md).
It is therefore not end-to-end wall-clock latency across outages.

Latency is derived from DONE jobs only; failed and unfinished jobs are excluded.
The implementation divides each duration into quotient/remainder parts before
summing, producing an exact floored mean without overflowing when individual
durations approach `INT64_MAX`.

## Restart example

Suppose five jobs include one DONE, one FAILED, one RUNNING with a retry left,
one ASSIGNED with no retries left, and one QUEUED.

After a crash and restart:

- Submitted remains 5 and completed remains 1.
- Failed becomes 2; queued becomes 2; assigned/running become 0.
- Attempts remain 4 until another assignment occurs.
- Retries becomes 1 as recovery requeues the eligible interrupted job.
- `startup_jobs_recovered=5` and `startup_interrupted_jobs=2`.
- All four session job counters are 0, and all worker gauges are 0.

A second restart with no new active assignments reports zero startup interruptions
and consumes no additional retries. The integration suite verifies this sequence.

## Wire format

These additive version 1 types use the existing 12-byte FLIN header:

| ID | Message | Direction | Payload | Whole frame |
| --- | --- | --- | --- | --- |
| 19 | STATS_REQUEST | Client → coordinator | Empty | 12 bytes |
| 20 | STATS_RESPONSE | Coordinator → client | 192 bytes | 204 bytes |

The response is 24 unsigned 64-bit integers, all big-endian, without padding.
The derived floating-point throughput is not transmitted. Offsets below are
relative to the payload:

| Offset | Field |
| --- | --- |
| 0 | `jobs_submitted_total` |
| 8 | `jobs_queued` |
| 16 | `jobs_assigned` |
| 24 | `jobs_running` |
| 32 | `jobs_completed_total` |
| 40 | `jobs_failed_total` |
| 48 | `job_attempts_total` |
| 56 | `job_retries_total` |
| 64 | `completed_latency_avg_ms` |
| 72 | `workers_retained` |
| 80 | `workers_alive` |
| 88 | `workers_expired` |
| 96 | `workers_dead` |
| 104 | `workers_busy` |
| 112 | `workers_idle` |
| 120 | `session_uptime_ms` |
| 128 | `session_jobs_submitted` |
| 136 | `session_jobs_completed` |
| 144 | `session_jobs_failed` |
| 152 | `session_job_retries` |
| 160 | `startup_jobs_recovered` |
| 168 | `startup_interrupted_jobs` |
| 176 | `startup_duration_ms` |
| 184 | `heartbeat_timeout_ms` |

Validation requires exact lengths, job/worker capacity bounds (256/64), valid
state-count sums, worker partitions, and session counts consistent with retained
totals. Attempts must equal retries plus nonqueued jobs, following the existing
job-model invariant. Retry totals are bounded by `jobs_submitted_total × UINT32_MAX`.
Latency, uptime, and startup duration fit the nonnegative signed 64-bit clock
range; latency is zero without completed samples. Timeout is in `1..INT_MAX`.
Counts are bounded before arithmetic, so malformed inputs cannot bypass checks
through integer wraparound. Inconsistent counters return `INVALID_STATS`.

Old peers reject the new message IDs; no capability negotiation is added.
The shared maximum frame remains 9744 bytes, set by full job listings.

## Read-only and transport guarantees

The coordinator aggregates published jobs and workers in one event-loop action
and encodes a complete reply. Later events cannot change buffered response bytes.
Separate stats/list/status commands can observe different moments.

The query does not write the WAL, allocate job/worker IDs, change queues, consume
retries, update the session baseline, or refresh heartbeats. It works at full job
capacity. It still needs one free slot under the existing 64-connection limit.

Unregistered client connections may mix stats, status, listings, submissions,
and PING with serialized replies. Once used for a query/submission, a connection
cannot become a worker. Registered workers cannot query stats, and incoming
STATS_RESPONSE messages are rejected by the coordinator.

The CLI shares the bounded query receiver with the other inspection commands.
It validates the entire response before printing anything. Fragmented responses
share one five-second response deadline; connect and send have separate budgets.
Invalid, truncated, or unexpected replies fail without a partial statistics report.

## Verification

```sh
make test-stats-protocol
make test-stats INTEGRATION_ARGS='--port 9000'
make SANITIZE=1 test-stats
```

Seven C groups cover literal bytes, unaligned buffers, every truncated prefix,
output preservation, malformed counts, mixed frames, durable/session baselines,
large attempts, overflow-safe latency, liveness boundaries, and invalid inputs.
Eleven integration scenarios cover all job states, retries versus terminal
failure, dead-slot reuse, full stores, read-only WAL/ID behavior, fragmented and
coalesced requests, connection roles, heartbeat expiry, repeated crash recovery,
CLI validation, and response deadlines.

The [CLI and observability review](observability-review.md) adds cross-command
checks against independently decoded WAL records and lifecycle logs. It also
distinguishes startup timing and recovery-event intervals from a full recovery
latency benchmark.
