# Installation

Installation copies Faultline's three executables into a directory your shell
can search. Once that directory is on `PATH`, `faultline status 1` works from
any working directory. The coordinator must be running at the selected endpoint,
and job 1 must exist there. A worker must be running to execute queued work.

This feature is included starting with [v0.1.1](releases/v0.1.1.md). Use its
source archive or a checkout containing the `install` target; the original
`v0.1.0` release tag remains unchanged.

## Personal installation

You need Make, a C11 compiler, and the platform's `install` utility on macOS or
Linux. There are no additional C library dependencies. Python is needed for
tests, not for building, installing, or running the commands.

From the repository root:

```sh
make install PREFIX="$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
command -v faultline
faultline --help
faultline-coordinator --help
faultline-worker --help
```

The commands are installed as regular executable files:

```text
~/.local/bin/faultline
~/.local/bin/faultline-coordinator
~/.local/bin/faultline-worker
```

`PATH` is the list of directories the shell searches for commands. Add the
`export PATH=...` line above once to `~/.zshrc` for zsh, or `~/.bashrc` for
interactive bash, to keep it in new terminals. Bash login shells must also load
that configuration or set the same path in their login startup file.
Installation does not edit your shell configuration.

`command -v faultline` should report the path you installed. If another copy is
selected, check the order of directories in `PATH`; restart your shell or use
`rehash` (zsh) / `hash -r` (bash) to refresh cached command locations.

## Start and use the installed commands

Use separate terminals, with the installed directory on each terminal's `PATH`.
Choose an absolute WAL path so changing directories does not select a different
file. Here is a fresh setup on the default `127.0.0.1:9000` endpoint:

```sh
# Terminal 1: first launch only.
mkdir -p "$HOME/.local/state/faultline"
faultline-coordinator --wal "$HOME/.local/state/faultline/coordinator.wal" --init-wal

# Terminal 2
faultline-worker

# Terminal 3
faultline ping
faultline submit sleep --args 1000 --max-retries 1
faultline status 1
faultline jobs
faultline workers
faultline stats
```

Use the job ID printed by `submit`; 1 is the first ID in a fresh log. `status`
reads one snapshot, so repeat it if the job is still queued or running.
The programs remain foreground processes; stop them with Ctrl+C.

On subsequent coordinator starts, reuse the same path **without** `--init-wal`:

```sh
faultline-coordinator --wal "$HOME/.local/state/faultline/coordinator.wal"
```

Start workers again after a coordinator restart. Installation does not change
connection handling, retries, durability, or networking. The default WAL is still
`faultline.wal` relative to the coordinator's current directory if `--wal` is
omitted; the installer does not set a new default or move existing logs.
To keep existing jobs, point `--wal` at your existing log's absolute path.

## Installation variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `PREFIX` | `/usr/local` | Base installation directory |
| `BINDIR` | `$(PREFIX)/bin` | Directory containing the three commands |
| `DESTDIR` | Empty | Temporary root prepended to `BINDIR` for staging |
| `INSTALL` | `install` | Utility used to create the directory and copy files |

Use absolute `PREFIX` / `BINDIR` paths. Destination paths containing spaces are
supported when the Make arguments are quoted. Commands receive mode `755`:
the owner can read, write, and execute; others can read and execute.

Plain `make install` uses `/usr/local/bin`. For a system-wide installation where
that location requires administrator access, first build as your ordinary user:

```sh
make
sudo make install
```

The personal installation above is enough for one account. A custom command
directory is also supported, for example `make install BINDIR="$HOME/bin"`.
Add whichever command directory you choose to `PATH`.

For packaging or inspection without writing to the real `/usr/local/bin`:

```sh
make install DESTDIR="$PWD/build/install-stage" PREFIX=/usr/local
# Files appear under build/install-stage/usr/local/bin/.
```

`DESTDIR` changes where files are copied; it is not compiled into the executables.
This is staging support, not a generated platform package or a prebuilt release.

## Builds, updates, and removal

`install` depends on `all`: a fresh checkout builds all three programs before
copying them. Existing up-to-date builds are reused. Normal Make defaults remain
`-O0 -g` under `build/debug`; installation does not select a separate optimized
build. `make SANITIZE=1 install PREFIX=...` deliberately installs the instrumented
copies from `build/sanitize`, intended for debugging rather than timing results.

Make does not detect changes to compiler/flag values alone. Use a fresh source
checkout when changing `CC` or `CFLAGS`, or rebuild after preserving any evidence
you need before `make clean` (which deletes all of `build/`).

To update an installation, stop its workers and coordinator, build the intended
source revision, then repeat `make install` with the same destination settings.
Restart with the retained WAL. A failed copy returns a nonzero Make status;
installation of the three files is not one atomic transaction, so resolve the
error and reinstall all three before restarting.

To uninstall the personal installation:

```sh
make uninstall PREFIX="$HOME/.local"
```

Repeat any `BINDIR` / `DESTDIR` overrides used for installation. Uninstall does
not need a compiler or compiled files. It removes only the exact three command
paths, keeps their directories and unrelated files, and can be repeated safely.
It neither deletes WALs nor stops running processes. It removes those paths
without tracking which source revision installed them.

## Verification

```sh
make test-install
make SANITIZE=1 test-install
```

Six regression checks build from a fresh temporary source tree using the chosen
`CC`, `CFLAGS`, and `SANITIZE` settings. They cover personal prefixes with spaces,
the default staged layout, a custom `BINDIR`, reinstall, installation errors,
and uninstall preserving other files without a compiler.

The live check puts the installed directory on `PATH`, temporarily makes the
original source/build path unavailable, and starts the commands by their short
names in separate working directories. It verifies PING/PONG, registration,
submission, the exact completed result and retry fields, unknown-ID exit status,
then restart/replay of the same job from an absolute WAL path. All launched
processes are stopped and reaped; their logs are checked for sanitizer errors.

The suite uses temporary directories, with bounded subprocess waits. It does not
install into your home directory or `/usr/local`. [Linux CI](ci.md) runs it in
both compiler jobs under a 90-second deadline and retains `install.log`.

Background services, automatic startup/reconnection, prebuilt downloads, and
package-manager distribution remain separate follow-up work.
