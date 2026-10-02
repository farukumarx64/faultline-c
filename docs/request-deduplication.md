# Request deduplication: deferred enhancement

Recorded on 2026-09-24. Status: **documented for later; not implemented**.
This is an optional enhancement outside the original MVP phase list. The
[persistence phase](persistence-review.md) is complete under its existing
contract. This proposal is tracked in the [optional post-MVP work](post-mvp.md).
Revisit it before introducing automatic submission retries or when duplicate
submissions become costly, especially for tasks with external effects. This
note proposes behavior; it does not change today's protocol, WAL format, or CLI.

## Problem to solve

1. A client submits a job.
2. The coordinator saves it and successfully flushes the WAL.
3. The coordinator crashes, or the connection fails, before the client receives
   the submission acknowledgment.
4. The client submits again. Today, that request can create a second job ID.

Persistence preserves accepted jobs. Request deduplication would let a client
repeat the same logical submission and recover its original job ID without
creating more work. Until then, a missing ACK means an uncertain outcome; the
current CLI does not automatically resubmit.

## Proposed behavior

The client supplies a stable request key, generated before the first submission
and reused for every retry of that logical request. An intentional new job gets
a new key, even if its arguments match a previous job.

| Incoming submission | Proposed coordinator response |
| --- | --- |
| Previously unseen key with a valid request | Create one job, durably associate the key with its definition and ID, then acknowledge. |
| Existing key with the same request definition | Return the original job ID; do not enqueue another job, advance counters, reset retries, or rerun a terminal job. |
| Existing key with a different request definition | Return an explicit conflict; preserve the original job and mapping. |

The request definition includes task type, exact argument bytes/length, and retry
allowance. Compare the complete definition, or specify a collision-safe scheme;
a checksum match alone must not establish that two requests are identical.
Returning an existing ID confirms the original admission, not a new execution
or a newly available retry budget. Status queries can expose its current state.

Keys must remain reusable after a client process exits and restarts. A CLI that
generates a fresh key on every retry would not solve the lost-ACK problem. A
future interface therefore needs an explicit way to retain and reuse the key.
Exact flag names, key encoding, and generation rules are still undecided.

## Durability requirements

- Commit job creation and the request-key association as one recoverable
  transition before sending the ACK. A crash must not leave a recoverable job
  without its mapping, or a mapping pointing to an absent job. Extending the
  creation record is one possible design; the format decision is still open.
- Replay must rebuild the association alongside the job. Complete surviving
  records must retain it even if the original ACK or flush outcome was uncertain.
- Multiple connections using the same key must resolve to one job ID. Prepare any
  required index changes before the commit, using the existing durable ordering.
- Look up an existing key before applying new-job capacity checks. An identical
  retry should still return its existing job ID when the job store is full.
- Retain the mapping while its job is retained, including DONE and FAILED jobs.
  Any future expiration, eviction, or compaction policy must explicitly define
  the deduplication window; forgetting a key can permit another job later.
- Define protocol/WAL version compatibility and handling of existing jobs that
  have no request key. Do not silently reinterpret old records or invent keys
  from argument equality.

## Decisions to make when revisiting

- Key size, encoding, generation, validation, and scope. The current system has
  one coordinator/WAL; any future client or tenant namespace needs an explicit
  rule. Keys do not provide authentication.
- Whether keys are mandatory for a new protocol version or optional, and what
  guarantee applies to older clients or unkeyed submissions.
- Conflict/error response payloads and how the CLI exposes an uncertain outcome
  together with the key needed for a safe retry.
- WAL compatibility, mapping representation, retention limits, and behavior
  after restoring a backup. The guarantee can only cover retained key history.

## Acceptance checks for the future milestone

1. Repeat the same key and payload on one connection and on multiple connections;
   observe one creation, one queue entry, and the same acknowledged job ID.
2. Repeat after the job is QUEUED, ASSIGNED, RUNNING, DONE, or FAILED. Preserve
   its state, result, attempt, retry budget, and position in the queue.
3. Reject key reuse with a changed task, argument byte, argument length, or retry
   allowance without changing existing state or creating another job.
4. Kill after durable creation but before the ACK. Restart with the same WAL,
   repeat the same key, and recover the original ID without another creation.
5. Interrupt creation before writing, during a partial record, before flush,
   and after flush. Recovery must produce either no admitted keyed job or one
   complete job/key association, never an inconsistent pair.
6. Inject storage failures and restart repeatedly; preserve the existing
   no-premature-ACK rule and mapping consistency.
7. With all 256 job slots occupied, return existing IDs for identical keyed
   retries while rejecting new admissions as usual.
8. Verify legacy compatibility and the chosen retention/expiration rules,
   including binary arguments and intentional new jobs using different keys.

## Boundary of this enhancement

Deduplication would prevent multiple job creations for the same retained request
key. A single job can still execute multiple attempts under the existing
[bounded at-least-once policy](recovery.md#at-least-once-execution-and-its-limits).
It would not provide exactly-once execution, undo external effects, add worker
completion ACKs, or make a deleted/rolled-back WAL recoverable. Tasks with effects
would still need idempotency at their destination.
