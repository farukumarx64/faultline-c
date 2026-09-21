# Job model and state transitions

The job model is defined in `include/job.h`, with coordinator-owned operations
in `src/coordinator/job.c`. It describes one job and enforces changes to that
record. A separate [FIFO queue](queue.md) now stores pending job IDs. The model
does not allocate globally unique IDs, send job messages, execute tasks, or
itself connect worker failure detection to retries. The [scheduler](scheduling.md)
now orchestrates storage, enqueue, assignment, reports, and worker-loss transitions.

## What a job contains

| Field | Purpose |
| --- | --- |
| `id` | Nonzero 64-bit job ID, supplied by the caller. The coordinator scheduler now allocates unique IDs within its lifetime. |
| `task_type` | SLEEP, PRIME_COUNT, FIBONACCI, or HASH. All four have [worker executors](tasks.md). |
| `arguments`, `argument_size` | An owned copy of the task's opaque argument bytes and their length. |
| `state` | QUEUED, ASSIGNED, RUNNING, DONE, or FAILED. |
| `worker_id` | Assigned worker ID while active; zero while queued. Terminal jobs retain the last worker ID for inspection. |
| `attempt` | Assignment number: zero before the first assignment, then 1, 2, 3, etc. |
| `retry_count`, `max_retries` | Requeues already granted and the maximum additional attempts allowed. |
| `created_at_ms` | Coordinator monotonic time when the record was initialized. Never changes. |
| `updated_at_ms` | Time of the latest successful state transition. |
| `assigned_at_ms` | Time the current/latest attempt was assigned. |
| `started_at_ms` | Time that attempt reported starting. |
| `finished_at_ms` | Time the job reached terminal DONE or FAILED. |
| `result`, `result_size` | Owned result bytes, set only on successful completion. Empty results are allowed. |
| `failure` | TASK or WORKER_LOST after a failed attempt; NONE initially and when a new attempt is assigned. |

Arguments and results each have a 1024-byte model limit. The fixed arrays make
ownership explicit without heap allocation: callers can reuse their input buffers
after initialization or completion succeeds. Lengths count bytes, not characters;
embedded zero bytes are allowed. Empty payloads may use a NULL source pointer.
A source buffer must not overlap the destination job record.

This is an initial bounded representation for the MVP, separate from the generic
protocol header's 1 MiB payload ceiling. Task-specific argument/result schemas
and validation belong to the [worker executors](tasks.md). The [job message codec](job-protocol.md)
now encodes task IDs, bounded opaque data, and assignment/report identity. The model checks
the known task type and byte limits, but does not interpret a sleep duration, hash
input, or numeric computation limit.

All numeric fields are host-order values. Neither this struct nor its arrays of
metadata constitute a wire format or WAL record. Job messages encode selected
fields explicitly; persistence serialization is still future work. The worker ID is a logical ID, never a socket descriptor.

## States and allowed transitions

```text
QUEUED --assign--> ASSIGNED --start--> RUNNING --complete--> DONE
                     |                  |
                     +------ fail ------+
                              |
                    retries remaining?
                       /           \
                     yes           no
                      |             |
                    QUEUED        FAILED
```

| Current state | Meaning | Allowed next states |
| --- | --- | --- |
| QUEUED | Waiting to be assigned. | ASSIGNED |
| ASSIGNED | An attempt belongs to a worker, which has not reported starting. | RUNNING, QUEUED, FAILED |
| RUNNING | The assigned worker has reported starting this attempt. | DONE, QUEUED, FAILED |
| DONE | Successful terminal outcome, including an optional result. | None |
| FAILED | Terminal failure after retry allowance is exhausted. | None |

QUEUED and FAILED transitions from active states occur only through the failure
operation and its retry rule. ASSIGNED can fail before STARTED, for example if
the worker disconnects or cannot begin the task. Completion requires RUNNING;
the worker message flow must report STARTED before COMPLETED.

There are no self-transitions. A duplicate STARTED or COMPLETED call fails without
changing the record. DONE and FAILED cannot be reassigned, restarted, completed
again, or requeued. The coordinator closes connections that send
invalid or duplicate reports; these strict model operations do not silently accept them.

`faultline_job_can_transition()` checks only this state graph. It cannot decide
whether a particular worker owns the job or whether retry budget remains. Use
the named operations to perform changes; do not set `job.state` directly.

## Operations and ownership

| Function | Behavior |
| --- | --- |
| `faultline_job_init()` | Initialize fresh storage as QUEUED, copy arguments, set creation time and retry limit. |
| `faultline_job_assign()` | Assign a queued job to a nonzero worker ID and increment the attempt number. |
| `faultline_job_start()` | Require ASSIGNED plus a matching worker/attempt and record the start time. |
| `faultline_job_complete()` | Require RUNNING plus a matching worker/attempt, copy the result, and mark DONE. |
| `faultline_job_fail()` | Require an active matching worker/attempt, then requeue or mark FAILED according to retry allowance. |

Initialization requires a known task, a nonzero ID, nonnegative time, and valid
argument storage. Later calls require an initialized record, valid inputs, an
allowed transition, and time at least as recent as `updated_at_ms`. Reports must
match both the worker ID and attempt. Errors leave the entire record unchanged,
including timestamps, counters, and payload bytes; validation happens before
mutation. Error results distinguish invalid input, oversized payloads, invalid
transitions, reversed time, wrong workers, and stale attempts.

The coordinator owns the record. Treat public fields as read-only outside the
module, and call init only on fresh/reusable storage, never to overwrite a live
job. This module allocates no memory, reads no clocks, and performs no I/O. Its
caller supplies time and controls the record's lifetime. The scheduler
also verifies that a worker is alive and idle; a nonzero ID alone does not
establish either condition. Terminal `worker_id` is historical attribution,
not a continuing reservation of that worker.

## Retry and attempt rules

`max_retries` means additional attempts after the first one:

| Retry limit | Maximum assignments |
| --- | --- |
| 0 | 1 |
| 1 | 2 |
| 2 | 3 |

On a failed active attempt:

```text
if retry_count < max_retries:
    retry_count += 1
    state = QUEUED
else:
    state = FAILED
```

A requeue preserves ID, task, arguments, creation time, and the attempt number.
It clears `worker_id` and resets assignment/start/finish timestamps to unset.
The failure reason remains available while queued; assignment clears it to NONE.
`updated_at_ms` records requeue time. The next assignment increments `attempt`.
The retry counter counts granted requeues, not all failures or completed retries;
the final exhausted failure does not increment it beyond its limit.

For example, job 42 might follow this sequence:

```text
job 42, worker 7, attempt 1 -> task fails -> QUEUED, retry_count=1
job 42, worker 7, attempt 2 -> starts again
late completion for job 42, worker 7, attempt 1 -> rejected
completion for job 42, worker 7, attempt 2 -> accepted
```

Checking the worker ID alone would accept the wrong report in this example.
Attempt numbers distinguish repeated assignments even to the same worker. They
use 64 bits so the largest 32-bit retry allowance plus the initial attempt fits.
The defined job messages carry the attempt along with job and worker identity.
Their coordinator handlers validate the sending connection before calling these operations.

These are record transitions only. Calling fail does not insert anything into
a FIFO, send another assignment, or run another task. An explicit successful
queue push adds the retried ID to the back. The scheduler now performs that
insertion for task failure and worker loss. The coordinator ties the active
assignment's lease to worker connection/heartbeat liveness; see the
[lease contract](recovery.md#assignment-lease). Separate execution deadlines and
persistent recovery remain later work. The model preserves only the current/latest
attempt's metadata, not a full attempt history.

## Timestamp rules

Times are coordinator `CLOCK_MONOTONIC` milliseconds. The caller obtains them
through the existing clock helper during runtime job handling. They are
not wall-clock dates or worker-provided timestamps. Equal timestamps are valid:
multiple events can occur in one millisecond. Going backward is rejected.

`-1` means an assignment/start/finish event has not occurred for this attempt.
Zero is a valid time, so it cannot be the unset marker. A failure before STARTED
leaves `started_at_ms` unset. Requeue clears the attempt's timestamps; terminal
states keep their available timestamps for inspection.

These current values belong to one coordinator clock lifetime. The upcoming
[durability contract](durability.md#time-across-restarts) preserves job timestamps
on a logical timeline: a new session continues from the recovered maximum plus
fresh monotonic elapsed time. Socket and heartbeat deadlines still use a fresh
raw monotonic clock. This policy is defined but not implemented; it excludes
downtime from cross-session job timestamp differences.

## Example and tests

This illustrates model calls with the ASCII argument `1000`, meaning a one-second
sleep. No executor runs in this snippet. Check return codes at each step:

```c
#include "job.h"
#include <stdlib.h>

int main(void)
{
    struct faultline_job job;
    const uint8_t arguments[] = {'1', '0', '0', '0'};

    if (faultline_job_init(&job, 42, FAULTLINE_TASK_SLEEP,
                           arguments, sizeof(arguments), 2, 100) != FAULTLINE_JOB_OK) {
        return EXIT_FAILURE;
    }
    if (faultline_job_assign(&job, 7, 110) != FAULTLINE_JOB_OK) {
        return EXIT_FAILURE;
    }
    if (faultline_job_start(&job, 7, job.attempt, 120) != FAULTLINE_JOB_OK) {
        return EXIT_FAILURE;
    }
    if (faultline_job_complete(&job, 7, job.attempt, NULL, 0, 140) != FAULTLINE_JOB_OK) {
        return EXIT_FAILURE;
    }
    /* job.state == FAULTLINE_JOB_DONE; job.attempt == 1 */
    return EXIT_SUCCESS;
}
```

`tests/test_jobs.c` exercises the full state graph, all named operations from
every state, payload ownership and boundaries, retry success/exhaustion, old
attempts, wrong workers, timestamp ordering, and integer limits. Rejected calls
are checked against a byte-for-byte snapshot to ensure no partial mutation.
Tests supply timestamps directly and need no sockets, workers, or sleeps.

Run `make test-unit` and `make SANITIZE=1 test-unit`. Job tests also run as part
of the full `make test` and `make test-sanitize` targets.
