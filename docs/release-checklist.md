# Faultline v0.1 release checklist

Review date: **2026-10-03**. Source under review:
`86cb174ac3d797f26961b6b02e1d44892fc4afdd`.

**Verification passed for the reviewed source revision.** This review does not
create a tag, publish a GitHub Release, or change the runtime guarantees.
The repository is now
[`farukumarx64/faultline-c`](https://github.com/farukumarx64/faultline-c);
the project and executables remain named Faultline and `faultline`.

## Checklist and evidence

| Check | Result and scope |
| --- | --- |
| Fresh checkout | PASS: clone the renamed public URL into a new directory; its HEAD matches the reviewed revision; no existing build files or WAL are copied. |
| Documented build | PASS: plain `make` with Apple Clang builds all three executables without warnings. |
| README quick start | PASS: default port 9000, PING/PONG, submission/status, all four task examples, jobs/workers/stats, and unknown-ID exit status 2. |
| Documented restart | PASS: all four completed results remain identical after reopening the same WAL; the next submission gets job ID 5 and completes. All six coordinator/worker processes across the smoke check exit cleanly. |
| Alternative endpoint | PASS: coordinator and worker configuration on port 9100, explicit heartbeat timings, and CLI `--coordinator` work. |
| Local normal tests | PASS: 131 C groups and 143 integration scenarios; no skips or compiler warnings. |
| Local ASan/UBSan tests | PASS: the same 131 C groups and 143 integration scenarios; no skips, compiler warnings, or sanitizer diagnostics. |
| Demo and benchmark verifier regressions | PASS in each build: 5 demo checks, 25 baseline/scaling verifier checks, and 15 controlled-recovery verifier checks; 45 total, with no skips. |
| Reproduce the recovery demo | PASS: plain `make demo-recovery` submits job 1, kills worker 1, and verifies worker 2 completes the same ID on attempt 2 with one retry and `slept_ms=6000`; process cleanup succeeds. |
| Hosted Linux CI | PASS: both GCC 13 and Clang 18 + ASan/UBSan jobs at the reviewed revision; see the exact run below. |
| Documentation links | PASS: 439 local links across 38 Markdown files, including 103 heading references; checked after the review edits. |
| Public evidence | PASS: the checked-in GIF, asciicast, and demo source match the capture report's SHA-256 hashes. Benchmark reports retain their original revisions and measurement conditions. |
| Repository hygiene | PASS for the inspected tracked tree: no build outputs, WALs, Python bytecode, `.env` files, or common private-key/token patterns found. This is a limited content scan, not proof that the full Git history contains no secrets. |
| Guarantees and limits | README and architecture agree with the implementation: loopback-only deployment, bounded retries, durable admission and results, uncertain missing ACKs, and no exactly-once effects or coordinator failover. |

After this review, the project owner selected the [MIT License](../LICENSE)
on 2026-10-03. The root license file and README now declare that choice, with
the copyright notice `Copyright (c) 2026 Faruk Umar`. The link count above records the
original review, before these two license links were added. Release notes,
a release tag, and GitHub publication remain separate launch actions.

## Reproduce the local checks

Use a new directory so existing binaries, compiler flags, or coordinator state
cannot affect the check. Requirements are Make, a C11 compiler, POSIX sockets and
threads, and Python 3.9+. Ports 9000 and 9100 must be free for the README smoke
check; do not stop another person's processes to claim those ports.

```sh
git clone https://github.com/farukumarx64/faultline-c.git
cd faultline-c
git rev-parse HEAD
make
```

Follow the [README quick start](../README.md#quick-start), including the restart
and [custom endpoint](../README.md#commands) examples. Compare the built-in
results against `slept_ms=1000`, `25`, `55`, and `a430d84680aabd0b`. Stop the
coordinator and workers before the core tests use port 9000.

```sh
make test INTEGRATION_ARGS='--port 9000'
make test-sanitize INTEGRATION_ARGS='--port 9000'
make test-demo-recovery test-benchmark-harness test-recovery-benchmark-harness
make SANITIZE=1 test-demo-recovery test-benchmark-harness test-recovery-benchmark-harness
make demo-recovery
```

Run these sequentially. Supplying port 9000 enables the two default-endpoint
checks that skip when automatic ports are used. The demo and benchmark verifier
targets are separate from `make test`; the benchmark verifiers use short fixtures,
not the AC-only timing campaigns. This release review does not replace or rerun
the published performance measurements.

The local review uses macOS 27.0.1 (26A434), ARM64, Apple Clang 21.0.0
(`clang-2100.3.34.2`), GNU Make 3.81, and Python 3.9.6. Normal builds use the
documented `-O0 -g` defaults and project warning flags. The sanitizer build adds
AddressSanitizer, UBSan, frame pointers, and nonrecovering undefined-behavior
checks through the Makefile. Linux additionally enables LeakSanitizer at normal
process exit; that is not a claim of macOS leak checking or leak checking after
deliberate SIGKILL.

## Hosted Linux evidence

[Linux CI run 37122370243](https://github.com/farukumarx64/faultline-c/actions/runs/37122370243)
was triggered by a push to `main` at the exact reviewed revision. Both jobs
completed successfully on their first attempt on Ubuntu 24.04.5 x86-64:

| Check | [GCC 13.3.0](https://github.com/farukumarx64/faultline-c/actions/runs/37122370243/job/111200864026) | [Clang 18.1.3 + ASan/UBSan](https://github.com/farukumarx64/faultline-c/actions/runs/37122370243/job/111200863943) |
| --- | --- | --- |
| Build with `-Werror` | PASS | PASS |
| C unit groups | 131 passed | 131 passed |
| Process integration scenarios | 143 passed; no skips | 143 passed; no skips |
| Deadline/failure-propagation checks | 7 passed | 7 passed |
| Batch harness regressions | 22 passed | 22 passed |
| Chaos harness regressions | 35 passed | 35 passed |
| Full seed-42 experiment | 100 acknowledged, 100 completed, 0 failed | 100 acknowledged, 100 completed, 0 failed |
| Log and chaos evidence uploads | Both succeeded | Both succeeded |

The run and job metadata and complete job logs were retrieved during this review.
All expected steps succeeded, and all four uploaded artifacts were listed as
available and unexpired. This review did not redownload and independently audit
their ZIP contents; the [earlier artifact audit](ci.md#github-hosted-linux-validation)
retains its original scope. Artifacts have a seven-day retention period.

The workflow covers pushes, pull requests, and manual dispatch. This evidence is
an actual push run, not a separate pull-request event test. Demo and benchmark
verifier targets are checked locally here and are not currently included in
Linux CI. The workflow and runtime source were unchanged by this review.

## Documentation changes and retained evidence

- Updated the clone command, expected checkout directory, CI badge, and historic
  Actions links to the renamed repository. Run/job IDs and source revisions stay
  unchanged. Updated the local Git `origin` URL too; that is local configuration.
- Clarified Python 3.9+ for tests and the demo. The optional GIF renderer still
  has its separately documented Python 3.10+ and Pillow requirements.
- Linked this checklist from the README and CI guide, and updated the post-MVP
  handoff to reflect the completed documentation and demo.
- Replaced the stale note that hosted verification of the sanitizer cleanup fix
  was pending with the now-confirmed Linux run and its precise scope.
- Checked local documentation targets, CLI flags/results, capacity/default
  constants, diagram semantics, all seven README benchmark rows against the
  recorded JSON aggregates, and recorded-demo provenance.

Raw review evidence stays in Git-ignored
`build/release-check-20261003-D2Asb6/`: fresh clone, build/test logs, README smoke
commands and process cleanup, GitHub run/job/artifact metadata, full Linux job
logs, and a `summary.json` with check counts and log hashes. The new demo's
recording, process logs, summary, and WAL are under the clone's
`build/demos/recovery-ra1n2zxp/`. Preserve the review directory outside `build/`
before running `make clean`.
Earlier benchmark and demo evidence directories were left intact.

This is verification of the identified source revision plus documentation
corrections. Commit and push the corrections, then confirm Linux CI on that
final commit before selecting a release tag. Passing tests exercise the recorded
paths and environments; they do not establish production readiness, multi-host
operation, exactly-once execution, or storage survival after disk loss.
