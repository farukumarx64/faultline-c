# Worker scaling benchmark

The full scaling runner implements the fixed
[benchmark contract](benchmarks.md) using the same verified workload and timing
boundaries as the [one-worker baseline](benchmark-baseline.md). Measurement is
pending; no scaling results are claimed by the implementation alone.

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
