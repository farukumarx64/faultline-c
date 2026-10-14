# CPU scaling across power modes

**All three campaigns and both evidence audits passed.** On this Apple M4, normal
battery performance was close to AC. Low Power Mode increased median batch time
by **75–99% relative to normal battery operation**, depending on worker count.
These are measurements of the specified workload, not battery-life or energy-use measurements.

Campaigns ran on **October 14, 2026 (UTC)** from clean revision
[`08f5d23faa63be562f0cae9ed2a29bf19b0bae51`](https://github.com/farukumarx64/faultline-c/commit/08f5d23faa63be562f0cae9ed2a29bf19b0bae51).
The later documentation commit does not change the revision actually measured.
The original [October 1 AC scaling report](benchmark-scaling.md) remains a separate historical result.

[Reviewed JSON](../benchmarks/results/power-scaling-20261014.json) ·
[Comparison CSV](../benchmarks/results/power-scaling-20261014.csv) ·
[All 72 runs, including warmups](../benchmarks/results/power-scaling-20261014-samples.csv)

## Workload and timing

Each run submitted **64 `prime_count(10000000)` jobs**, verified to return exactly
`664579`, with zero allowed retries and no injected failures. Each power campaign
used one warmup at each of 1, 2, 4, and 8 workers, followed by five rotated measured
rounds. That is 24 runs per campaign and **4,608 verified jobs across 72 runs**:
3,840 measured jobs and 768 warmup jobs. Warmups are excluded from the statistics.

Batch time starts immediately before the first submission and ends when the CLI
observes every job completed. It includes submission, execution, durable writes,
and completion observation. Pool startup, final result verification, and cleanup
are outside that interval. Throughput is 64 divided by batch duration. Job latency
runs from coordinator acceptance to durable completion and includes queue waiting.
The [workload contract](benchmarks.md) and [power contract](benchmark-power.md)
define the exact sample order, timing boundaries, power checks, and acceptance rules.

## Results

Values are medians of five measured runs. Brackets show the observed batch-time
minimum–maximum; full values and ranges for every metric are retained in JSON/CSV.
Average latency is the median of the five per-run means. Speedup uses the same
power campaign’s one-worker median as its reference.

| Power condition | Workers | Batch time, seconds [range] | Jobs/s | Average latency, seconds | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| AC, normal | 1 | 58.751 [57.691–59.680] | 1.089 | 28.904 | 1.000× |
| AC, normal | 2 | 29.862 [29.700–33.566] | 2.143 | 14.516 | 1.967× |
| AC, normal | 4 | 15.989 [15.803–16.155] | 4.003 | 7.456 | 3.675× |
| AC, normal | 8 | 13.575 [13.451–13.600] | 4.715 | 6.531 | 4.328× |
| Battery, normal | 1 | 56.344 [56.136–57.043] | 1.136 | 27.646 | 1.000× |
| Battery, normal | 2 | 29.435 [29.181–29.719] | 2.174 | 14.144 | 1.914× |
| Battery, normal | 4 | 15.867 [15.789–15.905] | 4.034 | 7.496 | 3.551× |
| Battery, normal | 8 | 13.444 [13.228–13.857] | 4.761 | 6.407 | 4.191× |
| Battery, Low Power Mode | 1 | 111.919 [111.779–112.094] | 0.572 | 55.764 | 1.000× |
| Battery, Low Power Mode | 2 | 56.179 [55.947–56.365] | 1.139 | 27.914 | 1.992× |
| Battery, Low Power Mode | 4 | 29.793 [29.504–31.731] | 2.148 | 14.999 | 3.757× |
| Battery, Low Power Mode | 8 | 23.580 [22.578–25.183] | 2.714 | 12.239 | 4.746× |

![CPU scaling across AC, normal battery, and Low Power Mode: all five measured batch times and throughputs at each worker count, with median lines](../benchmarks/results/power-scaling-20261014.svg)

[SVG](../benchmarks/results/power-scaling-20261014.svg) · [PNG](../benchmarks/results/power-scaling-20261014.png)

All five measurements are plotted. Small horizontal offsets separate conditions
at the same worker count. Connected markers are medians, not fitted performance curves.

| Workers | Battery batch-time change vs AC | Low Power Mode batch-time change vs battery |
| ---: | ---: | ---: |
| 1 | -4.10% | +98.63% |
| 2 | -1.43% | +90.86% |
| 4 | -0.76% | +87.77% |
| 8 | -0.97% | +75.40% |

Normal battery batches finished 0.8–4.1% sooner than AC in these campaigns. This
small difference does not establish that unplugging improves performance: source
and mode were measured in separate campaigns with unmeasured thermal conditions.

Low Power Mode had a much larger observed effect. At eight workers, median batch
time increased from 13.444 to 23.580 seconds compared with normal battery, and
median throughput fell from 4.761 to 2.714 jobs/s (about 43%). At one worker,
batch time nearly doubled. Average accepted latency also increased, including
the time jobs spent waiting in the FIFO queue.

Additional workers helped in every condition. Going from four to eight workers
increased median throughput by 17.8% on AC, 18.0% on normal battery, and 26.3% in
Low Power Mode. The latter’s 4.746× eight-worker speedup is relative to its own
slower one-worker run; it does not mean its absolute throughput exceeded AC.

No slow valid sample was removed. For example, the AC two-worker run in round 3
was 33.566 seconds, and Low Power Mode eight-worker samples ranged from 22.578
to 25.183 seconds. Five repetitions describe observed variation, not a strong
confidence interval or an isolated explanation of a bottleneck.

## Machine, build, and observed conditions

| Property | Recorded value |
| --- | --- |
| Machine | Apple M4, `Mac16,12`, arm64 |
| CPU cores | 10: 4 Performance + 6 Efficiency |
| RAM | 16 GiB |
| OS | macOS 27.0.1, build 26A434; Darwin 27.0.0 |
| Compiler | Apple Clang 21.0.0, `clang-2100.3.34.2` |
| Build | `-O2 -g -Werror`, `SANITIZE=0`; normal logging and WAL synchronization |
| Storage | Internal APFS SSD |
| Tools | Python 3.9.6, GNU Make 3.81 |
| Thermal telemetry | Unavailable at all 144 sample boundaries |

| Campaign | Whole invocation | Observed sample-boundary charge | Source / mode at all 48 boundaries |
| --- | ---: | --- | --- |
| AC | 28.736 min | 47% → 59%; actively charging | AC / Low Power Mode off |
| Battery | 28.486 min | 99% throughout | Battery / Low Power Mode off |
| Battery, Low Power Mode | 39.314 min | 97% → 92% | Battery / Low Power Mode on |

Whole-invocation time includes building, correctness tests, warmups, cooldowns,
verification, and cleanup. It is separate from the measured per-batch time.

The operator confirmed that the normal battery campaign stayed unplugged with
Low Power Mode off throughout. The unchanged 99% reading is retained as observed;
its cause was not established. Charge percentages and the differing invocation
durations do not establish energy consumption, battery life, or energy efficiency.

## Validation and evidence

- Every campaign used the same clean source revision, source hashes, machine,
  compiler, and build settings. Binary hashes remained stable within each campaign.
- Each fresh optimized build passed 134 C groups and 153 executed process checks.
  Two default-endpoint checks skipped because automatic ports were used: 155
  process checks were discovered in each campaign. No sanitizer timings were mixed in.
- All 4,608 jobs completed on attempt 1, with zero retries and terminal failures.
  Acknowledgment ledgers, listings, terminal results, counters, and lifecycle logs agreed.
- Re-running the comparison audit reproduced the saved JSON, CSV, and Markdown exactly.
  A separate audit recomputed log-derived latencies and metrics, checked the prescribed
  sample order and 30-second cooldowns, and verified deadlines and cleanup.
- All 41,520 tracked children/helpers were recorded reaped with retired process groups.
  The 270 coordinator EOF notices were attributed to intentional shutdown of owned,
  idle workers after successful verification. No unexpected runtime errors were accepted.

The reviewed JSON includes all per-run metrics, aggregate values, power observations,
provenance hashes, independent-audit counts, and the operator confirmation. The review
hashed retained WALs but did not independently replay them; it is not a new crash-recovery
or storage-loss experiment.

Raw evidence lives locally in `build/benchmarks/power-ac/`, `power-battery/`,
`power-battery-low-power/`, `power-comparison/`, and `power-review-y65_56fu/`.
A verified archive is retained outside `build/` at
`.benchmark-evidence/power-scaling-20261014.tar.gz`, with a SHA-256 checksum and
per-file manifest alongside it. This ignored directory survives `make clean`.
The raw archive and executables are not included in Git; the compact reviewed
reports and graph are. Copy the archive to independent storage for backup.

## Reproduce and interpret

Use the [three scaling commands and preparation procedure](benchmark-power.md#running-the-campaigns-yourself)
and then the [comparison command](benchmark-power.md#compare-completed-campaigns).
For the recorded implementation, use a clean checkout of `08f5d23`; use the same
revision for all three campaigns and fresh output directories. A different OS,
compiler, machine, or revision constitutes a new comparison, not an additional
sample in these results. The original workload and power profile JSON files are versioned.

To redraw the graph from the reviewed JSON, install the optional plotting dependency
in a virtual environment (Matplotlib 3.9.4 was used for this report):

```sh
python3 -m venv /tmp/faultline-power-plot
/tmp/faultline-power-plot/bin/python -m pip install matplotlib==3.9.4
/tmp/faultline-power-plot/bin/python benchmarks/plot_power.py \
  benchmarks/results/power-scaling-20261014.json \
  --output-prefix /tmp/faultline-power-plot/power-scaling-20261014
```

The plotting dependency is optional and is not needed to build Faultline, run
campaigns, or audit comparisons. The graph script is in the documentation revision;
the measured revision predates that script and these published artifacts.

These sequential single-machine campaigns do not isolate temperature, OS scheduling,
background activity, charge level, or charging effects. Before/after source and mode
checks are not continuous monitoring. The recommended pre-campaign idle cooldown and
background-app setup are operator procedures, not verified evidence. Missing thermal
data means throttling cannot be ruled out. Results are specific to this CPU workload;
**battery and Low Power Mode recovery campaigns remain unmeasured**.
