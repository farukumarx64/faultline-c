# Submission request deduplication

Implemented after v0.1.1. Opt in with `faultline submit --request-key HEX32`.
An identical retry returns the original job ID, including after a coordinator
restart using the same WAL. This extends admission; the existing bounded
at-least-once execution policy still applies.

## Why a request key is needed

A coordinator can save a job and then lose the TCP connection before the CLI
receives its acknowledgment. The CLI cannot tell whether the job was accepted.
Without a key, submitting again creates a new job. A key gives that logical
submission a stable identity that the client knows before its first send.

```text
Client                               Coordinator / WAL
submit key K, hash "hello"  --------> save job 1 + key K; sync
                           <---X---  ACK job 1 is lost
retry key K, hash "hello"   --------> find existing key and identical definition
                           <-------  ACK job 1; no new job or WAL record
```

## CLI usage

Build this revision first (`make`). Installed v0.1.1 binaries do not have the new
option; rebuild/reinstall if using commands from `PATH`.

Choose a fresh key once for a new logical job, and save it with the input:

```sh
# Generate once, before the first send. Keep this value for every retry.
request_key=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
printf '%s\n' "$request_key"

./build/debug/faultline submit hash --args hello --max-retries 1 \
  --request-key "$request_key"

# If the ACK was lost, repeat the SAME key, task, argument bytes, and retry limit.
./build/debug/faultline submit hash --args hello --max-retries 1 \
  --request-key "$request_key"
```

Both successful commands print the same `job_id=N`. Generate a different key for
an intentional second job, even if its input is identical. Do not regenerate the
key inside a retry loop. Retain it somewhere durable if the client itself can
restart; Faultline does not maintain a client-side request file.

Keys are exactly **32 hexadecimal digits**, representing 16 opaque bytes.
Upper/lowercase are equivalent; no `0x` prefix or UUID hyphens are accepted.
All-zero keys, duplicate flags, invalid digits, and incorrect lengths fail before
connecting. A random 128-bit key makes accidental collision extremely unlikely;
it is still the client's responsibility to select and retain a unique key.

| CLI exit | Meaning |
| --- | --- |
| `0` | Valid ACK; stdout is `job_id=N`, for either a new admission or an identical retry. |
| `3` | Key conflict: the coordinator already has this key with a different definition. No success output and no change to the original job. |
| `1` | Argument, connection, send, or protocol error. After an attempted send, failure to validate a reply means admission is unconfirmed. Keyed requests print a reminder to retry the identical submission with the same key. |

Changing `hello` to `goodbye` with the same key produces a conflict, not a new
job and not an update. A conflict response must echo the requested key; the CLI
rejects a response for a different key as a protocol error. It never retries
automatically or falls back to unkeyed submission when a peer rejects the new
message.

## Exact matching, state, capacity, and scope

The coordinator compares the task type, `max_retries`, argument length, and every
argument byte. It does not normalize task inputs: `sleep` arguments `1` and `01`
are different definitions, even if the task would interpret them identically.
`--args hello` and its equivalent `--args-hex` form do match because their bytes
are the same. Coordinator address and spelling/case of the hex key are not part
of the job definition.

The immutable key lives in the retained job record. The store searches its
bounded array of at most 256 jobs before checking new-admission capacity or
allocating an ID. Single-threaded event-loop ownership serializes concurrent
submissions. Duplicate and conflicting requests do no WAL I/O and do not change
queue position, timestamps, attempts, retry allowances, results, or job counts.
Other independent execution events can of course advance a job meanwhile.

Matching works in QUEUED, ASSIGNED, RUNNING, DONE, and FAILED. An identical retry
of a terminal job returns its ID; it does not run it again or reset a failed
job's budget. Use `faultline status ID` to read its current outcome. Existing
keys still resolve when all job slots are occupied, while a new key needs a free
slot. Keyed and unkeyed jobs share the same capacity.

Keys have **no expiry** while their jobs remain retained; current storage has no
eviction or compaction. A restart keeps both terminal jobs and their keys. Keys
are scoped to one coordinator's retained WAL history, with no per-user namespace.
They are identifiers, not authentication secrets. A different/new/deleted/rolled
back WAL cannot supply the original association. Unkeyed submissions remain
independent admissions and cannot acquire a key retroactively.

## Wire format

These are additive message types in protocol version 1; existing message bytes
are unchanged. All integer fields are unsigned big-endian; the key is raw bytes,
not its CLI hex text. Offsets exclude the existing 12-byte frame header.

`JOB_SUBMIT_KEYED` (type **21**):

| Payload offset | Bytes | Field |
| --- | --- | --- |
| 0 | 16 | Nonzero request key |
| 16 | 2 | Task type |
| 18 | 4 | Maximum retries |
| 22 | 4 | Argument length, 0–1024 |
| 26 | Argument length | Exact argument bytes |

The frame is 38–1062 bytes. New and duplicate admissions both use the existing
`JOB_SUBMIT_ACK` (type 7, 8-byte job ID). A conflict uses
`JOB_SUBMIT_CONFLICT` (type **22**), with exactly the 16-byte request key echoed
at payload offset zero (28-byte frame). There is no multiplexed correlation ID
in a success ACK; the CLI still has one outstanding request per connection.
Malformed requests, capacity failures, and worker-role violations follow the
existing connection-close policy. Conflicts are explicit replies and leave a
valid client connection usable.

Old clients' type 6 submissions continue working with a new coordinator. Old
coordinators reject unknown type 21; they cannot provide this guarantee. Upgrade
the CLI and coordinator together. Worker assignment/report formats are unchanged.
The shared C submit structure requires an all-zero key for legacy type 6 and a
nonzero key for type 21; the encoder refuses to silently discard a supplied key.

## Persistence and crash behavior

WAL version 1 adds type **8**, `JOB_CREATED_KEYED`: a 16-byte key followed by the
existing creation snapshot (84-byte prefix plus arguments). The whole payload
is 100–1124 bytes, covered by the existing CRC32. One complete record associates
the key with the job, definition, ID allocation, and queue insertion.

A first admission follows the existing prepare → append → sync → publish → ACK
ordering. There is no separate key record that could be lost while the job
survives. Later transition records keep their existing byte layouts. Replay
carries the immutable key forward from creation while validating each transition.
It rejects all-zero keys and duplicate keyed creations, even with valid CRCs.

A crash before a complete creation record may leave no job. Replay repairs only
a permissible incomplete final record, then the retry can admit the request.
A complete valid record that survives is replayed, even if sync or ACK was never
confirmed. Startup syncs recovered history before listening, so a subsequent
identical retry returns that job's ID without appending again. Interrupted
attempt reconciliation preserves the key and applies the existing retry policy.
Storage errors retain the existing fail-closed behavior; no failed write or sync
permits a premature ACK.

**Downgrade limit:** older WALs load unchanged. Once a keyed creation is appended,
older coordinators (including v0.1.1) reject that unknown record type and refuse
startup. There is no downgrade converter or record-skipping mode. Preserve the
WAL; deleting it or rolling back to an older copy discards admissions and keys.

## Verification

```sh
make test-deduplication
make SANITIZE=1 test-deduplication
# Included in the normal full suites and their existing Linux CI jobs:
make test
make test-sanitize
```

Two C groups check literal wire bytes, every partial prefix, bounded binary
arguments, unchanged outputs, key validation, and an independently generated
WAL/CRC vector. The store suite verifies that duplicate/conflicting submissions
perform no storage I/O or mutation, and injects partial writes and sync failures
for keyed creation alongside existing operations.

Twelve process scenarios cover concurrent first submissions, fragmented and
coalesced frames, exact-definition conflicts, CLI validation, empty and maximum
binary inputs, retained states, capacity, retry/FIFO preservation, restart,
invalid persisted keys, role restrictions, malformed replies, and no downgrade.
A TCP proxy deliberately drops a real ACK before restarting the coordinator and
retrying from the CLI. Instrumented real processes are killed before writing,
during partial headers/payloads, before sync, and after sync. These checks verify
process-crash behavior, not storage-device loss or power-failure survival.

### Local verification record — 2026-10-14

Environment: macOS 27.0.1 arm64, Apple Clang 21.0.0, Python 3.9.6.
Both configurations used `CFLAGS='-O0 -g -Werror'`; the instrumented configuration
also used `SANITIZE=1` (AddressSanitizer and UBSan).

| Check | Normal | ASan/UBSan |
| --- | --- | --- |
| `make test INTEGRATION_ARGS='--port 9000'` | 134 C groups + 155 process tests passed; zero skips | Same counts passed; zero skips or sanitizer diagnostics |
| Final `make test-deduplication test-observability` | 10 C groups + 18 process tests passed | Same counts passed |
| `make test-install` | 6 checks passed | 6 checks passed |

The final focused checks followed a logging correction so reused submissions
report `outcome=EXISTING` in every job state. No admission or execution behavior
changed after the full regression runs. The existing Linux workflow picks up
these tests through `test-unit` and `test-integration`; this revision has not
yet been verified on Linux. Historical release/benchmark
reports keep their original tested revisions and counts.

## Limits

This prevents duplicate **job creation** for a retained key. One job can still
execute multiple attempts under [bounded at-least-once recovery](recovery.md).
It does not promise exactly-once computation or external effects, authenticate
clients, add worker completion ACKs, or recover a lost/rolled-back WAL.
