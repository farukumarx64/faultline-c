# Reproducible worker-crash demo

One job keeps its ID when its worker is killed. An already-connected worker
runs attempt 2 and returns the correct result.

![A real terminal replay: one job moves to the surviving worker after SIGKILL and finishes on attempt 2](demos/recovery.gif)

[Terminal recording](demos/recovery.cast) · [Capture evidence](demos/recovery.json) ·
[Demo runner](../demos/recovery.py)

## Run it yourself

From the project root on macOS or Linux, with Make, a C11 compiler, and Python
3.9 or newer:

```sh
make demo-recovery
```

This builds the real executables and runs the demonstration. Expect roughly
15 seconds with the default settings. There is no recording software to install
for this command: the runner uses the Python standard library and saves an
asciicast v2 recording automatically.

Each run selects an available loopback port and creates a fresh WAL and evidence
directory under `build/demos/recovery-*`. It leaves an existing coordinator and
its log alone. The endpoint, PIDs, and worker identities are discovered from the
live run. The runner identifies the busy worker before signaling it.

To choose an evidence directory or use sanitizer builds:

```sh
make demo-recovery DEMO_ARGS='--output-dir build/demos/my-recovery'
make SANITIZE=1 demo-recovery
```

Explicit output directories must be new. Keep earlier evidence when repeating
the demo. `make clean` removes `build/`, so archive evidence you want to retain.

## What the recording shows

| Stage | Actual operation and check |
| --- | --- |
| Two workers ready | Launch a coordinator and two workers; verify both workers are alive and idle before submission. |
| One job running | Submit `sleep --args 6000 --max-retries 1`. Record the ACK and query the job until it is `RUNNING` on attempt 1. |
| Hard crash | Refresh the job's ownership, then send `SIGKILL` to that owned worker process. Confirm its exit and the coordinator's worker-loss event. |
| Same job, new worker | Verify the previously connected survivor is running the same ID on attempt 2, with `retries=1/1`. No replacement worker is launched. |
| Verified completion | Require `DONE`, the survivor's worker ID, attempt 2, one retry, and exactly `result="slept_ms=6000"`. Confirm cleanup before displaying PASS. |

The display contains actual CLI output and clearly marked `#` narration. The
`kill -KILL PID` line describes the signal sent through the runner's owned process
handle. The GIF is an offline rendering of this recorded terminal output, using
its original timestamps; it is not a recording of someone typing commands.
Reading pauses are part of the run. GIF timestamps are rounded to centiseconds.

Background inspection queries and process logs remain in the evidence directory.
The display shows selected snapshots so the important ID, worker, attempt, and
result stay readable. It does not print an invented `QUEUED` snapshot: reassignment
can happen between CLI polls. The coordinator's durable transition history proves
the intermediate requeue.

## Acceptance checks and cleanup

The runner reuses the existing batch/chaos harness for child-process ownership,
CLI deadlines, diagnostic checks, and cleanup. It verifies more than the last
screen:

- Exactly one acknowledged ID, two workers, and one deliberate crash.
- Seven ordered durable job events: submission, assignment, start, worker loss,
  reassignment, restart of execution, and completion. WAL sequence numbers advance.
- Exactly one retry and one accepted completion under a different worker ID.
- The surviving worker is the original process, with the same PID; no replacement
  is started. Repeated full status queries, job/worker listings, and counters agree.
- All owned children are reaped and their process groups have disappeared.

The default process deadline is **60 seconds**, including a **10-second cleanup
reserve**. Any incorrect result, unexpected worker loss, missing transition,
deadline, diagnostic, or cleanup failure fails the run. Forced teardown is reported
as a cleanup failure. Ctrl+C and SIGTERM request bounded teardown, preserve evidence,
and return an interrupted exit status. The final recording reading pause happens
after process cleanup.

Success exits 0; verification failure exits 1; invalid arguments exit 2;
SIGINT/SIGTERM interruptions exit 130/143. A failed recording is retained, but the
GIF renderer refuses to publish it as a successful demonstration.

Useful retained artifacts:

| File | Purpose |
| --- | --- |
| `recovery.cast`, `transcript.log` | Timed terminal replay and readable displayed output |
| `summary.json`, `manifest.json` | Verdict, configuration, binary hashes, source revision, and cleanup evidence |
| `submissions.json`, `final-snapshots.json`, `accounting.json` | Acknowledged ID, exact result, and matching counters |
| `events.jsonl`, `fault-actions.json`, `recovery-history.json` | Owned process events, the actual signal, and durable job transitions |
| `*.stdout.log`, `*.stderr.log`, `coordinator.wal` | Full executable output and the fresh coordinator log |

## Render a new GIF

This optional step requires **Python 3.10+**, Pillow, and a monospaced font. The
runtime and demo do not depend on Pillow. Use a Python 3.10+ interpreter for the
venv command below; macOS's older system Python can still run the demo itself.

```sh
python3 -m venv build/demo-render-venv
build/demo-render-venv/bin/python -m pip install -r demos/requirements-render.txt

# Replace the directory with the one printed by your successful run.
build/demo-render-venv/bin/python demos/render_recovery.py \
    build/demos/my-recovery/recovery.cast \
    --output build/demos/my-recovery/recovery.gif
```

The renderer reads the successful summary beside the recording, checks the final
screen against the verified result, and writes a looping GIF and a compact JSON
evidence report. When exporting elsewhere, it also copies the `.cast` beside the
GIF. Existing outputs are refused. Menlo is detected on macOS and DejaVu Sans Mono
on Linux; pass `--font /path/to/monospace.ttf` to select another font. An asciicast
player can also replay `recovery.cast` directly.

The checked-in files under `docs/demos/` preserve one captured run. Its JSON report
records the exact binary hashes, source revision/dirty state, signal target,
terminal status, transitions, cleanup count, and hashes of the local raw evidence.

## Scope and verification

This demonstrates **hard-crash recovery through TCP connection loss**. It does not
measure heartbeat-expiry recovery or coordinator restart, and it does not claim
exactly-once execution. The retry starts the task again from the beginning. See
[worker recovery](recovery.md) for the lease and retry guarantees.

The demo is a correctness demonstration, not a benchmark. It runs on AC or battery;
its display delays and elapsed time are not performance results.

```sh
make test-demo-recovery
make SANITIZE=1 test-demo-recovery
```

These five regression checks cover successful recovery and recorded output,
rejection of an incorrect result of the same length, deadline cleanup, SIGTERM
cleanup during an active attempt, and preservation of an existing evidence
directory. They are separate from `make test` and the benchmark campaigns.

The checked-in capture lasts **14.81 seconds**: job **1** moves from worker **1**
to the already-connected worker **2** and finishes on attempt **2**, with **1/1**
retries and result `slept_ms=6000`. Its cleanup reaped all **48** owned children
(including CLI and metadata helpers), with no remaining process groups. All five
demo regression checks passed in both normal and ASan/UBSan builds. Separate
renderer checks rejected a failed summary, an incomplete recording, a mismatched
final result, and an existing output file.
