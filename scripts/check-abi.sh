#!/bin/sh
# Fails if any generated ABI header copy (kernel/src/abi/*.h,
# userland/include/*.h) doesn't match what scripts/sync-abi.sh would
# produce right now -- i.e. someone edited a generated copy directly
# instead of editing abi/ and re-running `make sync-abi`. Intended for
# CI / pre-commit, not the normal build (the normal build just
# silently re-syncs -- see the top-level Makefile's `sync-abi` target).
set -eu
cd "$(dirname "$0")/.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for f in kernel/src/abi/syscall_nr.h kernel/src/abi/task_info.h \
         userland/include/syscall_nr.h userland/include/task_info.h; do
  mkdir -p "$tmp/$(dirname "$f")"
  [ -f "$f" ] && cp "$f" "$tmp/$f" || : > "$tmp/$f.missing"
done

./scripts/sync-abi.sh > /dev/null

status=0
for f in kernel/src/abi/syscall_nr.h kernel/src/abi/task_info.h \
         userland/include/syscall_nr.h userland/include/task_info.h; do
  if ! diff -u "$tmp/$f" "$f"; then
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "[check-abi] drift detected -- a generated copy was hand-edited." >&2
  echo "[check-abi] edit abi/*.h instead, then run 'make sync-abi'." >&2
fi
exit $status
