# Query a job with the CLI

`faultline status ID` asks the coordinator for one saved job and prints a readable
snapshot. It works for QUEUED, ASSIGNED, RUNNING, DONE, and FAILED jobs, including
records restored from the WAL after coordinator restart.

## Usage and output

After building with `make`, use the job ID printed by `submit`:

```sh
./build/debug/faultline submit sleep --args 1000 --max-retries 1
# job_id=1
./build/debug/faultline status 1
```

The default coordinator is `127.0.0.1:9000`. To select another numeric IPv4 endpoint:

```sh
./build/debug/faultline status 1 --coordinator 127.0.0.1:9100
```

The ID must contain only decimal digits and be between 1 and
18,446,744,073,709,551,615 (`UINT64_MAX`). Leading zeroes are accepted, but zero,
signs, whitespace, fractions, hexadecimal notation, and overflow are rejected
before connecting. Only the optional `--coordinator IPv4:PORT` pair is accepted
after the ID. `faultline --help` lists the command and exit codes.

Before a worker takes the example job:

```text
job_id=1
state=QUEUED
worker_id=none
attempt=0
retries=0/1
failure=NONE
result_bytes=0
```

After worker 1 completes it:

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

The actual state and worker depend on when the query runs. Each invocation reads
once and exits; it does not wait for completion or subscribe to changes.

| Field | Interpretation |
| --- | --- |
| `job_id` | The requested coordinator-issued identity. |
| `state` | Current saved state, spelled out instead of its numeric wire code. |
| `worker_id` | `none` for QUEUED; current owner for ASSIGNED/RUNNING; last owner for DONE/FAILED. |
| `attempt` | Latest assignment number; zero before any assignment. |
| `retries` | Consumed retry allowances / maximum retries after the initial attempt. |
| `failure` | NONE, TASK, or WORKER_LOST; a queued retry retains its previous failure reason. |
| `result_bytes` | Length of the original binary result. |
| `result` | Quoted, escaped result, printed only for DONE; an empty success prints `result=""`. |

Retry allowance is consumed when an unsuccessful attempt returns to QUEUED.
For example, `attempt=1` and `retries=1/1` on a queued job mean its second and
final assignment is still waiting. A new assignment clears the previous failure.
Terminal worker IDs are history, not proof that the worker remains alive.
See the [state consistency rules](job-status-protocol.md#state-and-counter-consistency).

Printable ASCII result bytes appear directly except quotes and backslashes.
All other bytes use a lowercase, two-digit `\xHH` escape. For example, bytes
`41 00 0a 22 5c ff` display as `result="A\x00\x0a\x22\x5c\xff"`.
This preserves embedded zeroes and keeps control bytes from changing terminal
output. `result_bytes` counts original bytes, not display characters. Results
are printed by explicit length, never treated as NUL-terminated strings. The
four built-in tasks return readable ASCII; no task-specific interpretation is
needed to display them.

## Exit codes and errors

| Code | Meaning | Output |
| --- | --- | --- |
| 0 | A known job was successfully queried, including a FAILED job. | Snapshot on stdout. |
| 2 | The coordinator returned NOT_FOUND for the requested ID. | `faultline: job ID not found` on stderr; empty stdout. |
| 1 | Invalid arguments, connection/send/receive failure, malformed reply, mismatched ID, or output error. | Diagnostic on stderr; protocol/transport errors produce no snapshot. |

A failed job is a successful lookup. Its `state` and `failure` fields describe
the execution outcome. An unknown ID is different from either a failed job or
an unreachable coordinator. NOT_FOUND says only that this coordinator's retained
store lacks the record. A missing submission ACK remains uncertain; status
does not provide [request deduplication](request-deduplication.md).

## Coordinator lookup and transport

`src/coordinator/main.c` admits `JOB_STATUS_REQUEST` on unregistered client
connections and calls the existing `faultline_scheduler_find()` on the published
job store. It copies the record into a status response, or echoes the ID in
NOT_FOUND. The single event loop owns the store, so snapshot copying and encoding
finish before another event can change that record. Buffered response bytes stay
stable even if delivery takes several writes.

Queries do not append/flush the WAL, allocate IDs or jobs, consume retries,
remove queue entries, assign workers, or renew heartbeats. They still work when
all 256 job slots are occupied. Normal scheduling, worker reports, and timeouts
can independently change the job before the next query. Startup replay and
interrupted-attempt reconciliation finish before the coordinator starts serving
queries; no old TCP connection is restored.

A connection may mix status requests, submissions, and PING, with serialized
replies. After its first valid submission or query, it cannot become a worker.
Registered worker connections cannot query jobs. Status responses and NOT_FOUND
are outbound-only at the coordinator. Invalid direction, role, ID, or length
closes the connection under the existing protocol policy.

The CLI sends the existing [20-byte request](job-status-protocol.md) and reads
the reply in bounded stages: header, fixed payload prefix, then any result bytes.
It checks header type/length before collecting the prefix, and invokes the codec
to validate the prefix and inner length before collecting variable data. The
entire response shares one five-second monotonic deadline; partial transfers do
not restart it. Connect and send retain their separate five-second budgets.

The complete reply must pass codec validation and echo the requested job ID
before the CLI prints anything. The CLI owns a bounded 1072-byte receive buffer,
closes its socket on every post-connect exit path, and does not automatically
retry. A response is a snapshot, not an execution lease or proof of worker
progress. Existing [durability guarantees](durability.md) remain unchanged.

## Verification

```sh
make test-status
make SANITIZE=1 test-status
# Include the default endpoint branch as well (port 9000 must be available):
make test-status INTEGRATION_ARGS='--port 9000'
```

Fifteen scenarios in `tests/integration/test_status.py` exercise the real CLI,
coordinator, workers, and independent TCP peers. They cover every job state,
unknown IDs, retries and both failure reasons, all built-in results, empty and
maximum binary results, fragmented/coalesced frames, malformed/truncated replies,
strict argument parsing, 64-bit boundaries, response ID matching, timeout budgets,
connection roles, a full job store, heartbeat expiry despite polling, and queries
after a coordinator crash/restart. Read-only checks compare exact WAL bytes and
verify ID allocation and FIFO behavior. The suite is included in `make test`.
