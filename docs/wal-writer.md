# Reliable WAL appending and synchronization

The standalone WAL writer now creates a new log, holds an exclusive lock, writes
complete records, and synchronizes them before reporting success. It implements
the storage boundary from the [durability contract](durability.md), using the
existing [version 1 format](wal-format.md).

The coordinator now uses this module through its [transaction layer](persistence.md),
which publishes live changes only after successful append/sync. The separate
[replay module](wal-replay.md) opens existing logs, reconstructs state, repairs
incomplete tails, and resumes this writer before startup reconciliation.

## Successful append means successful sync

```text
check writer state and expected sequence
                 ↓
validate and encode the whole record into a bounded memory buffer
                 ↓
write every byte, continuing from the unwritten offset
                 ↓
fsync the WAL file; retry an interrupted sync
                 ↓
advance synced_sequence and next_sequence
                 ↓
return FAULTLINE_WAL_WRITE_OK
```

There is no public append-without-sync operation or batch flush. Every successful
append includes its required flush. Coordinator integration waits for
this success before publishing a transition or exposing its ACK/assignment/result.
The writer does not itself mutate jobs, manage retry counters, or send messages.
It validates each record's fields and sequence, but the caller must validate the
job transition against live state before appending. Replay checks
those relationships when reading an existing history.

A successful `write()` only reports bytes transferred; storage errors can appear
later. `fsync()` is the chosen synchronization boundary, including relevant file
metadata. See the [write manual](https://man7.org/linux/man-pages/man2/write.2.html)
and [fsync manual](https://man7.org/linux/man-pages/man2/fsync.2.html).

The contract covers a coordinator process crash with retained local storage and
successful filesystem synchronization. It does not claim universal OS-crash or
power-loss survival. On macOS, `fsync()` can leave data in a device cache; Apple
documents `F_FULLFSYNC` for stronger flushing. This implementation uses the agreed
`fsync()` boundary. See [Apple's documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html).

## Creating a new WAL

Initialize the handle with `FAULTLINE_WAL_WRITER_INIT`, then call
`faultline_wal_writer_create(&writer, path)`. Its directory must already exist.
Absolute paths and relative paths are supported; a filename alone uses the
current directory. Empty paths and trailing slashes are invalid arguments.

Creation performs these steps:

1. Open the parent directory and create the file relative to that descriptor.
   `O_CREAT | O_EXCL` refuses any existing final path, including a symlink,
   directory, FIFO, or existing WAL. There is no truncate or overwrite mode.
2. Open the new regular file with append semantics and mode `0600`, subject to
   the process umask. `O_NOFOLLOW` applies to the final filename; parent directory
   symlinks resolve normally. Both descriptors are close-on-exec.
3. Acquire an exclusive, nonblocking `flock()` on the WAL. Retry EINTR, but
   report contention or other failures instead of waiting indefinitely.
4. Encode and completely write the 24-byte version 1 file header.
5. Successfully `fsync()` the file, then its parent directory. Only then set the
   handle to READY with `next_sequence=1` and `synced_sequence=0`.

The directory sync matters because the file contents and the directory entry
giving it a name are separate persistence concerns. Syncing the file alone does
not establish the required parent-directory synchronization.

The lock remains held until the WAL descriptor closes, even after an append
failure. The parent directory descriptor is also kept until close. Separate
opens compete for the lock, including within one process. These are advisory
locks: every participating writer must obey them. See the
[flock manual](https://man7.org/linux/man-pages/man2/flock.2.html).

The handle has one owner. Do not copy a live handle, concurrently call it from
multiple threads, or use it in a fork child. Do not manipulate its descriptors
outside the module. A forked child must close inherited descriptors or exec with
the close-on-exec flags; otherwise an inherited descriptor can retain the lock.
The local WAL path must remain stable during use; live rename/unlink/replacement
and uncooperative writers are outside this storage policy.

Existing-file append is available through `faultline_wal_replay_open()` only
after replay establishes the valid history, final sequence, queue/counter state,
and any repairable trailing fragment, then syncs the file and parent directory.
Shared opening code now verifies a regular file and enters RECOVERING; appends
remain disabled until initialization or replay succeeds. The descriptor stays
locked throughout this handoff. There is no caller-supplied offset or sequence
to bypass validation. See the [replay guide](wal-replay.md).

## Partial writes and interruptions

Suppose a record is 100 bytes and the first `write()` accepts only 30:

```text
write bytes  0..99  → 30 written; next offset = 30
write bytes 30..99  → interrupted (EINTR); offset stays 30
write bytes 30..99  → 50 written; next offset = 80
write bytes 80..99  → 20 written; next offset = 100
fsync               → success
```

Positive short writes are progress. The next call starts at the first unwritten
byte. A `-1`/EINTR return is retried without advancing the offset. A zero-byte
return for a nonempty request becomes EIO instead of an endless loop. Other
write errors, including ENOSPC, EDQUOT, EFBIG, and EIO, fail the handle.

An interrupted `fsync()` is retried as the same pending synchronization; other
sync errors fail the handle. No new record is written during these retries.
`O_APPEND` positions each write at EOF, but a whole record may require multiple
calls. Exclusive ownership and locking prevent cooperating writers from
interleaving them. A record is not one guaranteed atomic physical write.

The encoded buffer is at most 2164 bytes. Appending allocates no heap memory.
Creation allocates a temporary pathname copy and frees it before writing the header.

## Errors and the failed state

| Outcome | Handle behavior |
| --- | --- |
| Invalid arguments, record fields, or unexpected sequence | No write/sync; a READY handle stays usable. |
| Final sequence `UINT64_MAX` synced | Keep that confirmed sequence; next sequence becomes zero. Further appends report exhaustion without I/O. |
| Fatal initialization, lock, write, or sync error | Enter FAILED; preserve the first operation and errno. |
| Append attempted on FAILED | Return IO_ERROR without additional writes or syncs. |
| Close a healthy handle | Close descriptors and enter CLOSED. Repeated close is harmless. |
| Close a failed handle | Close descriptors once, retain FAILED and the original error, and return IO_ERROR. |
| Close itself fails | Enter FAILED; retain that error if there was no earlier one. |

Read-only diagnostics are `failed_operation`, `system_error`, `synced_sequence`,
and `next_sequence`. For example, `sync_record` with EIO identifies a failed
record flush; `write_record` with ENOSPC identifies lack of space during writing.
`faultline_wal_writer_operation_name()` provides a printable operation name.
The saved error survives later cleanup calls changing the process's `errno`.

If sequence 8 fails after sequence 7 succeeded, `synced_sequence` stays 7.
Sequence 8 may be absent, partial, or complete. The writer does not report
acceptance, rewind/delete the record, retry the logical append, or permit
sequence 9. Replay inspects what survived; a failed sync is not proof
that a complete record disappeared.

After I/O failure, coordinator integration stops admission and
scheduling, closes connections without logging further job-loss transitions,
and exits unsuccessfully. A failed handle cannot be reopened with `create()`,
including after close. There is no automatic fresh-log fallback.

Always close the handle after a create or replay attempt that may have opened
descriptors, including failed attempts. Initialization can leave an empty, partial, or complete
file. The writer leaves it in place and never silently resets or removes it.
Recovery/operator handling must distinguish those cases.

Close performs no writes, syncs, truncation, or unlinking. It attempts each close
once and preserves the first error. It does not blindly retry an interrupted
close: the descriptor may already have been released and reused. Close errors
are fatal; process termination cleans up any remaining resources. See the
[Linux close manual](https://man7.org/linux/man-pages/man2/close.2.html) and
[Apple close manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/close.2.html).

## Files and verification

- [`include/wal_writer.h`](../include/wal_writer.h): handle, states, results,
  diagnostics, and create/append/close APIs.
- [`src/coordinator/wal_writer.c`](../src/coordinator/wal_writer.c): creation,
  locking, complete writes, sync ordering, and failure handling.
- [`src/coordinator/wal_writer_internal.h`](../src/coordinator/wal_writer_internal.h):
  a private syscall dependency seam for deterministic tests. Normal creation
  selects actual `flock`, `write`, `fsync`, and `close` calls.
- [`tests/test_wal_writer.c`](../tests/test_wal_writer.c): eleven test groups
  using temporary files, injected syscall outcomes, and child processes.

```sh
make test-wal-writer
make SANITIZE=1 test-wal-writer
make test-unit
make SANITIZE=1 test-unit
```

Coverage includes all seven record types, binary maximum-size payloads, 17-byte
partial writes, exact file bytes, file-before-directory sync ordering, complete
record length before sync, and unchanged confirmed sequence during I/O. Tests
exercise every split of a 36-byte record with interrupted writes and syncs, and
write the file header one byte at a time.

Failure cases include no-space, quota, file-size, and device errors; zero write
progress; header/record/directory sync failures; partial headers and records;
invalid inputs; exhaustion; and close errors. A close test immediately reuses a
released descriptor number and proves cleanup does not close its replacement.
Lock tests use separate opens and separate processes.

The process-crash check waits until a child reports a successful synced append,
then sends SIGKILL before the child closes its writer. The parent verifies the
surviving file header and allocation record and acquires the released lock.
This demonstrates retained bytes after a writer process crash. It does not
simulate power loss or reconstruct coordinator job state.

The eleven writer groups are joined by twelve [replay groups](wal-replay.md#verification),
plus seven coordinator-store groups. With seven [status protocol groups](job-status-protocol.md#verification),
the current C unit total is 114. Replay
validates history, reconstructs state,
handles only permissible incomplete tails, syncs the recovered prefix, and safely
resumes appending. [Coordinator integration](persistence.md) applies this boundary
to live job transitions and startup reconciliation.
