# Testing and chaos phase review

Review date: 2026-09-30. Runtime and harness revision:
`4bdd1a3a7a60538a250fe62a6bc48b0486ef6725`.
All six seed-run manifests recorded that revision with a clean working tree.
This review adds documentation; runtime, protocol, WAL, and harness behavior
are unchanged.

**The local chaos review passes:** all six full seed experiments and both
harness regression suites passed in normal and sanitizer builds. The local
worker-crash acceptance criterion is met within the boundaries below. The later
[Linux CI integration](ci.md#chaos-ci-integration-validation) is locally validated;
GitHub-hosted execution and controlled benchmarks remain separate follow-ups.

The matrix below is the macOS review. The subsequent
[Linux harness regression check](chaos-testing.md#linux-harness-regression-verification)
passed both suites with GCC and Clang sanitizers on Ubuntu ARM64; it is separate
from the full seed matrix and GitHub-hosted validation.
The subsequent [full Linux seed-42 experiment](chaos-testing.md#linux-fixed-seed-experiment-verification)
also passed in both builds. The three-seed matrix below remains macOS evidence.

The phase's local acceptance question is: after deliberately crashing and
replacing workers, does every acknowledged job remain accounted for, and does
every job still eligible after faults stop finish correctly? The
[contract](chaos.md) defines this bounded experiment, and the
[harness guide](chaos-testing.md) describes its implementation and artifacts.

## Experiment and toolchain

The seed set was chosen in the contract before this review: **42, 7, 2026**.
Each seed runs once with normal binaries and once with AddressSanitizer/UBSan.
Each experiment uses a fresh WAL, output directory, loopback port, coordinator,
and worker pool. There is no automatic retry of a failed experiment.

| Setting | Review configuration |
| --- | --- |
| Workers / submitted jobs | 5 / 100 per experiment |
| Task | `sleep --args 3000`; expected result is exactly `slept_ms=3000` |
| Retry allowance | 3; at most 4 assigned attempts per job |
| Injected failure | SIGKILL one busy owned worker, account for its loss, then register a replacement |
| Fault window / spacing | 30,000 ms after admission / seeded 2,000–10,000 ms gaps |
| Heartbeat interval / timeout | 2,000 / 6,000 ms |
| Overall deadline / cleanup reserve | 180,000 / 10,000 ms |
| Host | macOS 26.6.2, Darwin 25.6.0, arm64 |
| Compiler / Make / Python | Apple Clang 21.0.0 / GNU Make 3.81 / Python 3.9.6 |
| Build flags | `-O0 -g -Werror`, plus the Makefile's C11 and warning flags |
| Sanitizer build flags | `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all` |
| Sanitizer environment | `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`; `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` |

Both builds were forced with `make -B ... all`, so this review did not depend
on potentially stale executable timestamps. The full compilation output and
toolchain versions are retained. The normal and sanitizer lanes overlapped;
seeds ran sequentially within each lane. Run durations therefore describe
these correctness experiments and must not be treated as scaling or sanitizer
overhead benchmarks. `caffeinate -i` inhibited idle sleep on this Mac; it is
an external validation aid, not a portable harness dependency.

## Seed matrix evidence

All six runs returned **CHAOS_PASS on their first invocation**. Each satisfied
`100 submitted = 100 completed + 0 terminally_failed`, with exact individual
IDs and results. Every injected crash interrupted an actual coordinator-owned
attempt, and that job completed on a later attempt.

| Build | Seed | Crashes / interrupted attempts / replacements | Attempts / retries | Eligible at drain, all DONE | Children reaped | Elapsed seconds |
| --- | --- | --- | --- | --- | --- | --- |
| Normal | 42 | 5 / 5 / 5 | 105 / 5 | 51 | 562 | 72.477 |
| Normal | 7 | 5 / 5 / 5 | 105 / 5 | 50 | 554 | 69.227 |
| Normal | 2026 | 4 / 4 / 4 | 104 / 4 | 52 | 558 | 70.068 |
| ASan/UBSan | 42 | 5 / 5 / 5 | 105 / 5 | 46 | 498 | 78.725 |
| ASan/UBSan | 7 | 4 / 4 / 4 | 104 / 4 | 45 | 488 | 74.687 |
| ASan/UBSan | 2026 | 4 / 4 / 4 | 104 / 4 | 47 | 482 | 79.677 |

Across six separate coordinator histories, this is **600 submitted, 600 DONE,
zero FAILED, 627 attempts, and 27 retries**. All 291 jobs eligible at drain
start completed. All 3,142 direct children were reaped and their owned groups
retired; counts include CLI and metadata helper processes. Each run ended with
five ALIVE, idle workers and successful cleanup. No build warnings or unexpected
runtime/sanitizer diagnostics occurred in the full experiments.

The saved plans match for each seed across builds, but actual execution differs.
For seed 42, the interrupted job IDs were `18, 26, 37, 44, 57` in the normal
run and `23, 31, 42, 48, 59` in the sanitizer run. Seed 7 produced five crashes
in normal mode and four under sanitizers: gap timing starts after each completed
replacement cycle, so different cycle durations change how many candidates fit
inside 30 seconds. Both satisfy the same cutoff and coverage rules. Neither
build was given extra time or additional faults to equalize the result.

## What was checked independently

The review's saved-artifact audit reads the submission ledger, drain snapshot,
full statuses, job and worker listings, statistics, fault actions, event log,
and final process ledger. It checks each run rather than accepting its PASS
string or adding together summary counters.

1. All 100 submission IDs are distinct and acknowledged. Final listing/status
   IDs match them exactly. Completed and failed ID sets are disjoint and cover
   the original ledger, so `submitted = completed + terminally_failed` follows
   from identities as well as counts.
2. Every DONE status contains the exact result and `failure=NONE`. Attempts and
   retries agree with each other and stay within the original input's allowance.
   A FAILED job would require exhausted retries, WORKER_LOST, and no result.
3. Every retry and exhausted outcome has the corresponding actual lost job and
   attempt in the crash evidence. Final attempt/retry sums agree with `stats`.
4. All IDs saved as eligible at drain start are DONE. No new fault signal appears
   after drain starts. Every crash/replacement cycle settled first; all SIGKILLs
   occurred within the requested fault window.
5. Replacement registrations have fresh worker IDs. Five final workers are
   ALIVE and idle, and no job remains queued, assigned, or running.
6. Every child has a recorded exit, was reaped, and has a retired process group.
   Only deliberately killed worker children have SIGKILL exits. Cleanup succeeded,
   and full experiment logs contain no ERROR or sanitizer diagnostics.
7. The saved candidate plans match between the normal and sanitizer run for
   each seed. Source revision, configuration, build identities, and runtime
   options match the intended experiment.

The audit also records SHA-256 digests for each run's manifest, summary,
accounting report, event log, and WAL in `review-evidence.json`. These help detect
later artifact changes; they are not an independent guarantee of log completeness
or a durable external audit service.

## Harness regression evidence

| Check | Normal | ASan/UBSan |
| --- | --- | --- |
| Forced executable build with warnings as errors | Passed | Passed |
| `test-batch-harness` | 20 passed, 37.344 s | 20 passed, 44.242 s |
| `test-chaos-harness` | 32 passed, 64.559 s | 32 passed, 72.420 s |
| Full planned seed runs | 3 of 3 CHAOS_PASS | 3 of 3 CHAOS_PASS |
| Independent saved-artifact audit | All 3 runs passed | All 3 runs passed |

All 104 regression-test executions passed, with zero skips. The zero-retry
live case produced `6 submitted = 5 DONE + 1 FAILED` in each build, with the
failure attributed to the deliberately interrupted attempt. Its artifacts are
`build/chaos/regression-y4kpv059/run-1/` (normal) and
`build/chaos/regression-6otcm432/run-1/` (sanitizers). Markdown file links and
anchors, code-fence balance, reproduction shell syntax, and `git diff --check`
also passed.

The 20 baseline and 32 chaos regression tests exercise the verifier as well as
successful work. Cases include missing/duplicate ACKs, wrong same-length
results, equal totals with substituted IDs, unknown statuses, gaps/duplicates
in retry histories, insufficient recovery coverage, replacement failure,
deadline expiry, and SIGINT/SIGTERM cleanup. A zero-retry live case requires an
attributed terminal failure rather than incorrectly demanding every job succeed.

Intentional negative cases must return their specified failure and reap owned processes;
the regression test passes because the harness rejects that bad experiment.
This is different from discarding a failing seed. The positive seed matrix
does not deliberately exhaust a nonzero retry allowance; that policy also has
the existing [focused recovery tests](recovery.md#run-the-checks).

The existing 131 C groups and 143 integration scenarios were not rerun for this
documentation-only review. Their earlier validation is recorded in the
[observability review](observability-review.md#verification-record) and
[Linux CI guide](ci.md#validation-and-next-step). Those records remain separate
from the freshly executed harness checks here.

## Reproduce and retain the evidence

Run the two modes sequentially for a simple local reproduction:

```sh
(
    set -eu
    make -B CFLAGS='-O0 -g -Werror' all
    make CFLAGS='-O0 -g -Werror' test-batch-harness test-chaos-harness
    for seed in 42 7 2026; do
        make CFLAGS='-O0 -g -Werror' test-chaos CHAOS_ARGS="--seed $seed"
    done

    export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    make -B SANITIZE=1 CFLAGS='-O0 -g -Werror' all
    make SANITIZE=1 CFLAGS='-O0 -g -Werror' test-batch-harness test-chaos-harness
    for seed in 42 7 2026; do
        make SANITIZE=1 CFLAGS='-O0 -g -Werror' test-chaos CHAOS_ARGS="--seed $seed"
    done
)
```

Stop and inspect the first failure; do not pick a different seed or repeat until
a run happens to pass. Each invocation prints its fresh artifact directory.
To use named directories, add `--output-dir <new-directory>` to `CHAOS_ARGS`.
Existing output directories are deliberately refused. Preserve failures too.

This review's raw artifacts are under
`build/chaos/review-20260930-InHv0c/`, with `normal-seed-42`, `normal-seed-7`,
`normal-seed-2026`, and the corresponding `sanitize-seed-*` directories.
`normal-validation.log` and `sanitize-validation.log` capture forced builds,
seed invocations, and regression output. The review-specific `audit.py` and
`review-evidence.json` sit alongside them. `regression-artifacts.json` indexes
52 run summaries produced by this review's process fixtures, including expected
failures. Unit-only checks and preflight refusals do not create run summaries.
Harness regression directories remain under `build/chaos/regression-*`.

These raw files are local and ignored by Git. `make clean` removes them, and a
fresh clone will not contain them. Archive the whole review directory and any
referenced regression directories before cleaning if the original evidence must
be retained. This Markdown record preserves the reviewed configuration,
results, and boundaries; it does not publish all runtime logs or WALs.

## Guarantees and limits

| Evidence supports | Boundary |
| --- | --- |
| No acknowledged ID vanished in these runs | Six finite runs do not prove all schedules, seeds, or long-running churn are correct. |
| Eligible work finished after this bounded fault window | Success requires a healthy pool, remaining retry allowance, and enough time. Retry exhaustion is a legitimate terminal outcome. |
| Exact results and attributed attempts for this profile | The workload is homogeneous sleep jobs, not mixed tasks, CPU-heavy work, or external side effects. |
| Repeatable random candidate plans | A seed does not replay OS scheduling, exact job/worker identities, completion races, or even necessarily the number of faults that fit in the window. |
| ASan/UBSan reported no exercised-path errors | This does not prove memory safety. Leak detection was disabled in this review; ThreadSanitizer was not run. SIGKILL prevents exit-time checks in killed workers. |
| Controlled process cleanup completed | Harness SIGKILL, host loss, an unresponsive OS/filesystem, or descendants escaping owned process groups are outside its cleanup guarantee. |
| Local worker-crash recovery with the coordinator alive | This matrix does not inject coordinator crashes, silent open connections, stale reports, packet loss/partitions, storage errors, or physical power loss. Those with existing focused suites have separate evidence. |
| One host and fresh loopback/WAL runs | No multi-machine, production-network, Windows, Linux chaos, or GitHub-hosted chaos result is established here. |
| One retained outcome per accepted job/attempt lease | Bounded at-least-once retries can execute work again; there is no exactly-once external-effect guarantee. |

Old-attempt and wrong-worker reports are still governed by the
[lease/recovery contract](recovery.md). A lost submission ACK remains ambiguous;
[request deduplication](request-deduplication.md) is deferred, and the harness
fails uncertain admission rather than resubmitting. Coordinator restart and
storage guarantees remain those of the [persistence review](persistence-review.md).
The coordinator retains at most 256 jobs, and the harness accepts 1–16 workers;
this is not an unbounded load/soak test or a capacity benchmark.

## Phase disposition and handoff

The planned multi-seed review milestone is complete. There were no unexpected
failed seed runs, no sanitizer findings, and no runtime/harness fixes needed.
The tested local profile demonstrates retained job identities, bounded retries,
correct completion after worker replacement, and reliable verifier/cleanup
behavior across both successful and deliberately invalid experiments.

Following this review, the [Linux workflow](ci.md) adds both harness regression
suites and a bounded seed-42 experiment alongside the existing C and process
tests. Available logs and chaos evidence are configured for upload after success
or failure. Its new shell steps passed local Linux validation in both builds;
GitHub-hosted execution and actual uploads remain to be verified after pushing.

The next separate MVP experiment phase is **benchmarks**: compare a fixed
workload with 1, 2, 4, and 8 workers, then measure controlled failure-recovery
overhead. Define workload, timing boundaries, repetitions, and reported metrics
before collecting those numbers. The chaos elapsed times above are not those
benchmark results.
