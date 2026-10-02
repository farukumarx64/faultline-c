# Worker scaling benchmark

The full scaling runner implements the fixed
[benchmark contract](benchmarks.md) using the same verified workload and timing
boundaries as the [one-worker baseline](benchmark-baseline.md). The complete
24-run campaign and independent audit passed; measurements are recorded below.

## Results: campaign started 2026-10-01 UTC

**The full scaling campaign and independent audit passed.** All 1,536 jobs
(256 warmup, 1,280 measured) returned exactly `664579` on attempt 1, with zero
retries or terminal failures. Every run met its deadline and passed cleanup.

The [reviewed JSON report](../benchmarks/results/scaling-20261001.json) retains
unrounded values, sample provenance, machine observations, and evidence hashes.
The [CSV](../benchmarks/results/scaling-20261001.csv) contains all 24 runs, with
warmups explicitly labelled. Times below are seconds; reported metrics are
medians of the five measured runs at each worker count.

| Workers | Batch time | Observed batch range | Jobs/second | Average job latency | Speedup | Worker-normalized efficiency |
| ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 1 | 59.008 | 58.237–60.805 | 1.085 | 29.076 | 1.000× | 100.00% |
| 2 | 29.856 | 29.712–29.996 | 2.144 | 14.457 | 1.976× | 98.82% |
| 4 | 16.924 | 16.673–17.700 | 3.782 | 7.894 | 3.487× | 87.17% |
| 8 | 13.942 | 13.851–16.061 | 4.590 | 6.717 | 4.232× | 52.91% |

All five measured batch times, in measured-round order:

| Workers | Round 1 | Round 2 | Round 3 | Round 4 | Round 5 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 58.237 | 60.805 | 59.008 | 58.643 | 59.389 |
| 2 | 29.996 | 29.808 | 29.856 | 29.931 | 29.712 |
| 4 | 17.107 | 17.700 | 16.673 | 16.924 | 16.717 |
| 8 | 14.130 | 16.061 | 13.851 | 13.888 | 13.942 |

Variation in the remaining metrics is retained too:

| Workers | Jobs/second range | Per-run mean latency range, seconds | Median per-run p95 latency, seconds |
| ---: | --- | --- | ---: |
| 1 | 1.053–1.099 | 28.472–30.573 | 54.558 |
| 2 | 2.134–2.154 | 14.327–14.498 | 27.127 |
| 4 | 3.616–3.839 | 7.878–8.536 | 14.437 |
| 8 | 3.985–4.620 | 6.677–7.868 | 11.594 |

Two workers nearly doubled throughput. Four workers delivered 3.49× the
one-worker throughput; eight delivered 4.23×. Moving from four to eight workers
improved median throughput by approximately **21.4%**, despite doubling the
worker count. This is diminishing improvement for this workload and machine,
not evidence that eight workers should provide an eightfold improvement.

Average accepted latency fell from 29.076 to 6.717 seconds. This includes FIFO
queue waiting: additional workers let queued jobs start sooner. It does not
mean an individual prime-count computation became 4.33 times faster.

All five samples are retained, including the slower 16.061-second eight-worker
run in round 2. Its after-run one-minute system load was 16.841, the highest
recorded boundary load; the lowest was 2.001. Load includes benchmark processes
and other activity, and this experiment does not attribute the slowdown to a
specific cause. The median limits the influence of one slower sample without
removing that observation.

## Machine and validation evidence

The campaign ran from clean local revision
`6eccc341af0974c802c992ba9afeee1285eca0a8`. That implementation commit was made
before timing to meet the committed-source contract. Source/binary hashes, Git
revision, and hardware/software identity remained unchanged during the campaign.

| Property | Recorded value |
| --- | --- |
| Machine | Apple M4, `Mac16,12`, ARM64 |
| Cores | 10 physical/logical: 4 Performance + 6 Efficiency |
| RAM | 16 GiB |
| OS | macOS 27.0.1, build 26A434; Darwin 27.0.0 |
| Compiler | Apple Clang 21.0.0, `clang-2100.3.34.2` |
| Build | `-O2 -g -Werror`, `SANITIZE=0`, normal WAL synchronization |
| Storage | Internal APFS SSD |
| Power | AC and low-power mode off at all 48 checked boundaries |
| Thermal telemetry | Unavailable; absence of throttling is not established |

The four warmups took 57.186, 29.604, 16.838, and 14.411 seconds at 1/2/4/8
workers respectively. The full invocation took 1,743.130 seconds (about 29 minutes),
including building, correctness checks, cooldowns, verification, and cleanup.
This total invocation time is separate from each measured batch duration.

Validation completed:

- 25 benchmark regression checks passed, including real 2/4/8-worker execution.
- The fresh optimized build passed 131 C test groups and 141 process integration
  tests; two default-endpoint tests skipped because automatic ports were used.
- All 24 samples passed per-ID accounting, exact results, owner/attempt checks,
  repeated status checks, counters, deadlines, and cleanup. Every registered
  worker completed work in each run.
- The independent audit recomputed timings, mean/p95 latencies, ranges, and
  speedups from raw evidence, checked the prescribed sample order and cooldowns,
  and verified all 11,909 directly tracked children/helpers were recorded reaped
  with retired process groups.

The coordinator logged 90 `worker_dead reason=eof` warnings when idle workers
were intentionally stopped during cleanup. The audit attributed each one to its
owned worker: final ALIVE/idle status, completed job history, cleanup SIGTERM,
worker `stopped reason=signal signal=15 exit_code=0`, and successful reaping.
These are shutdown notices after work finished. Other warnings/errors are not
permitted; none were accepted. No runtime code or acceptance rule was relaxed
to obtain passing samples.

Raw evidence remains under Git-ignored `build/benchmarks/scaling-0yn9f77x/`,
including `independent-audit.json` and its `audit_scaling.py` script. The earlier
`scaling-dirty-source-check` was a deliberate negative preflight check: it
rejected uncommitted source before starting any workload. No timed campaign
sample was replaced. No sanitizer performance measurements were mixed into this
optimized campaign.

These results describe a single-host fixed workload. CPU placement, unequal M4
core classes, other applications, coordinator work, CLI polling, and durable
writes can all affect scaling; this experiment does not isolate a bottleneck.
Five repetitions show observed variation rather than a strong confidence
interval. Multi-host scaling and recovery overhead remain separate experiments.

## Run the campaign

On macOS with AC power connected, low-power mode off, and a clean committed
checkout:

```sh
make benchmark-scaling
# Optional new evidence directory; existing paths are rejected:
make benchmark-scaling BENCHMARK_ARGS='--output-dir build/benchmarks/my-scaling'
```

The command calls `benchmarks/run_baseline.py --scaling`. Shared implementation
keeps submission, result verification, timing, logging, and cleanup identical to
the one-worker runner. It creates fresh optimized Clang binaries with
`-O2 -g -Werror`, `SANITIZE=0`, then runs the core correctness suite before timing.
The workload remains 64 `prime_count(10000000)` jobs, each returning exactly
`664579`, with zero retries and no injected failures. Normal WAL persistence and
logs stay enabled. The production C code is unchanged.

The plan has four warmups in worker-count order 1/2/4/8, followed by five measured
rounds:

| Round | Worker-count order |
| --- | --- |
| 1 | 1, 2, 4, 8 |
| 2 | 2, 4, 8, 1 |
| 3 | 4, 8, 1, 2 |
| 4 | 8, 1, 2, 4 |
| 5 | 1, 4, 2, 8 |

That is 24 fresh runs and 1,536 individually verified jobs; 20 runs and 1,280 jobs
contribute to reported measurements. The earlier one-worker development samples
are historical context, not the denominator for this campaign's speedup. Each
run creates its own coordinator, WAL, and worker pool. There is a 30-second
cooldown after cleanup between runs. Every warmup must take at least ten seconds.

## Verification and ownership

The worker registry must contain precisely the owned pool, with unique IDs and
healthy idle workers before submission and after completion. Every acknowledged
job ID must appear in the final verified set; matching totals alone do not pass.
Each job must have matching assignment, start, and completion under one owned
worker ID on attempt 1. Jobs may belong to different workers, but no job can
silently change owners. Every full result is checked twice against the exact
expected bytes. Logs, listings, statuses, and counters must agree.

The runner rejects incomplete, duplicate, reordered, or invalid samples before
computing aggregates. A failed warmup or measured run stops the campaign, retains
its evidence, and prevents a reportable result. A new attempt must repeat the
entire plan; slow valid samples cannot be dropped or replaced.

The checkout must be clean at entry and after every sample, with its revision,
source hashes, and binary hashes unchanged. Hardware, OS, kernel, storage, and
Python identity must match across all before/after machine snapshots. Power,
thermal warnings, and free-space checks use the same rules as the baseline.
Load observations may vary and are retained for interpretation. Only a successful
parent summary sets `reportable_scaling_campaign=true`; an individual sample
cannot establish that the complete matrix passed.

## Metrics and variation

Batch time runs from just before the first submission CLI to the first parsed
job listing proving every acknowledged job DONE. It includes serial admission,
execution, durability, logging, and polling delay. Startup, final verification,
and teardown are outside that metric. Average job latency is coordinator
completion minus acceptance time, including time waiting in the FIFO queue.

Each worker count retains five raw values, median, minimum, and maximum for batch
time, jobs/second, mean job latency, and nearest-rank p95 job latency. Latencies
are summarized per run, not pooled into an apparently larger independent sample.

`speedup(W) = median_batch_time(1) / median_batch_time(W)`

`worker_normalized_efficiency_percent(W) = 100 * speedup(W) / W`

Speedup uses the ratio of medians, not the median of per-round ratios. A speedup
of 2 means the same 64-job batch takes half as long. Efficiency is a descriptive
normalization: the M4's performance/efficiency cores are unequal, and workers
share the machine with the coordinator, CLI, harness, and other applications.
Five repetitions show observed variation, not a strong confidence interval.

## Evidence, deadlines, and checks

Each campaign has a unique Git-ignored `build/benchmarks/scaling-*` directory.
It retains source/build provenance, copied inputs, the full ordered plan, logs,
and a parent summary. Per-run directories identify position, stage, measured
round, and worker count, for example `05-measured-r1-w2`. They retain the same
ACK ledger, statuses, accounting, timings, latencies, machine snapshots, event
trace, WAL, process ledger, and cleanup evidence as the baseline.

The whole invocation has a two-hour deadline, including build/tests and cooldowns.
Each run has a 600-second deadline with ten seconds reserved for cleanup. SIGINT
and SIGTERM use the existing bounded teardown. Cleanup failure invalidates a
sample even after all jobs finish. `make clean` removes raw evidence; reviewed
reports belong under `benchmarks/results/`.

`make test-benchmark-harness` now runs 25 checks. Added coverage exercises the
exact rotated plan, ratio-of-medians calculation, invalid/incomplete campaigns,
ownership across multiple workers, dirty-source rejection, machine-identity
comparison, and real 2/4/8-worker execution. The small process fixtures and their
elapsed times are correctness checks, not scaling samples.
