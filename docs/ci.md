# Linux continuous integration

[`.github/workflows/linux-ci.yml`](../.github/workflows/linux-ci.yml) defines
the GitHub Actions workflow named **Linux CI**. It runs on pushes and pull
requests, and supports manual runs with `workflow_dispatch`. There are no
branch or path filters.

The workflow takes effect on GitHub when the file is committed and pushed.
Manual dispatch requires it on the repository's default branch. Adding the
workflow does not itself configure branch protection or require checks before
merging.

## Two independent jobs

| Check | Compiler | Build mode |
| --- | --- | --- |
| GCC 13 | `gcc-13` | Normal debug build, `SANITIZE=0` |
| Clang 18 + ASan/UBSan | `clang-18` | AddressSanitizer and UndefinedBehaviorSanitizer, `SANITIZE=1` |

Both use the GitHub-hosted `ubuntu-24.04` x86-64 runner. The named Ubuntu release
avoids following an automatic `ubuntu-latest` release change. Its image, package
patches, and toolchain patch versions can still change; every job records the
actual OS, compiler, Make, Python, and sanitizer symbolizer versions.

The required tools are already included in the
[Ubuntu 24.04 runner image](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md).
No external Python packages or project dependencies are installed. GCC adds a
compiler check beyond the Apple Clang toolchain used for local development;
the Clang job exercises Linux with runtime instrumentation.

Each job gets a fresh checkout and its own runner. `fail-fast: false` allows the
other configuration to finish if one fails. Each job has a 15-minute deadline.
New runs for the same workflow/ref cancel obsolete runs; different pull requests
remain independent.

## Build and test sequence

1. Check out the triggering revision.
2. Record environment/toolchain versions.
3. Build coordinator, worker, and CLI executables.
4. Run all C unit groups.
5. Run all process integration suites.
6. Run the 7 CI deadline/failure-propagation checks.
7. Run the 21 batch-harness regression checks.
8. Run the 35 chaos-harness regression checks.
9. Run the full seed-42 chaos experiment.
10. Upload available logs and chaos evidence, including after test failures.

Every Make invocation passes the selected compiler and sanitizer mode explicitly.
CI sets `CFLAGS='-O0 -g -Werror'`. This retains the Makefile's C11 mode and warning
flags, provides debug symbols, and makes compiler warnings fail the job.
Local Make defaults are unchanged.

Only the executable build uses `--jobs=2`. All test targets run sequentially.
Core process integration suites use `INTEGRATION_ARGS='--port 9000'`. This includes
the two default-endpoint checks that normally skip with automatic ports. Each
matrix job has its own network namespace/runner, so both can use port 9000.
Some persistence fixtures and the chaos harnesses select temporary ports
independently.

At introduction, each configuration runs **131 C groups and 143 integration
scenarios**. The workflow calls the existing Make targets, so future additions
to those targets automatically join CI. It does not freeze the suite to those
counts or replace integration testing with a build-only check.

The tests run real coordinator/worker/CLI processes and cover protocol
fragmentation, execution, failures, retry limits, stale reports, WAL I/O errors,
coordinator crashes/replay, and inspection commands. Each fixture owns temporary
state; it does not use the repository's local `faultline.wal`.

## Bounded chaos checks

Each compiler job explicitly invokes `test-batch-harness`, `test-chaos-harness`,
and `test-chaos`. These targets remain separate from local `make test`. The full
no-fault `test-batch` experiment and the broader three-seed review remain manual
checks; CI runs the baseline harness's regression suite and one full chaos seed.

The experiment fixes seed **42**, **5 workers**, **100 sleep jobs** of **3000 ms**,
**3 retries** per job, a **30000 ms** fault window, and a **180000 ms** overall
harness deadline. It saves evidence in `build/chaos/ci-seed-42`. Passing requires
the harness's existing per-ID accounting, exact results, retry limits, actual
worker-loss/recovery coverage, and successful process cleanup. A fixed seed
repeats the candidate plan; scheduling can still change which jobs are hit.

Each harness/experiment step has a four-minute Actions deadline. Each calls the
shared [`tests/ci/run_with_deadline.sh`](../tests/ci/run_with_deadline.sh) runner.
GNU `timeout` sends SIGTERM after 210 seconds and SIGKILL after another 15 seconds
if needed. The full experiment normally enforces its own 180-second deadline
first, including its 10-second cleanup reserve. The job's 15-minute deadline
remains the outer limit. A timeout fails the step; it is not treated as a pass or
retried. Abrupt runner loss can prevent graceful cleanup and uploads; the runner
must dispose of the process environment in that case.

The runner requires a positive integer timeout; zero cannot silently disable it.
It uses `set -euo pipefail`, so saving output with `tee` does not hide a failed
command, and failure to write the log also fails the step. Ordinary errors retain
their exit code; GNU timeout returns 124 on expiry, or 137 when forced termination
is required. A command that handles SIGTERM by exiting zero still times out.
The separate `test-ci-deadlines` target verifies this runner on Linux, under a
90-second command deadline and a two-minute Actions step deadline.

## Conditions that fail CI

The live harness is the acceptance verifier. An experiment rejection exits 1;
Make propagates it as a failed recipe, and the shared runner makes the Actions
step fail. The harness regression suites deliberately exercise invalid runs and
pass only when those runs are rejected and their owned processes are reaped.
There is no acceptable-failure override on the full seed-42 experiment.

| Condition | Required behavior and regression evidence |
| --- | --- |
| Missing job | Compare every acknowledged ID with listings and individual statuses. Substituting ID 999 for a submitted ID fails even when totals match. |
| Incorrect result | Compare the full expected `slept_ms=N` text. A live chaos fixture changes `slept_ms=2000` to the same-length `slept_ms=2001`; the process must exit 1. |
| Invalid retries | Enforce allowance, `attempt = retries + 1` for terminal jobs, and matching loss histories. A live post-fault fixture reports `4/3` retries and must fail. |
| Insufficient recovery coverage | Require actual interruption and a qualifying recovery (or exhaustion for zero retries). Jobs that all complete before any useful crash still produce `INSUFFICIENT_COVERAGE` and exit 1. |
| Deadline exceeded | Stop unfinished work, clean up, and fail. Never count a running job as terminally failed to balance totals. The outer runner independently rejects hangs and ignores a clean exit after timeout. |
| Cleanup failure | Require reaped children and retired process groups. A new live fixture completes and verifies every job, then requires SIGKILL to stop an owned helper; the final verdict must still be FAIL. |

A valid `accounting.json` is only one acceptance condition. Recovery coverage,
late diagnostics, overall time, and cleanup must also pass before exit 0.
Intentional fault-phase worker SIGKILLs are permitted; forced cleanup SIGKILLs are
failures. Terminal WORKER_LOST outcomes remain permitted only under the documented
retry-exhaustion policy; eligible drain work must finish DONE. This does not add
a blanket prohibition on legitimate FAILED jobs.

## Sanitizer behavior

`SANITIZE=1` selects the existing Makefile instrumentation for C programs and
tests: `-fsanitize=address,undefined`, frame pointers, and nonrecovering undefined
behavior checks.

The job also sets:

- `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`: detect memory misuse and check for
  leaks on supported normal process exits; stop on detected errors.
- `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`: fail on detected undefined
  behavior and request a stack trace.
- `ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18`: translate instrumented
  addresses into readable function/file locations.

A process deliberately terminated with SIGKILL cannot perform exit-time leak
checks. These tests check executed paths; passing does not prove the absence of
all memory errors or establish race-freedom. ThreadSanitizer is not part of this
configuration.

References: [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
and [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html).

## Failure visibility

Build and test output is shown live and saved through `tee`. An explicit Bash
shell enables `pipefail` in GitHub Actions, so a failed `make` remains a failed
step even when `tee` successfully saves its output. There is no
`continue-on-error` or automatic retry to hide test failures.

The workflow uploads the files available from these two evidence sets, retained
for seven days:

| Artifact suffix | Contents |
| --- | --- |
| `-logs` | `environment.log`, `build.log`, `unit.log`, `integration.log`, `deadlines.log`, `batch-harness.log`, `chaos-harness.log`, and `chaos-seed-42.log` under the runner's temporary `faultline-ci` directory. |
| `-chaos-evidence` | All `.json`, `.json.tmp`, `.jsonl`, `.log`, and `.wal` files under `build/chaos`, including regression fixtures and the full seed-42 run. |

Prefixes are `linux-gcc` and `linux-clang-sanitizers`, giving four artifacts when
both jobs reach the new checks. The chaos evidence includes configuration and
seed, submissions, fault actions, events, per-ID accounting, final snapshots,
summaries, process logs, and WALs when produced. Directory hierarchy distinguishes
fixtures; fixture executables and unrelated files are excluded. This does not
archive the core integration suites' temporary directories.

Each completed run retains `summary.json` (verdict and failure details),
`manifest.json` (seed, configuration, and the chaos candidate plan),
`events.jsonl` (event trace), numbered child stdout/stderr logs, and the
coordinator WAL once created. The regression harness's own output is named
`harness-N.stdout.log` / `harness-N.stderr.log`, so errors before a final summary
are included by the same upload rules. Empty stderr logs are preserved too.

JSON updates write to a temporary file before replacing the published file.
An interrupted write or rename may leave `*.json.tmp`; CI retains that file as
diagnostic evidence. It may be incomplete JSON and is not a final verdict or a
replacement for the published summary. Preserving evidence does not change a
failed experiment's exit status.

Both uploads use `always()` to attempt preservation after failures. A step skipped
after an earlier failure produces no evidence; missing files trigger a warning,
not fabricated results. Cancellation or loss of the runner can still prevent
upload. The configured patterns were checked locally; actual artifact transfer
requires GitHub-hosted execution.

In the GitHub Actions run, download `linux-gcc-chaos-evidence` or
`linux-clang-sanitizers-chaos-evidence` together with the corresponding `-logs`
artifact. The full experiment is under `ci-seed-42`; regression output remains
grouped under its `regression-*` directory. Read the summary first, then use the
manifest seed and event trace to investigate the same run. Local files stay under
`build/chaos` until explicitly removed, including by `make clean`.

A preflight rejection can occur before a run directory or WAL exists. SIGKILL,
runner loss, or storage failure can prevent a final summary from being written.
The upload retains the files actually produced, including partial writes and
outer logs; it cannot manufacture missing evidence or run after runner loss.

The token has `contents: read`, checkout does not persist credentials, and both
official actions are pinned to full commit IDs with release comments.
The pull-request event is `pull_request`. No secrets, deployments, repository
writes, or privileged `pull_request_target` execution are needed.

See [GitHub's workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)
for triggers, permissions, job strategy, shell behavior, and concurrency.

## Reproduce the checks on Linux

Install GCC 13, Clang/LLVM 18 with sanitizer runtimes, Make, libc development
headers, Bash, GNU coreutils (`timeout` and `tee`), and Python 3. Use a free loopback port 9000 and clean build directories
when changing compilers or flags.

```sh
make test-ci-deadlines
make clean
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' all
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-unit
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-integration INTEGRATION_ARGS='--port 9000'
timeout --signal=TERM --kill-after=15s 210s make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-batch-harness
timeout --signal=TERM --kill-after=15s 210s make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-chaos-harness
timeout --signal=TERM --kill-after=15s 210s make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-chaos CHAOS_ARGS='--seed 42 --workers 5 --jobs 100 --sleep-ms 3000 --max-retries 3 --fault-duration-ms 30000 --deadline-ms 180000 --output-dir build/chaos/ci-seed-42'

# Copy any evidence you want to retain outside build/ before cleaning.
make clean
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' all
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-unit
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-integration INTEGRATION_ARGS='--port 9000'
timeout --signal=TERM --kill-after=15s 210s make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-batch-harness
timeout --signal=TERM --kill-after=15s 210s make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-chaos-harness
timeout --signal=TERM --kill-after=15s 210s make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-chaos CHAOS_ARGS='--seed 42 --workers 5 --jobs 100 --sleep-ms 3000 --max-retries 3 --fault-duration-ms 30000 --deadline-ms 180000 --output-dir build/chaos/ci-seed-42'
```

## Validation and next step

Validation date: 2026-09-29. The workflow's actual `run` blocks were extracted
and executed as an unprivileged user in separate Ubuntu 24.04.5 ARM64 containers,
using the same compiler selections, flags, sanitizer options, and port 9000.
Environment: GCC 13.3.0, Clang/LLVM 18.1.3, GNU Make 4.3, Python 3.12.3,
and Linux 6.12.76-linuxkit.

| Check | Result |
| --- | --- |
| GCC 13 normal Linux build | All 131 C groups and 143 integration scenarios passed; zero skips or compiler warnings. |
| Clang 18 ASan/UBSan Linux build | The same 131 C groups and 143 scenarios passed; zero skips, compiler warnings, or sanitizer diagnostics, with leak detection enabled. |
| Actionlint 1.7.12 | Workflow syntax, expressions, and configuration passed. External ShellCheck was not installed; shell syntax was checked separately. |
| Shell syntax and failure propagation | All `run` blocks passed `bash -n`; a controlled failing `make` kept its nonzero exit status through `tee` and saved its diagnostic. |
| macOS compatibility spot check | `make all test-logs` passed after the logging-call fix. |
| Documentation/diff checks | Local Markdown links resolve, fences are balanced, and `git diff --check` passes. |

GCC's first build identified three zero-length printf-style format strings in
coordinator logging calls. They now use the logger's existing `NULL` format
option for events with no extra fields. Output and runtime behavior are unchanged;
both final Linux configurations passed with warnings treated as errors.

Local ARM64 containers are separate from GitHub's hosted x86-64 runners. No
GitHub-hosted run is claimed by this validation record: the Actions checks on
the committed and pushed revision provide that evidence. Action checkout,
artifact upload, triggers, and cancellation were configured and statically
validated, but were not executed by GitHub during this local validation.

The [local chaos phase review](chaos-review.md),
[Linux harness regressions](chaos-testing.md#linux-harness-regression-verification),
and [full Linux seed-42 experiment](chaos-testing.md#linux-fixed-seed-experiment-verification)
provided the evidence for the subsequent CI integration below. Controlled scaling
and recovery benchmarks remain a separate MVP experiment phase.

## Chaos CI integration validation

Verified on 2026-09-30 using base revision
`403510d20abe270615aeb6f9aa4adc0091f6da9b` plus the workflow changes described
above. The runtime and harness code are unchanged. The workflow SHA-256 was
`276ed1f630323b9779aa8f654494d851451daffe9ea82fce06a569a4e4f14482`.

The actual environment, build, and three new test `run` blocks were extracted
from YAML and executed with `bash --noprofile --norc -e -o pipefail` in separate,
fresh Ubuntu 24.04.5 ARM64 containers. They ran as UID/GID 1000 with external
networking disabled and loopback available, using GCC 13.3.0 and Clang/LLVM
18.1.3, GNU Make 4.3, Python 3.12.3, and Linux 6.12.76-linuxkit. Compiler flags
and sanitizer options matched the workflow, including leak detection. Host idle
sleep was inhibited during execution.

| New workflow check | GCC 13 | Clang 18 + ASan/UBSan |
| --- | --- | --- |
| Batch harness | 20/20 passed, 28.269 s | 20/20 passed, 28.256 s |
| Chaos harness | 32/32 passed, 58.319 s | 32/32 passed, 57.995 s |
| Full seed 42 | `CHAOS_PASS`, 68.921 s | `CHAOS_PASS`, 68.959 s |
| Full-run job accounting | 100 submitted = 100 DONE + 0 FAILED; 105 attempts, 5 retries | Same |
| Fault and recovery coverage | 5 crashes, 5 replacements, 5 recovered jobs | Same |

Times measure each complete shell step and are validation observations, not
benchmarks. All 104 regression-check executions passed without skips. Each full
experiment completed the 52 jobs still eligible at drain start. An independent
artifact audit checked exact IDs, results, retry attribution and limits, agreement
between status/list/stats views, fault cutoff, and cleanup. Each full run reaped
all 586 recorded children and retired their process groups. All 26 regression
run summaries per build also recorded reaped children and retired groups. Their
20 deliberate FAIL verdicts per build are expected negative-test evidence, not
failed regression checks. No unexpected runtime or sanitizer diagnostics occurred.

Workflow validation also passed:

- Actionlint 1.7.12 with ShellCheck 0.11.0, plus `bash -n` on every `run` block.
- For all three new blocks in both environments, a controlled `make` failure
  preserved exit code 23 through `tee` and saved its diagnostic.
- Repeating those controls with only the watchdog interval shortened from 210 s
  to 0.1 s preserved timeout exit code 124 through `tee`. This tests timeout
  failure propagation without waiting for the production deadline.
- The configured evidence patterns selected 2533 GCC files and 2527 sanitizer
  files, including every summary from successful and deliberately failing runs,
  full-run WALs, JSON reports, and process logs. Fixture executables were excluded.
- Documentation links/fences and `git diff --check` passed.

Local evidence is retained under Git-ignored
`build/ci-chaos-20260930-dlqTbm/`: the source archive and provenance, extracted
shell blocks, linter output, container metadata, logs, chaos artifacts, control
results, and independent `audit.py`/`results.json`. `make clean` removes it.
The two validation containers were removed after copying and checking evidence.

The existing 131 C groups and 143 integration scenarios were not rerun for this
workflow-only integration; their earlier Linux validation is recorded above.
The new steps and upload configuration are ready for GitHub, but these local
ARM64 checks do not establish hosted x86-64 results, actual artifact transfer,
or runner cancellation behavior. Those remain to be verified on a pushed
revision. Additional seeds, sustained-load tests, and controlled benchmarks
remain outside this CI profile.

## CI acceptance gate validation

Verified on 2026-09-30 after introducing the shared deadline runner, its seven
regression checks, and the three additional live chaos rejection fixtures.
The exact current workflow shell blocks for environment, build, deadline checks,
both harness suites, and seed 42 ran in fresh unprivileged Ubuntu ARM64 containers
with GCC 13.3.0 and Clang 18.1.3 + ASan/UBSan. Flags and leak detection matched CI.

| Check | GCC | Clang + ASan/UBSan |
| --- | --- | --- |
| Deadline/logging runner | 7 passed | 7 passed |
| Batch harness | 20 passed | 20 passed |
| Chaos harness | 35 passed | 35 passed |
| Full seed-42 experiment | 100 DONE, 0 FAILED; 5 crashes and 5 recovered attempts | Same |

All 124 check executions passed without skips. The final deadline-test fixture
also passed a separate seven-check Linux rerun after strengthening its emergency
cleanup for failed assertions. Its forced-stop test exercises the actual
15-second SIGKILL grace, with a one-second test deadline; it does not simulate
timeout by returning a chosen exit code.

An independent artifact audit confirmed exact job IDs/results, retry limits and
loss histories, sufficient recovery, and cleanup. Each full experiment reaped
all 586 recorded children. All 29 regression-run summaries per build recorded
reaped children and retired process groups. The three new rejection fixtures
each returned exit 1. In the cleanup fixture, all six jobs and their recovery
were verified before the forced stop caused the final FAIL verdict.

Actionlint 1.7.12, ShellCheck 0.11.0, Python/shell syntax, local documentation
links/fences, and `git diff --check` passed. Raw evidence, extracted workflow
commands, source provenance, and `audit.py`/`results.json` are retained in
Git-ignored `build/ci-gates-20260930-9dqkuvqj/`; `make clean` removes them. Test
containers were removed after evidence was copied and verified.

These changes affect CI infrastructure, tests, and documentation. The C runtime
and production harness verification rules are unchanged; the existing core C
and process integration suites were not rerun. GitHub-hosted x86-64 execution
and actual artifact uploads remain to be verified after pushing this revision.

## Evidence preservation validation

Verified on 2026-10-01 against base revision
`afe747f` plus the retention changes. Both harness suites ran through the
workflow's deadline/logging runner in fresh unprivileged Ubuntu ARM64 containers,
with the same GCC 13 and Clang 18 + ASan/UBSan configurations used above.

| Check | GCC | Clang + ASan/UBSan |
| --- | --- | --- |
| Batch regression suite | 21 passed | 21 passed |
| Chaos regression suite | 35 passed | 35 passed |
| Completed run summaries | 6 successful, 23 deliberately rejected | Same |
| Retained outer harness logs | 86, including 14 early-error stderr logs | Same |
| Selected evidence files | 1732 | 1724 |

All 112 regression-check executions passed without skips. An independent audit
applied the workflow's upload patterns to the actual artifacts and checked every
summary, manifest seed/configuration, final trace event, child/harness log, and
WAL after admission. Empty logs were included. The publication-error fixture's
previous JSON and unpublished `.json.tmp` were both selected, while executables
and fixture scripts were excluded. Every completed fixture also confirmed reaped
children and retired process groups. Actionlint 1.7.12, Python/shell syntax,
documentation links/fences, and `git diff --check` passed.

Evidence, file inventories with SHA-256 hashes, extracted workflow commands,
source provenance, and the audit script/results are retained under Git-ignored
`build/ci-artifacts-20261001-bgupahgb/`. The validation containers were removed
after copying and checking their evidence. `make clean` removes the local files.

This change affects artifact selection and regression checks, not the C runtime
or production harness. The unchanged core suites, deadline suite, and full
100-job seed-42 experiment were not rerun; their prior evidence is recorded above.
This verifies local Linux artifact production and selection. Actual GitHub-hosted
upload/download remains to be verified after pushing the workflow.
