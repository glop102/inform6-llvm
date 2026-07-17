#!/usr/bin/env bash
# M3 gate: story files compiled through the full LLVM pipeline ($LLVM=2,
# the default) must behave identically to classic ones ($LLVM=0). The
# bytes are intentionally different, so compare glulxe transcripts.
#
# Run from the tests/ directory. Requires ../inform6-llvm to be built and
# glulxe on the PATH (the nix devshell provides it; ../result/bin/glulxe
# from `nix build .#glulxe` works too).

set -u
cd "$(dirname "$0")"

I6=../inform6-llvm
if command -v glulxe >/dev/null 2>&1; then
    GLULXE=glulxe
elif [ -x ../result/bin/glulxe ]; then
    GLULXE=../result/bin/glulxe
else
    echo "skip  all (glulxe not found; enter the devshell or nix build .#glulxe)"
    exit 0
fi
fail=0

run_story() {
    local seconds=$1 story=$2 input=$3 log=$4 status
    if timeout "$seconds" "$GLULXE" "$story" < "$input" > "$log" 2>&1; then
        return 0
    else
        status=$?
    fi
    echo "FAIL  $(basename "$story") (interpreter exited $status; see $log)"
    return 1
}

check() {
    local name=$1 input=$2; shift 2
    local flags=("$@")
    if ! $I6 "${flags[@]}" '$LLVM=0' "$name.inf" "$name.classic.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (classic compile failed)"; fail=1; return
    fi
    if ! $I6 "${flags[@]}" '$LLVM=2' "$name.inf" "$name.llvm.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (llvm compile failed)"; fail=1; return
    fi
    if ! run_story 30 "$name.classic.ulx" "$input" "$name.classic.log"; then
        fail=1; return
    fi
    if ! run_story 30 "$name.llvm.ulx" "$input" "$name.llvm.log"; then
        fail=1; return
    fi
    if cmp -s "$name.classic.log" "$name.llvm.log"; then
        echo "ok    $name"
    else
        echo "FAIL  $name (transcripts differ; see $name.classic.log / $name.llvm.log)"
        fail=1
    fi
}

# glulxercise (tests/glulxercise.inf, Andrew Plotkin's Glulx interpreter
# unit test from https://eblong.com/zarf/glulx/, public domain) is
# self-checking, so it gets its own gate instead of a transcript diff:
# several of its checks print layout-dependent values (heap addresses,
# memory sizes), which differ between builds legitimately. The classic
# build must pass everything; the LLVM build may fail only the known
# layout-sensitive checks:
#   - "jumpabs test=": test_jumpabs jumps to test_jumpabs_2+5, running
#     another routine's (optimized) body inside its own stack frame
#   - "token=": @catch tokens are stack addresses, and optimized frame
#     layouts differ
check_glulxercise() {
    local name=glulxercise
    if ! $I6 -G '$LLVM=0' "$name.inf" "$name.classic.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (classic compile failed)"; fail=1; return
    fi
    if ! $I6 -G '$LLVM=2' "$name.inf" "$name.llvm.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (llvm compile failed)"; fail=1; return
    fi
    if ! run_story 60 "$name.classic.ulx" "$name.walk" "$name.classic.log"; then
        fail=1; return
    fi
    if ! run_story 60 "$name.llvm.ulx" "$name.walk" "$name.llvm.log"; then
        fail=1; return
    fi
    # the logs contain raw high-bit bytes (char-output tests), so grep -a
    if ! grep -aq 'Goodbye' "$name.classic.log" || \
       ! grep -aq 'Goodbye' "$name.llvm.log"; then
        echo "FAIL  $name (a run crashed or hung; see $name.*.log)"
        fail=1; return
    fi
    if grep -aq 'FAIL' "$name.classic.log" || \
       [ "$(grep -ac 'All tests passed' "$name.classic.log")" -ne 3 ]; then
        echo "FAIL  $name (classic build fails its own checks; see $name.classic.log)"
        fail=1; return
    fi
    local bad fail_count jumpabs_count token_count
    bad=$(grep -a 'FAIL' "$name.llvm.log" | grep -av -e 'token=' -e 'jumpabs test=')
    fail_count=$(grep -ac 'FAIL' "$name.llvm.log")
    jumpabs_count=$(grep -ac 'jumpabs test=.*FAIL' "$name.llvm.log")
    token_count=$(grep -ac 'token=.*FAIL' "$name.llvm.log")
    if [ -n "$bad" ] || [ "$fail_count" -ne 11 ] ||
       [ "$jumpabs_count" -ne 1 ] || [ "$token_count" -ne 10 ]; then
        echo "FAIL  $name (llvm build fails non-layout-sensitive checks; see $name.llvm.log)"
        if [ -n "$bad" ]; then
            printf '%s\n' "$bad" | while IFS= read -r line; do
                printf '      %s\n' "$line"
            done
        fi
        fail=1; return
    fi
    echo "ok    $name"
}

check m3      /dev/null -G
check zregion /dev/null -G
check veneer  /dev/null -G
check_glulxercise
# Library via the devshell (INFORM6_LIB) with tests/lib as fallback; see
# the note in run-m1.sh.
LIBDIR=${INFORM6_LIB:-lib}
if [ -d "$LIBDIR" ]; then
    check cloak cloak.walk -G +include_path="$LIBDIR" +language_name=english
else
    echo "skip  cloak (no library: enter the devshell or clone tests/lib)"
fi

exit $fail
