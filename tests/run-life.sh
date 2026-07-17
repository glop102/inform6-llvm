#!/usr/bin/env bash
# Life benchmark: compile tests/life.inf both ways ($LLVM=0 classic,
# $LLVM=2 full pipeline), check that the simulations behave identically
# (transcripts must match except the self-reported "Elapsed:" line),
# and report repeated timing statistics plus deterministic dynamic instruction
# counts when glulxe-counted is available.
#
# Run from the tests/ directory. Requires ../inform6-llvm to be built and
# glulxe on the PATH (the nix devshell provides it; ../result/bin/glulxe
# from `nix build .#glulxe` works too).

set -u
cd "$(dirname "$0")"

BENCH_RUNS=${BENCH_RUNS:-5}
if [[ ! $BENCH_RUNS =~ ^[1-9][0-9]*$ ]]; then
    echo "FAIL  life (BENCH_RUNS must be a positive integer)"
    exit 1
fi

I6=../inform6-llvm
if command -v glulxe >/dev/null 2>&1; then
    GLULXE=glulxe
elif [ -x ../result/bin/glulxe ]; then
    GLULXE=../result/bin/glulxe
else
    echo "skip  life (glulxe not found; enter the devshell or nix build .#glulxe)"
    exit 0
fi

if ! $I6 -G '$LLVM=0' life.inf life.classic.ulx >/dev/null 2>&1; then
    echo "FAIL  life (classic compile failed)"; exit 1
fi
if ! $I6 -G '$LLVM=2' life.inf life.llvm.ulx >/dev/null 2>&1; then
    echo "FAIL  life (llvm compile failed)"; exit 1
fi

if timeout 120 "$GLULXE" life.classic.ulx < /dev/null > life.classic.log 2>&1; then
    :
else
    status=$?
    echo "FAIL  life (classic interpreter exited $status; see life.classic.log)"
    exit 1
fi
if timeout 120 "$GLULXE" life.llvm.ulx < /dev/null > life.llvm.log 2>&1; then
    :
else
    status=$?
    echo "FAIL  life (llvm interpreter exited $status; see life.llvm.log)"
    exit 1
fi

check_output() {
    local log=$1
    if ! diff <(grep -v '^Elapsed:' life.classic.log) \
              <(grep -v '^Elapsed:' "$log") >/dev/null; then
        echo "FAIL  life (simulation differs; see life.classic.log / $log)"
        exit 1
    fi
}

elapsed_ms() {
    local log=$1 line
    line=$(grep '^Elapsed: [0-9][0-9]* ms$' "$log" || true)
    if [ "$(grep -c '^Elapsed: [0-9][0-9]* ms$' "$log")" -ne 1 ]; then
        echo "FAIL  life (malformed elapsed time in $log)" >&2
        return 1
    fi
    line=${line#Elapsed: }
    printf '%s\n' "${line% ms}"
}

check_output life.llvm.log
if ! classic_elapsed=$(elapsed_ms life.classic.log); then exit 1; fi
if ! llvm_elapsed=$(elapsed_ms life.llvm.log); then exit 1; fi
classic_times=("$classic_elapsed")
llvm_times=("$llvm_elapsed")

run_timed() {
    local kind=$1 story=$2 log=$3 run=$4 status
    if timeout 120 "$GLULXE" "$story" < /dev/null >"$log" 2>&1; then
        return 0
    else
        status=$?
    fi
    echo "FAIL  life ($kind interpreter exited $status on run $run; see $log)"
    return 1
}

for ((run = 2; run <= BENCH_RUNS; run++)); do
    classic_log="life.classic.$run.log"
    llvm_log="life.llvm.$run.log"
    if ((run % 2 == 0)); then
        run_timed llvm life.llvm.ulx "$llvm_log" "$run" || exit 1
        run_timed classic life.classic.ulx "$classic_log" "$run" || exit 1
    else
        run_timed classic life.classic.ulx "$classic_log" "$run" || exit 1
        run_timed llvm life.llvm.ulx "$llvm_log" "$run" || exit 1
    fi
    check_output "$classic_log"
    check_output "$llvm_log"
    if ! classic_elapsed=$(elapsed_ms "$classic_log"); then exit 1; fi
    if ! llvm_elapsed=$(elapsed_ms "$llvm_log"); then exit 1; fi
    classic_times+=("$classic_elapsed")
    llvm_times+=("$llvm_elapsed")
done

timing_summary() {
    local -n values=$1
    local -a sorted
    local median_index
    mapfile -t sorted < <(printf '%s\n' "${values[@]}" | sort -n)
    median_index=$(( (${#sorted[@]} - 1) / 2 ))
    printf 'median %s ms (min %s, max %s, n=%s)' \
        "${sorted[$median_index]}" "${sorted[0]}" \
        "${sorted[${#sorted[@]}-1]}" "${#sorted[@]}"
}

echo "ok    life"
echo "      classic: $(timing_summary classic_times)"
echo "      llvm:    $(timing_summary llvm_times)"

if command -v glulxe-counted >/dev/null 2>&1; then
    GLULXE_COUNTED=$(command -v glulxe-counted)
elif [ -x ../result-counted/bin/glulxe-counted ]; then
    GLULXE_COUNTED=../result-counted/bin/glulxe-counted
elif [ "${REQUIRE_GLULXE_COUNTED:-0}" = 1 ]; then
    echo "FAIL  life (glulxe-counted is required but was not found)"
    exit 1
else
    echo "      dynamic: skipped (glulxe-counted not found)"
    exit 0
fi

COUNTED_HISTOGRAM=0
if "$GLULXE_COUNTED" -help 2>&1 | grep -q -- '--opcode-histogram'; then
    COUNTED_HISTOGRAM=1
fi

count_story() {
    local story=$1 log=$2 status line re histogram_re histogram_sum
    local -a options=()
    if [ "$COUNTED_HISTOGRAM" -eq 1 ]; then
        options+=(--opcode-histogram)
    fi
    if timeout 120 "$GLULXE_COUNTED" "${options[@]}" "$story" \
            </dev/null >/dev/null 2>"$log"; then
        :
    else
        status=$?
        echo "FAIL  life ($story counted interpreter exited $status)"
        return 1
    fi
    re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
    line=$(grep -aE "$re" "$log" || true)
    if [ "$(grep -acE "$re" "$log")" -ne 1 ] || [[ ! $line =~ $re ]]; then
        echo "FAIL  life ($story has malformed dynamic count)"
        return 1
    fi
    COUNTED_RESULT=${BASH_REMATCH[1]}
    if [ "$COUNTED_HISTOGRAM" -eq 0 ]; then
        return 0
    fi
    histogram_re='^GLULXE_OPCODE_COUNT_0x[0-9A-F]+=([0-9]+)$'
    if ! grep -aqE "$histogram_re" "$log"; then
        echo "FAIL  life ($story has no opcode histogram)"
        return 1
    fi
    histogram_sum=$(grep -aE "$histogram_re" "$log" |
        awk -F= '{ sum += $2 } END { print sum + 0 }')
    if [ "$histogram_sum" -ne "$COUNTED_RESULT" ]; then
        echo "FAIL  life ($story opcode histogram sums to $histogram_sum, expected $COUNTED_RESULT)"
        return 1
    fi
}

write_opcode_comparison() {
    local classic_log=$1 llvm_log=$2 output=$3
    {
        printf 'opcode\tclassic\tllvm\tdelta\n'
        awk -F= '
            /^GLULXE_OPCODE_COUNT_0x[0-9A-F]+=[0-9]+$/ {
                opcode = $1
                sub(/^GLULXE_OPCODE_COUNT_/, "", opcode)
                if (FNR == NR) classic[opcode] = $2
                else llvm[opcode] = $2
                seen[opcode] = 1
            }
            END {
                for (number = 0; number < 576; number++) {
                    opcode = sprintf("0x%03X", number)
                    if (opcode in seen)
                        print opcode "\t" classic[opcode] + 0 "\t" \
                            llvm[opcode] + 0 "\t" llvm[opcode] - classic[opcode]
                }
            }
        ' "$classic_log" "$llvm_log"
    } >"$output"
}

COUNTED_RESULT=0
count_story life.classic.ulx life.classic.count.log || exit 1
classic_count=$COUNTED_RESULT
count_story life.llvm.ulx life.llvm.count.log || exit 1
llvm_count=$COUNTED_RESULT
delta=$((llvm_count - classic_count))
percent=$(awk -v delta="$delta" -v base="$classic_count" \
    'BEGIN { printf "%+.2f%%", 100 * delta / base }')
echo "      dynamic: classic $classic_count, llvm $llvm_count ($percent)"
if [ "$COUNTED_HISTOGRAM" -eq 1 ]; then
    write_opcode_comparison life.classic.count.log life.llvm.count.log life.opcodes.tsv
    echo "      opcodes: life.opcodes.tsv"
else
    echo "      opcodes: skipped (glulxe-counted lacks --opcode-histogram; refresh the dev shell)"
fi
