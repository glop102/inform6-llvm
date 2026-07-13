#!/usr/bin/env bash
# M1 gate: with $LLVM=1, the capture/replay seam must produce story files
# byte-identical to the classic encoder ($LLVM=0).
#
# Run from the tests/ directory. Requires ../inform6-llvm to be built and,
# for cloak.inf, the library cloned at tests/lib (see README).

set -u
cd "$(dirname "$0")"

I6=../inform6-llvm
fail=0

check() {
    local name=$1; shift
    local flags=("$@")
    if ! $I6 "${flags[@]}" '$LLVM=0' "$name.inf" "$name.classic.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (classic compile failed)"; fail=1; return
    fi
    if ! $I6 "${flags[@]}" '$LLVM=1' "$name.inf" "$name.llvm.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (llvm compile failed)"; fail=1; return
    fi
    if cmp -s "$name.classic.ulx" "$name.llvm.ulx"; then
        echo "ok    $name"
    else
        echo "FAIL  $name (story files differ)"; fail=1
    fi
}

check hello       -G
check torture     -G
check veneer      -G
check glulxercise -G
# The library comes from the devshell (INFORM6_LIB, a flake input), with
# a local tests/lib clone as fallback outside the shell.
LIBDIR=${INFORM6_LIB:-lib}
if [ -d "$LIBDIR" ]; then
    # +language_name: the compiler's default is "English", but the library
    # ships the file as lowercase english.h (matters on case-sensitive FS).
    check cloak -G +include_path="$LIBDIR" +language_name=english
else
    echo "skip  cloak (no library: enter the devshell or clone tests/lib)"
fi

exit $fail
