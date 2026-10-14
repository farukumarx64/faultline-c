# Battery and Low Power Mode benchmarks

Status: **runner, validation, comparison reports, and regression tests implemented;
new performance measurements pending.** No battery or Low Power Mode timings
have been published. The existing AC-only reports remain historical results.

The versioned [power profiles](../benchmarks/profiles/power-v1.json) extend the
shared benchmark runner. They do not change Faultline execution or persistence.
Keep the [scaling](benchmark-scaling.md) and [recovery](benchmark-recovery.md)
workloads separate: CPU scaling measures computation throughput; recovery uses
waiting tasks to isolate hard-crash and heartbeat-expiry behavior.

## Three separate conditions

| CLI profile | Power source | Low Power Mode | Initial charge | Minimum observed charge |
| --- | --- | --- | --- | --- |
| `ac` (default) | AC | Off | Not constrained | Not constrained |
| `battery` | Battery | Off | 90–100% | 20% |
| `battery-low-power` | Battery | On | 90–100% | 20% |

The power contract is `faultline-power-v1`, independent of the unchanged
`scaling-prime-count-v1` and `recovery-sleep-v1` workload contracts. The conditions
have IDs `ac-normal-v1`, `battery-normal-v1`, and `battery-low-power-v1`.
Only these fixed profiles are accepted; unsupported settings need a new version.

Compare AC to battery with Low Power Mode off to study the source change.
Compare battery to battery-low-power to study Low Power Mode while holding the
source constant. The latter comparison is not a battery-life or joules-per-job
measurement. Battery percentages are coarse condition checks, not energy meters.
AC with Low Power Mode on is outside this first matrix.

## Preserve the workload and build

For scaling, every power profile runs the same 64 `prime_count(10000000)` jobs at
1, 2, 4, and 8 workers. Each result must be `664579`. The four warmups and five
rotated measured rounds are unchanged: 24 fresh runs, 1,536 verified jobs per
campaign. The full three-profile comparison contains 4,608 jobs, with warmups
excluded from statistics.

For recovery, every profile runs the same 32 `sleep(10000)` jobs on four workers,
with one retry allowed. No-fault, SIGKILL, and heartbeat-expiry scenarios keep
their separate controls and fault schedules. Three warmups plus five rounds give
18 runs and 576 verified jobs per power profile. Do not merge crash and expiry
observations or combine recovery with CPU-scaling throughput.

Builds remain Clang `-O2 -g -Werror`, sanitizers off, with normal WAL sync and
runtime logs. Each campaign builds fresh binaries and runs the correctness
suite before timing. The checkout must be clean and committed for full scaling
and recovery campaigns. Source, binary hashes, machine identity, expected job
results, sample order, retry rules, deadlines, and process cleanup retain their
existing checks. Do not edit code or upgrade the OS/compiler between profiles.

The historical AC scaling and recovery reports predate request deduplication and
this runner change. Use them as historical context, not as the controlled AC leg
of a new power comparison. **Rerun AC on the same committed revision and machine
as the two battery campaigns.** The comparison tool rejects different revisions,
source hashes, compiler/build settings, or machine identities. Binary hashes must
stay unchanged within each campaign; separate builds can have different hashes
because debug information includes their different build paths.

## What the runner observes

Before expensive build/testing, a preflight reads power conditions and checks the
initial charge range. Before the first warmup it checks that range again, so
battery use during the build cannot silently start the campaign below 90%.
Every sample then records source, Low Power Mode, charge, and available thermal
information before starting the pool and after final result verification. The
20% floor applies to every observed battery reading.

Each observation uses owned, bounded `pmset` helpers:

1. `pmset -g batt` establishes source and charge.
2. `pmset -g custom` supplies the settings for that source, with its section
   parsed independently of section order and the other source's values.
3. `pmset -g live` confirms the effective Low Power Mode setting agrees.
4. `pmset -g therm` records available thermal/performance information.
5. Another `pmset -g batt` brackets the settings reads; a source change fails.

Raw output and parsed observations are retained even if validation fails.
Unknown/ambiguous source, charge, mode, inconsistent effective settings, a wrong
profile, or an observed low charge invalidates the sample. Known thermal warnings
invalidate all profiles. When Low Power Mode is intentionally on, reported CPU
limits below 100% are retained rather than mistaken for an unexpected mode;
explicit thermal/performance warnings still fail. Missing thermal telemetry is
recorded as unavailable, not as proof of an unthrottled machine.

Checks are **at boundaries, not continuous monitoring**. A brief unplug/replug,
mode change, or thermal event between observations may go undetected. The 20%
floor is an observed threshold, not a guarantee of the charge at every instant.
Keep the configured condition stable for the whole campaign. If you know it
changed, interrupt with Ctrl-C. If you discover the change after completion,
mark that retained campaign invalid:

```sh
printf '%s\n' 'Known power-mode change between observations; exclude this campaign.' \
  > build/benchmarks/power-battery/INVALID
```

The comparison tool rejects an `INVALID` marker at campaign or sample level.
Use a fresh directory for another complete attempt; never replace only the
failed or slow samples. Existing AC-only published artifacts are unchanged.

## Running the campaigns yourself

Commit the implementation first, then use the same clean revision for every
campaign in a comparison. Keep the lid open, avoid concurrent builds/workloads,
and use the same display/background-app setup. Charging can change temperatures:
allow at least 15 minutes of idle cooldown after charging or changing the power
condition before starting each campaign. This is an operator procedure, not an
automatic guarantee of thermal equilibrium. The runner still applies the existing
30-second cooldown between individual runs.

Before each battery campaign, charge to 100% when practical, then unplug and
allow the cooldown. Verify that charge is still at least 90% before starting.
Recharge and cool down again before the other battery campaign. Do not plug in
mid-campaign to extend it. If a complete campaign cannot finish above the observed
floor, retain the failure and define a new *shared* workload/profile; do not shorten
only the battery condition or silently lower the threshold.

Choose the source-specific mode in **System Settings → Battery**. Depending on
the model and macOS version, it appears as Low Power Mode or an Energy Mode
selection for battery/power adapter. Select normal/Automatic with Low Power Mode
off for the first two conditions, and Low Power for the third. The runner reads
settings and never changes them. See [Apple's power-mode guide](https://support.apple.com/en-us/101613).

Readiness checks (read-only):

```sh
pmset -g batt
pmset -g custom
pmset -g live
```

Run these one at a time, changing the physical source and mode between them.
Each output directory must be new:

```sh
# Connected to AC; Low Power Mode off.
make benchmark-scaling BENCHMARK_ARGS='--power-profile ac --output-dir build/benchmarks/power-ac'

# Unplugged; Low Power Mode off; start at 90–100% charge.
make benchmark-scaling BENCHMARK_ARGS='--power-profile battery --output-dir build/benchmarks/power-battery'

# Recharge, unplug, cool down; Low Power Mode on; start at 90–100% charge.
make benchmark-scaling BENCHMARK_ARGS='--power-profile battery-low-power --output-dir build/benchmarks/power-battery-low-power'
```

The previous AC scaling campaign took about 29 minutes including build, checks,
and cooldowns. Allow more time for the new battery profiles; their timings are
not measured yet. Each campaign has the existing two-hour hard deadline, each
sample ten minutes, and cleanup keeps its ten-second reserve. The runner stops
on the first invalid sample and preserves its evidence.

For a separate recovery comparison, replace `benchmark-scaling` with
`benchmark-recovery` and use three different output directories, such as
`power-recovery-ac`, `power-recovery-battery`, and `power-recovery-battery-low-power`.
The earlier AC recovery campaign was roughly 40 minutes; battery timing is
unknown. `benchmark-baseline` also accepts the flag, but its one-worker development
series is not accepted as a complete scaling comparison.

Without `--output-dir`, new runs go under `build/benchmarks/<power-profile>/`.
Ordinary correctness tests do not require any physical power setting.

## Compare completed campaigns

```sh
make benchmark-compare-power POWER_COMPARE_ARGS='--ac build/benchmarks/power-ac --battery build/benchmarks/power-battery --battery-low-power build/benchmarks/power-battery-low-power --output-dir build/benchmarks/power-comparison'
```

The command also accepts three recovery directories, but all inputs must have
one matching workload. It reads retained evidence and does not execute jobs or
change the power mode. It rejects incomplete/failed campaigns, incorrect source
or build identity, changed binaries/source snapshots, mixed power labels,
invalid power observations, missing/duplicate job IDs, incorrect results/retries,
inconsistent timing metrics, and cleanup failures. It recomputes aggregates from
the complete ordered sample set and checks them against campaign summaries.
It checks verifier evidence; it does not independently replay the WAL or prove
that files were never edited. Hashes in the report identify the consumed evidence.

A successful comparison writes a new directory containing:

- `comparison.md`: readable tables of medians and min–max variation.
- `comparison.csv`: numerical metrics and percentage changes versus AC and
  versus battery with Low Power Mode off.
- `comparison.json`: all five values behind each aggregate, per-profile results,
  machine/build identity, charge observations, artifact hashes, and limitations.

Each power condition keeps its own aggregates and worker scaling speedups.
Percentage change is `100 × (candidate median / reference median − 1)`;
positive batch-time/latency changes mean slower results, while positive throughput
changes mean more completed jobs per second. A zero or negative reference has no
percentage reported. Recovery's paired overhead values remain separate from its
absolute timing metrics. Samples are never pooled across power profiles.

This first design uses sequential campaigns; source/mode can be confounded with
battery discharge, temperature, background activity, and order. Five repetitions
show variation but do not establish a universal causal effect or energy savings.
Raw evidence stays under `build/` until archived; `make clean` removes it. Preserve
successes and failures externally before cleaning. Publish reviewed reports under
`benchmarks/results/` only after actual campaigns pass. There is currently no new
power-mode performance result to publish.

## Verification

```sh
make test-power-benchmark-harness
make SANITIZE=1 test-power-benchmark-harness
make test-benchmark-harness
make test-recovery-benchmark-harness
```

Power-profile parsing and comparison use synthetic fixtures. Process tests run
real jobs with simulated power observations, including a changed source/mode or
low charge after successful execution and deadline cleanup. These are correctness
checks, not performance measurements. Existing scaling and recovery regressions
continue to exercise real workers. Linux CI runs the focused power suite under
normal and ASan/UBSan builds with a deadline and retained logs/process evidence;
macOS power collection itself is not exercised on Linux.

Local validation on **2026-10-14** passed all 25 baseline/scaling checks, 12 power
checks, and 15 recovery checks in normal and ASan/UBSan builds, with compiler
warnings treated as errors. The added recovery-comparison fixture also passed
the final focused normal run. Logs are retained locally as
`build/power-normal-regressions.log`, `build/power-focused-final.log`, and
`build/power-sanitize-regressions.log`; these files are not published results.

A read-only probe of this Mac successfully parsed its actual battery source,
63% charge, and Low Power Mode off. The initial-charge gate correctly rejected
that state; all probe helpers were cleaned up. Thermal telemetry was unavailable.
This establishes collector/preflight behavior, not readiness for a later campaign
or a measured performance result. A subsequent end-to-end preflight at 62%
correctly wrote a failed development-series summary before any build or job
started, with all helpers reaped. Its local evidence is in
`build/benchmarks/power-preflight-check-q_3dh55_/rejected-development-series/`.
No new timing campaign has been run.
