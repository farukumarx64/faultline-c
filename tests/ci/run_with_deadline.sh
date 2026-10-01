#!/usr/bin/env bash
# Linux CI: bound a command and preserve failure even when tee succeeds.
set -euo pipefail

if [[ $# -lt 3 || ! "$1" =~ ^[1-9][0-9]*$ ]]; then
  echo "usage: bash run_with_deadline.sh POSITIVE_SECONDS LOG COMMAND [ARG ...]" >&2
  exit 2
fi

deadline=$1
log=$2
shift 2

# GNU timeout returns 124 on expiry (137 if SIGKILL is required). Neither is
# success. pipefail also makes an unwritable log fail an otherwise good command.
timeout --signal=TERM --kill-after=15s "${deadline}s" "$@" 2>&1 | tee "$log"
