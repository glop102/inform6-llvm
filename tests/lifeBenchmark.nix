{ writeShellApplication, coreutils, diffutils, gawk, gnugrep
, glulxe, glulxe-counted, compiledStories }:

let
  classic = compiledStories.classic.life;
  llvm = compiledStories.llvm.life;
in
writeShellApplication {
  name = "inform6-llvm-benchmark-life";
  runtimeInputs = [ coreutils diffutils gawk gnugrep glulxe glulxe-counted ];
  text = ''
    BENCH_RUNS=''${BENCH_RUNS:-5}
    if [[ ! $BENCH_RUNS =~ ^[1-9][0-9]*$ ]]; then
        echo "FAIL  life (BENCH_RUNS must be a positive integer)"
        exit 1
    fi

    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-life.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM

    run_timed() {
        local kind=$1 story=$2 log=$3 run=$4 status
        if timeout 120 glulxe "$story" </dev/null >"$log" 2>&1; then
            return 0
        else
            status=$?
        fi
        echo "FAIL  life ($kind interpreter exited $status on run $run)"
        return 1
    }

    check_output() {
        local log=$1
        if ! diff <(grep -v '^Elapsed:' "$work/life.classic.log") \
                  <(grep -v '^Elapsed:' "$log") >/dev/null; then
            echo "FAIL  life (simulation differs)"
            exit 1
        fi
    }

    elapsed_ms() {
        local log=$1 line
        line=$(grep '^Elapsed: [0-9][0-9]* ms$' "$log" || true)
        if [ "$(grep -c '^Elapsed: [0-9][0-9]* ms$' "$log")" -ne 1 ]; then
            echo "FAIL  life (malformed elapsed time)" >&2
            return 1
        fi
        line=''${line#Elapsed: }
        printf '%s\n' "''${line% ms}"
    }

    run_timed classic ${classic} "$work/life.classic.log" 1 || exit 1
    run_timed llvm ${llvm} "$work/life.llvm.log" 1 || exit 1
    check_output "$work/life.llvm.log"
    classic_elapsed=$(elapsed_ms "$work/life.classic.log") || exit 1
    llvm_elapsed=$(elapsed_ms "$work/life.llvm.log") || exit 1
    classic_times=("$classic_elapsed")
    llvm_times=("$llvm_elapsed")

    for ((run = 2; run <= BENCH_RUNS; run++)); do
        classic_log="$work/life.classic.$run.log"
        llvm_log="$work/life.llvm.$run.log"
        if ((run % 2 == 0)); then
            run_timed llvm ${llvm} "$llvm_log" "$run" || exit 1
            run_timed classic ${classic} "$classic_log" "$run" || exit 1
        else
            run_timed classic ${classic} "$classic_log" "$run" || exit 1
            run_timed llvm ${llvm} "$llvm_log" "$run" || exit 1
        fi
        check_output "$classic_log"
        check_output "$llvm_log"
        classic_elapsed=$(elapsed_ms "$classic_log") || exit 1
        llvm_elapsed=$(elapsed_ms "$llvm_log") || exit 1
        classic_times+=("$classic_elapsed")
        llvm_times+=("$llvm_elapsed")
    done

    timing_summary() {
        local -n values=$1
        local -a sorted
        local median_index
        mapfile -t sorted < <(printf '%s\n' "''${values[@]}" | sort -n)
        median_index=$(( (''${#sorted[@]} - 1) / 2 ))
        printf 'median %s ms (min %s, max %s, n=%s)' \
            "''${sorted[$median_index]}" "''${sorted[0]}" \
            "''${sorted[''${#sorted[@]}-1]}" "''${#sorted[@]}"
    }

    echo "ok    life"
    echo "      classic: $(timing_summary classic_times)"
    echo "      llvm:    $(timing_summary llvm_times)"

    histogram=0
    counted_help=$(glulxe-counted -help 2>&1 || true)
    if grep -q -- '--opcode-histogram' <<<"$counted_help"; then
        histogram=1
    fi

    count_story() {
        local story=$1 log=$2 status line re histogram_re histogram_sum
        local -a options=()
        if [ "$histogram" -eq 1 ]; then options+=(--opcode-histogram); fi
        if timeout 120 glulxe-counted "''${options[@]}" "$story" \
                </dev/null >/dev/null 2>"$log"; then
            :
        else
            status=$?
            echo "FAIL  life (counted interpreter exited $status)"
            return 1
        fi
        re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
        line=$(grep -aE "$re" "$log" || true)
        if [ "$(grep -acE "$re" "$log")" -ne 1 ] || [[ ! $line =~ $re ]]; then
            echo "FAIL  life (malformed dynamic count)"
            return 1
        fi
        COUNTED_RESULT=''${BASH_REMATCH[1]}
        if [ "$histogram" -eq 0 ]; then return 0; fi
        histogram_re='^GLULXE_OPCODE_COUNT_0x[0-9A-F]+=([0-9]+)$'
        if ! grep -aqE "$histogram_re" "$log"; then
            echo "FAIL  life (no opcode histogram)"
            return 1
        fi
        histogram_sum=$(grep -aE "$histogram_re" "$log" |
            awk -F= '{ sum += $2 } END { print sum + 0 }')
        if [ "$histogram_sum" -ne "$COUNTED_RESULT" ]; then
            echo "FAIL  life (opcode histogram sums to $histogram_sum, expected $COUNTED_RESULT)"
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
    count_story ${classic} "$work/life.classic.count.log" || exit 1
    classic_count=$COUNTED_RESULT
    count_story ${llvm} "$work/life.llvm.count.log" || exit 1
    llvm_count=$COUNTED_RESULT
    delta=$((llvm_count - classic_count))
    percent=$(awk -v delta="$delta" -v base="$classic_count" \
        'BEGIN { printf "%+.2f%%", 100 * delta / base }')
    echo "      dynamic: classic $classic_count, llvm $llvm_count ($percent)"
    if [ "$histogram" -eq 1 ]; then
        write_opcode_comparison "$work/life.classic.count.log" \
            "$work/life.llvm.count.log" "$work/life.opcodes.tsv"
        echo "      opcodes: collected"
    else
        echo "      opcodes: skipped (glulxe-counted lacks --opcode-histogram)"
    fi
  '';
}
