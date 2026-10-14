# Post-MVP work

Recorded on 2026-10-02; updated on 2026-10-14. Installation support is included
in [v0.1.1](releases/v0.1.1.md). Request deduplication is implemented after that
release. Battery and Low Power Mode benchmark support and CPU-scaling results
are complete; power-mode recovery measurements remain pending.
These enhancements are outside the original MVP requirements. The
[scaling](benchmark-scaling.md) and AC-only
[recovery](benchmark-recovery.md#results-2026-10-02) campaigns and reports are
complete. The README, architecture diagram, and [recovery demo](recovery-demo.md)
are complete too. [Faultline v0.1.0](releases/v0.1.0.md) has been released;
[release verification](release-checklist.md) records the checks.
This list sets no implementation date for the remaining proposals. The completed
features' guides define their current behavior; the original workload contracts
and published AC-only results are unchanged.

## Completed since v0.1.0

[Installation support](installation.md) adds `make install` / `make uninstall`,
personal and system prefixes, and staged installation. With the command directory
on `PATH`, the CLI, coordinator, and worker run from any directory. Foreground
startup and explicit WAL selection remain under the user's control. Services,
automatic startup, prebuilt downloads, and package-manager distribution remain
possible follow-ups; they are not included in this installation feature.

[Request deduplication](request-deduplication.md) adds an optional stable
`--request-key`: identical retries return the original ID, conflicting reuse is
rejected, and the key-to-job association is saved atomically and recovered from
the WAL. Keys are retained with jobs, including terminal jobs. Unkeyed admissions
remain available. This prevents duplicate job creation for a retained key;
execution and external effects are still subject to at-least-once retries.

## Additional power-mode benchmark profiles

[Battery and Low Power Mode benchmarks](benchmark-power.md) now add three
explicit conditions to the shared runner: AC with Low Power Mode off, battery
with it off, and battery with it on. The default remains AC. Scaling and recovery
remain separate workloads, with unchanged jobs, repetitions, build settings,
timing boundaries, correctness checks, deadlines, and cleanup.

The versioned power contract checks the selected source and effective mode,
requires battery campaigns to start at 90–100% charge and remain at or above 20%
at observed boundaries, and preserves evidence on failure. A comparison command
audits three complete campaigns before writing separate aggregates and percentage
changes. It requires matching source, build, and machine configurations, including
a fresh AC control on the same revision for each comparison.

**CPU scaling complete:** the [October 14 report](benchmark-power-results.md)
documents all three campaigns, 4,608 correctly completed jobs, medians, variation,
and a graph. Normal battery performance was close to AC; Low Power Mode took
75–99% longer than normal battery for the same batches. Observed limits and the
constant 99% normal-battery reading are retained in the report. This is a
performance comparison, not an energy-efficiency measurement.

**Still pending:** separate battery and Low Power Mode recovery comparisons.
The guide includes commands and an operator procedure. The older AC results
remain historical measurements; regression fixtures are not performance samples.

## Possible later work

Package-manager distribution, prebuilt downloads, and optional service management
remain possible installation follow-ups. Additional performance questions—such
as AC with Low Power Mode on, energy use, or mixed-power operation—need separately
defined contracts and evidence. No implementation date is set for these ideas.

Power profiles constrain controlled measurements, not Faultline itself or its
ordinary correctness tests. Archive successes and failures outside `build/`
before `make clean` removes local evidence.
