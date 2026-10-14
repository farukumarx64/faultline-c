# Post-MVP work

Recorded on 2026-10-02; updated on 2026-10-14. Installation support is included
in [v0.1.1](releases/v0.1.1.md). Request deduplication is implemented after that
release; the remaining enhancements below are proposals.
These enhancements are outside the original MVP requirements. The
[scaling](benchmark-scaling.md) and AC-only
[recovery](benchmark-recovery.md#results-2026-10-02) campaigns and reports are
complete. The README, architecture diagram, and [recovery demo](recovery-demo.md)
are complete too. [Faultline v0.1.0](releases/v0.1.0.md) has been released;
[release verification](release-checklist.md) records the checks.
This list sets no implementation date for the remaining proposals. The completed
features' guides define their current behavior; benchmark contracts are unchanged.

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

## Remaining proposals

| Enhancement | Reason to revisit | Existing guarantee |
| --- | --- | --- |
| Additional power-mode benchmark profiles | When there is a specific question about battery operation or Low Power Mode | Official timing campaigns use AC power with Low Power Mode off |

## Additional power-mode benchmark profiles

AC-only measurements are sufficient for the current MVP. Battery operation and
Low Power Mode are optional experiments, using configuration in the shared
benchmark runner rather than separate execution implementations. Neither a
battery profile nor a Low Power Mode profile exists today.

Candidate configurations:

| Configuration | Status | Intended comparison |
| --- | --- | --- |
| AC, Low Power Mode off | Existing reference contract | Current scaling and recovery measurements |
| Battery, Low Power Mode off | Proposed | Compare against AC with the power mode held constant |
| Battery, Low Power Mode on | Proposed | Compare against battery with Low Power Mode off |

Choose the question and workload before implementing a profile: CPU throughput,
failure recovery, or both in separate campaigns. Within each comparison, keep
the workload, job/worker counts, retry policy, fault schedule, build, durability,
timing boundaries, repetitions, and cooldowns identical. Change only the power
source or the Low Power Mode setting being studied.

Before running these experiments:

- Define and version each profile, its supported machines, and its acceptance
  rules. Preserve the existing AC-only profile and its results.
- Define the battery starting-charge range, minimum remaining charge, and
  recharge/cooldown procedure. Record charge before and after samples.
- Read the effective settings for the selected power source; checking AC
  settings alone cannot validate a battery run. Record power source, Low Power
  Mode, machine/build identity, load, and available thermal telemetry.
- Specify when conditions are checked and disclose the observation limits.
  Unknown required settings or observed power/mode changes invalidate the
  controlled sample. Mixed-power runs need a separately defined exploratory
  profile and must not enter AC-only or battery-only aggregates.
- Retain all per-ID result, retry, fault-coverage, deadline, and process-cleanup
  checks. Test the new condition checks without weakening existing validation.

Publish each profile's raw samples, warmups, medians, variation, and limitations
separately in the benchmark documentation. Preserve failed attempts and their
logs/WALs, and link comparisons to their source samples. Use fresh evidence
directories; never combine partial campaigns to manufacture a complete result.
Archived raw evidence should live outside the repository before `make clean`
removes the originals under `build/`.

These proposals do not restrict Faultline itself or ordinary correctness tests
to AC power. They define possible future conditions for performance measurements.
