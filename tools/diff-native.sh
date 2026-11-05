#!/usr/bin/env bash
# Checks that the native backend behaves like the PVM: for each program, compares stdout, stderr and the exit code of
# `pvm` and of the executable built by `coyote-llvm --native-only`.   (Motto: do not trust the PVM, worry about LLVM.)
#   tools/diff-native.sh <build-dir> <program.coy>...
# Prints SAME / DIFFERENT / NOT NATIVE (outside the native subset: such programs run as bundles, i.e. on the PVM itself).
# Exit code: 1 when any program is DIFFERENT. Needs clang.
set -u
bin="$1"; shift
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
bad=0
for src in "$@"; do
  n="$(basename "${src%.coy}")"
  if ! "$bin/coyote" build "$src" -o "$tmp/$n.ml" 2>"$tmp/$n.cerr"; then echo "$n: COMPILE ERROR: $(head -1 "$tmp/$n.cerr")"; continue; fi
  "$bin/pvm" "$tmp/$n.ml" >"$tmp/$n.pvm.out" 2>"$tmp/$n.pvm.err"; echo $? >"$tmp/$n.pvm.rc"
  if ! "$bin/coyote-llvm" --native-only "$tmp/$n.ml" -o "$tmp/$n.bin" >"$tmp/$n.llvm.log" 2>&1; then
    echo "$n: NOT NATIVE: $(head -1 "$tmp/$n.llvm.log" | cut -c1-140)"; continue; fi
  "$tmp/$n.bin" >"$tmp/$n.nat.out" 2>"$tmp/$n.nat.err"; echo $? >"$tmp/$n.nat.rc"
  if cmp -s "$tmp/$n.pvm.out" "$tmp/$n.nat.out" && cmp -s "$tmp/$n.pvm.err" "$tmp/$n.nat.err" && cmp -s "$tmp/$n.pvm.rc" "$tmp/$n.nat.rc"; then
    echo "$n: SAME (exit $(cat "$tmp/$n.pvm.rc"))"
  else
    echo "$n: DIFFERENT"; bad=1
    diff <(cat "$tmp/$n.pvm.out" "$tmp/$n.pvm.err" "$tmp/$n.pvm.rc") <(cat "$tmp/$n.nat.out" "$tmp/$n.nat.err" "$tmp/$n.nat.rc") | head -10
  fi
done
exit $bad
