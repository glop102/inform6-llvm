#!/usr/bin/env bash
# Focused LLVM optimization gate. This checks that known routines really pass
# through the lowerer and retain narrow static instruction-count bounds.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
I6="$ROOT/inform6-llvm"
SOURCE="$ROOT/tests/opt.inf"
fail=0

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/inform6-opt.XXXXXX") || exit 1
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

show_log() {
    while IFS= read -r line; do
        printf '      %s\n' "$line"
    done < "$1"
}

compile_log="$tmpdir/llvm.compile.log"
if (cd "$tmpdir" && LC_ALL=C "$I6" -G '$LLVM=3' "$SOURCE" opt.llvm.ulx) \
        >"$compile_log" 2>&1; then
    :
else
    status=$?
    echo "FAIL  opt (LLVM compile exited $status)"
    show_log "$compile_log"
    exit 1
fi

stub_line='LLVM: this compiler was built without LLVM support; all routines were compiled classically'
if grep -aFqx "$stub_line" "$compile_log"; then
    if [ "${REQUIRE_LLVM:-0}" = 1 ]; then
        echo "FAIL  opt (compiler uses the no-LLVM stub)"
        exit 1
    fi
    echo "skip  opt (compiler uses the no-LLVM stub; set REQUIRE_LLVM=1 to require it)"
    exit 0
fi

if grep -aqE '^! LLVM: (bailed on|could not lower) ' "$compile_log"; then
    echo "FAIL  opt (unexpected LLVM bailout)"
    grep -aE '^! LLVM: (bailed on|could not lower) ' "$compile_log" |
        while IFS= read -r line; do printf '      %s\n' "$line"; done
    fail=1
fi

stats_re='^LLVM: optimized ([0-9]+) of ([0-9]+) captured routines \(([0-9]+) not lifted, ([0-9]+) not lowered\); ([0-9]+) instructions -> ([0-9]+)$'
stats_count=$(grep -acE "$stats_re" "$compile_log")
stats_line=$(grep -aE "$stats_re" "$compile_log" || true)
if [ "$stats_count" -ne 1 ] || [[ ! $stats_line =~ $stats_re ]]; then
    echo "FAIL  opt (missing or malformed aggregate statistics)"
    fail=1
else
    optimized=${BASH_REMATCH[1]}
    captured=${BASH_REMATCH[2]}
    not_lifted=${BASH_REMATCH[3]}
    not_lowered=${BASH_REMATCH[4]}
    insts_in=${BASH_REMATCH[5]}
    insts_out=${BASH_REMATCH[6]}
    if [ "$optimized" -ne 11 ] || [ "$captured" -ne 11 ] ||
       [ "$not_lifted" -ne 0 ] || [ "$not_lowered" -ne 0 ]; then
        echo "FAIL  opt (lowering coverage: $stats_line)"
        fail=1
    fi
    if [ "$insts_in" -ne 136 ] || [ "$insts_out" -gt 152 ]; then
        echo "FAIL  opt (aggregate instruction bound: $stats_line)"
        fail=1
    fi
fi

check_routine() {
    local name=$1 expected_in=$2 max_out=$3
    local re count line
    re="^LLVM: routine ${name}: ([0-9]+) instructions -> ([0-9]+)$"
    count=$(grep -acE "$re" "$compile_log")
    line=$(grep -aE "$re" "$compile_log" || true)
    if [ "$count" -ne 1 ] || [[ ! $line =~ $re ]]; then
        echo "FAIL  opt ($name has missing or malformed statistics)"
        fail=1
        return
    fi
    if [ "${BASH_REMATCH[1]}" -ne "$expected_in" ] ||
       [ "${BASH_REMATCH[2]}" -gt "$max_out" ]; then
        echo "FAIL  opt ($name instruction bound: $line)"
        fail=1
    fi
}

check_routine Opt_StoreFusion  7  8
check_routine Opt_CompareReturn 5  2
check_routine Opt_SelectReturn  5  4
check_routine Opt_BooleanTree   5 10
check_routine Opt_LoopPhi       9 11
check_routine Opt_InductionSelect 13 19
check_routine Opt_BranchLayout 13 17
check_routine Opt_SwitchOrder  12 14

classic_log="$tmpdir/classic.compile.log"
if (cd "$tmpdir" && LC_ALL=C "$I6" -G '$LLVM=0' "$SOURCE" opt.classic.ulx) \
        >"$classic_log" 2>&1; then
    :
else
    status=$?
    echo "FAIL  opt (classic compile exited $status)"
    show_log "$classic_log"
    exit 1
fi

if command -v glulxe >/dev/null 2>&1; then
    GLULXE=$(command -v glulxe)
elif [ -x "$ROOT/result/bin/glulxe" ]; then
    GLULXE="$ROOT/result/bin/glulxe"
elif [ "${REQUIRE_GLULXE:-0}" = 1 ]; then
    echo "FAIL  opt (glulxe is required but was not found)"
    exit 1
else
    if [ "$fail" -eq 0 ]; then
        echo "ok    opt (counts; behavior skipped without glulxe)"
    fi
    exit "$fail"
fi

run_story() {
    local story=$1 log=$2 status
    if timeout 30 "$GLULXE" "$story" </dev/null >"$log" 2>&1; then
        return 0
    else
        status=$?
    fi
    echo "FAIL  opt ($(basename "$story") interpreter exited $status)"
    return 1
}

run_story "$tmpdir/opt.classic.ulx" "$tmpdir/classic.log" || fail=1
run_story "$tmpdir/opt.llvm.ulx" "$tmpdir/llvm.log" || fail=1

if [ "$fail" -eq 0 ] && ! cmp -s "$tmpdir/classic.log" "$tmpdir/llvm.log"; then
    echo "FAIL  opt (classic and LLVM transcripts differ)"
    fail=1
fi
if [ "$fail" -eq 0 ] && ! grep -aq '^done\.$' "$tmpdir/llvm.log"; then
    echo "FAIL  opt (completion marker missing)"
    fail=1
fi

if command -v glulxe-counted >/dev/null 2>&1; then
    GLULXE_COUNTED=$(command -v glulxe-counted)
elif [ -x "$ROOT/result-counted/bin/glulxe-counted" ]; then
    GLULXE_COUNTED="$ROOT/result-counted/bin/glulxe-counted"
elif [ "${REQUIRE_GLULXE_COUNTED:-0}" = 1 ]; then
    echo "FAIL  opt (glulxe-counted is required but was not found)"
    exit 1
else
    GLULXE_COUNTED=
fi

run_counted() {
    local story=$1 count_log=$2 status line re
    if timeout 30 "$GLULXE_COUNTED" "$story" </dev/null >/dev/null \
            2>"$count_log"; then
        :
    else
        status=$?
        echo "FAIL  opt ($(basename "$story") counted interpreter exited $status)"
        return 1
    fi
    re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
    line=$(grep -aE "$re" "$count_log" || true)
    if [ "$(grep -acE "$re" "$count_log")" -ne 1 ] || [[ ! $line =~ $re ]]; then
        echo "FAIL  opt ($(basename "$story") has malformed dynamic count)"
        return 1
    fi
    COUNTED_RESULT=${BASH_REMATCH[1]}
}

if [ -n "$GLULXE_COUNTED" ]; then
    COUNTED_RESULT=0
    if run_counted "$tmpdir/opt.classic.ulx" "$tmpdir/classic.count"; then
        classic_count=$COUNTED_RESULT
    else
        fail=1
    fi
    if run_counted "$tmpdir/opt.llvm.ulx" "$tmpdir/llvm.count"; then
        llvm_count=$COUNTED_RESULT
    else
        fail=1
    fi
    if [ "$fail" -eq 0 ] &&
       { [ "$classic_count" -ne 362 ] || [ "$llvm_count" -gt 395 ]; }; then
        echo "FAIL  opt (dynamic instruction bound: classic $classic_count, LLVM $llvm_count)"
        fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    if [ -n "$GLULXE_COUNTED" ]; then
        echo "ok    opt (dynamic: classic $classic_count, LLVM $llvm_count)"
    else
        echo "ok    opt (dynamic counts skipped without glulxe-counted)"
    fi
fi
exit "$fail"
