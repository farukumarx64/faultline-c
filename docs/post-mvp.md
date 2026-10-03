# Optional post-MVP work

Recorded on 2026-10-02. Status: **proposals only; not implemented**.
These enhancements are outside the current MVP requirements. The
[scaling](benchmark-scaling.md) and AC-only
[recovery](benchmark-recovery.md#results-2026-10-02) campaigns and reports are
complete. The README, architecture diagram, and [recovery demo](recovery-demo.md)
are complete too; [release verification](release-checklist.md) records the checks
and remaining launch actions. Finish the v0.1 release before expanding scope.
This list sets no implementation date and does not change the
current protocol, durability guarantees, or benchmark contract.

| Enhancement | Reason to revisit | Existing guarantee |
| --- | --- | --- |
| [Submission request deduplication](request-deduplication.md) | Before adding automatic client resubmission, or when duplicate submissions become costly | A lost ACK leaves an uncertain outcome; resubmission can create another job |
| Additional power-mode benchmark profiles | When there is a specific question about battery operation or Low Power Mode | Official timing campaigns use AC power with Low Power Mode off |

## Request deduplication

The [existing proposal](request-deduplication.md) describes stable client request
keys, returning the original job ID for an identical retry, rejecting conflicting
key reuse, and durably recovering the key-to-job association after restart.
It already includes open design decisions and acceptance checks; keep that file
as the detailed design when this work is resumed.

This enhancement would avoid duplicate job creation for a retained request key.
A job could still execute multiple attempts under the existing at-least-once
policy. Submission deduplication does not promise exactly-once external effects.

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
