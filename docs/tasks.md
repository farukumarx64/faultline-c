# Built-in task execution

For a first coordinator launch, explicitly create its WAL with `--init-wal`.
Later launches recover the same file; see [persistence startup](persistence.md#starting-and-restarting).

Workers now execute `sleep`, `prime_count`, `fibonacci`, and `hash`, send real
STARTED/COMPLETED/FAILED reports, and accept another assignment after reporting
the current outcome. The coordinator validates the report, stores the result,
and prints it in its `job_completed` log. Submission prints an acceptance ID;
[`faultline status ID`](status.md) retrieves state and saved result bytes.
[Persistence](persistence.md)
now retains accepted results and reconciles active jobs after coordinator restart.

## Start with sleep

Use three terminals from the project root after running `make`:

```sh
# Terminal 1
./build/debug/faultline-coordinator --port 9000

# Terminal 2
./build/debug/faultline-worker

# Terminal 3
./build/debug/faultline submit sleep --args 1000
# job_id=1
```

On a fresh coordinator, its log includes these events (worker ID depends on
registration order):

```text
[INFO] coordinator job_assigned job_id=1 state=ASSIGNED worker_id=1 attempt=1 retry_count=0 pending=0 result_bytes=0
[INFO] coordinator job_started job_id=1 state=RUNNING worker_id=1 attempt=1 retry_count=0 pending=0 result_bytes=0
[INFO] coordinator job_completed job_id=1 state=DONE worker_id=1 attempt=1 retry_count=0 pending=0 result_bytes=13 result="slept_ms=1000"
```

`1000` means one thousand milliseconds, or one second. The worker waits until
its monotonic deadline, then reports the requested duration. `slept_ms=1000`
does not claim an exact wall-clock runtime: scheduling and report delivery can
add delay. A zero-duration sleep also succeeds.

## Arguments and results

The existing protocol still transports bounded opaque byte arrays. These are
the task-level meanings of those bytes; no new message IDs or header fields
were necessary.

| Task | Argument | Allowed range | Example result |
| --- | --- | --- | --- |
| `sleep` | Decimal milliseconds | 0–86,400,000 (one day) | `1000` → `slept_ms=1000` |
| `prime_count` | Inclusive upper bound N | 0–100,000,000 | `100` → `25` |
| `fibonacci` | Sequence index N; F(0)=0, F(1)=1 | 0–93 | `10` → `55` |
| `hash` | Raw input bytes | 0–1024 bytes | `hello` → `a430d84680aabd0b` |

Numeric arguments must be nonempty ASCII digits. Leading zeros are allowed;
signs, spaces, fractions, units, embedded NULs, overflow, and values above the
task's limit are rejected. Parsing uses the explicit byte count, so it never
assumes that a network buffer is a NUL-terminated C string. Overflow is checked
before multiplying by ten and adding the next digit.

All four tasks return ASCII text without a trailing NUL in the payload. Results
use decimal numbers except `hash`, which returns exactly 16 lowercase hexadecimal
digits. Results still have the model/protocol's 1024-byte ceiling.

```sh
./build/debug/faultline submit prime_count --args 100
./build/debug/faultline submit fibonacci --args 10
./build/debug/faultline submit fibonacci --args 93
./build/debug/faultline submit hash --args hello
./build/debug/faultline submit hash --args-hex 00
./build/debug/faultline submit hash
```

Those results are respectively `25`, `55`, `12200160415121876738`,
`a430d84680aabd0b`, `af63bd4c8601b7df`, and `cbf29ce484222325`.
An omitted argument is valid for hash (the empty byte sequence), but invalid
for a numeric task. Hex is an input transport option, not numeric notation:
`--args-hex 3130` sends ASCII `10`; `--args-hex 0a` sends one newline byte.

The CLI checks options and byte bounds; the worker checks task semantics. Thus
`submit fibonacci --args 94` still gets a job ID, then receives a TASK failure
when executed. Acceptance is separate from task success.

## Executor algorithms

`include/task.h` defines the blocking executor API and limits.
`src/worker/task.c` contains the implementation. It has no socket or coordinator
job-state access: input goes in, a result or status comes out. On any error or
cancellation, the caller's result buffer remains unchanged.

- **Sleep** computes a monotonic deadline and sleeps in chunks of at most 20 ms.
  Each wake checks cancellation and the remaining time. It does not spin or
  block the networking thread for the requested duration. Interrupted waits
  recheck the deadline.
- **Prime count** counts 2 separately, then checks odd candidates for divisibility
  by odd divisors up to the square root. `divisor <= candidate / divisor` avoids
  squaring a value that might overflow. Both loops check cancellation. This is
  a straightforward CPU workload with constant auxiliary memory; it is not an
  optimized sieve, and large bounds can take substantial time.
- **Fibonacci** adds the preceding two values iteratively, using constant memory.
  The maximum index is 93 because F(93) fits an unsigned 64-bit integer and F(94)
  does not. The loop never computes F(94) as an unnecessary intermediate value.
- **Hash** performs one FNV-1a 64-bit pass over every input byte, including zeroes.
  It starts at 14695981039346656037, XORs each byte, then multiplies by
  1099511628211 with unsigned 64-bit wraparound. This is a deterministic,
  noncryptographic checksum chosen to keep the first implementation small and
  dependency-free. It is not a password hash. A single input is at most 1024
  bytes; throughput experiments can submit batches of jobs.

## Keeping heartbeats running

A worker process now has its main thread plus at most one task pthread:

```text
Main thread                            Task thread
receive and validate full assignment
send JOB_STARTED
start executor with owned assignment   -> read owned arguments
send heartbeats                           sleep or compute
receive coordinator traffic               write result/status
check completion every <=50 ms          <- publish atomic done flag
join task thread
send COMPLETED or FAILED
release local assignment
receive next assignment
```

The assignment is copied from the receive buffer before execution, so reusing
that buffer cannot change the task's arguments. The executor writes its result
and status, then stores `done=true` in a C11 atomic flag. The main thread reads
those fields only after observing that flag and joining the thread. This gives
the result a defined publication point instead of an unsynchronized shared flag.

Only the main thread ever writes to the TCP socket. Heartbeats and job reports
therefore cannot interleave their bytes. Every frame uses the existing full-send
helper and bounded send deadline. Task computation cannot block heartbeats;
network backpressure or OS scheduling still can delay them. Sending a frame
successfully proves local transmission, not acceptance by the coordinator.

While a task is active, the main loop wakes for its next heartbeat, incoming
data, a partial-frame deadline, or a completion check at most 50 ms away.
That adds a small result-notification delay without a busy loop. After completion
there is no idle task thread: the next job creates a new one. No thread pool,
parallel jobs inside one worker, or new external library is introduced.

STARTED is sent immediately before creating the task thread. A thread-start
failure can therefore be reported as TASK from RUNNING. Even a very fast task
has STARTED before its terminal report. Local ownership is cleared only after
the complete terminal frame has been sent; the coordinator independently frees
the worker only after validating that report. A second assignment while the
worker still owns a job remains a protocol error.

## Failures, retries, and shutdown

Invalid input or an executor/system error sends JOB_FAILED with the existing
TASK reason. The worker remains usable. Its `job_failed_sent` log includes
`task_status=invalid_arguments` or `task_status=system_error`. Successful reports
log `task_status=ok`. Local cancellation is not sent as a failure during shutdown.
The current failure payload has no detailed error text.

SIGKILL bypasses the worker's cleanup entirely. The separate
[worker recovery checks](recovery.md) verify that the coordinator requeues an
interrupted task and an already-connected worker completes it. They cover both
SIGKILL and a busy worker paused with SIGSTOP until its heartbeat expires.

The coordinator applies its existing retry budget. With `--max-retries 1`, an
invalid argument is tried twice and then becomes FAILED; retries do not fix
invalid input. Retried jobs join the FIFO's back. Every attempt reports the
original job ID, current worker ID, and current 64-bit attempt number.

SIGINT/SIGTERM, coordinator EOF/reset, invalid incoming frames, and send errors
all pass through cleanup. Cleanup sets an atomic cancellation flag and joins
the task thread before its storage goes away. Long sleep and prime-count tasks
check that flag repeatedly. Fibonacci and hash are bounded short computations
and check before publishing their result. No detached task survives worker
shutdown, and no result is fabricated for cancelled work.

The task thread inherits blocked SIGINT/SIGTERM signals; the main thread restores
its prior mask after creation. The signal handler only sets the main thread's
stop flag. Ordinary main-thread code performs cancellation, joining, and socket
cleanup. A local stop exits with status 0; a connection/protocol error exits 1.
An in-progress network helper can still use its existing five-second budget.

The coordinator sees unfinished disconnected work as WORKER_LOST and retries or
fails it. A partitioned/paused worker can overlap a retry until it detects the
lost connection, so attempt identity and repeatable tasks still matter. There is
no exactly-once guarantee, completion acknowledgment, lease deadline independent
of heartbeats, or automatic reconnect. Accepted state is now durable through
the [coordinator WAL](persistence.md).

## Result visibility and verification

On DONE, the coordinator appends `result="..."` to its job log. Printable ASCII
is readable directly; quotes, backslashes, control characters, and non-ASCII
bytes use `\xHH` escapes. It uses the recorded byte count, never `%s` on an
untrusted result. Raw bytes remain unchanged in the job record. This also
preserves safe single-line logs for controlled peers returning binary data.

Run `make test-execution` for the 11 process scenarios, or
`make SANITIZE=1 test-execution` for AddressSanitizer/UBSan binaries. These include
sleep completion and FIFO reuse, all four real results, two concurrent workers,
literal wire reports and maximum binary input, invalid arguments, retry exhaustion,
heartbeats during sleep/CPU work beyond a 500 ms timeout, cancellation on disconnect
and signals, a replacement finishing an interrupted task, and escaped result logs.

`tests/test_tasks.c` adds seven C groups: sleep timing, known prime counts,
Fibonacci boundaries, known hash vectors, invalid inputs with unchanged outputs,
pre-cancellation, and cancellation of running sleep/prime work. They run through
`make test-unit`. Full suites use `make test` and `make test-sanitize`.
