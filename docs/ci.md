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
6. Upload available logs, including when an earlier step failed.

Every Make invocation passes the selected compiler and sanitizer mode explicitly.
CI sets `CFLAGS='-O0 -g -Werror'`. This retains the Makefile's C11 mode and warning
flags, provides debug symbols, and makes compiler warnings fail the job.
Local Make defaults are unchanged.

Only the executable build uses `--jobs=2`. Unit and integration targets run
sequentially, and process suites use `INTEGRATION_ARGS='--port 9000'`. This includes
the two default-endpoint checks that normally skip with automatic ports. Each
matrix job has its own network namespace/runner, so both can use port 9000.
Some persistence fixtures select their own temporary ports independently.

At introduction, each configuration runs **131 C groups and 143 integration
scenarios**. The workflow calls the existing Make targets, so future additions
to those targets automatically join CI. It does not freeze the suite to those
counts or replace integration testing with a build-only check.

The tests run real coordinator/worker/CLI processes and cover protocol
fragmentation, execution, failures, retry limits, stale reports, WAL I/O errors,
coordinator crashes/replay, and inspection commands. Each fixture owns temporary
state; it does not use the repository's local `faultline.wal`.

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

The workflow uploads `environment.log`, `build.log`, `unit.log`, and
`integration.log` when those steps produced files. Artifacts are named
`linux-gcc-logs` and `linux-clang-sanitizers-logs` and retained for seven days.
The upload step uses `always()` to preserve available evidence after failures;
cancellation or loss of the runner can still prevent artifact upload.

Only these explicit log paths are uploaded. Child-process diagnostics included
by the test harness appear in the test logs; this is not an archive of every
temporary WAL or every process's private output.

The token has `contents: read`, checkout does not persist credentials, and both
official actions are pinned to full commit IDs with release comments.
The pull-request event is `pull_request`. No secrets, deployments, repository
writes, or privileged `pull_request_target` execution are needed.

See [GitHub's workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)
for triggers, permissions, job strategy, shell behavior, and concurrency.

## Reproduce the checks on Linux

Install GCC 13, Clang/LLVM 18 with sanitizer runtimes, Make, libc development
headers, and Python 3. Use a free loopback port 9000 and clean build directories
when changing compilers or flags.

```sh
make clean
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' all
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-unit
make CC=gcc-13 SANITIZE=0 CFLAGS='-O0 -g -Werror' test-integration INTEGRATION_ARGS='--port 9000'

make clean
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' all
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-unit
make CC=clang-18 SANITIZE=1 CFLAGS='-O0 -g -Werror' test-integration INTEGRATION_ARGS='--port 9000'
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

This adds Linux CI to the Testing & chaos phase. The worker-crash harness
and later scaling/recovery benchmarks have separate targets and review steps. The
[chaos-test contract](chaos.md) now defines the harness defaults and acceptance
rules; it does not add a chaos job to this workflow yet.
The [batch baseline harness](batch-testing.md) is now available through explicit
`test-batch` and `test-batch-harness` targets. They are not invoked by this workflow;
CI still runs the existing unit and integration suites. The
[seeded crash harness](chaos-testing.md) is also available through `test-chaos`
and `test-chaos-harness`, with seed/action evidence and replacements; neither
new target is invoked by this workflow yet.
