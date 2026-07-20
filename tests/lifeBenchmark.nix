{ lib, runCommand, writeShellApplication, coreutils, diffutils, gawk, gnugrep
, glulxe, glulxe-counted, inform6-llvm, compiledStories, python3 }:

let
  classic = compiledStories.classic.life;
  directBuild = runCommand "life-direct" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/life.inf} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  direct = "${directBuild}/story.ulx";
in
writeShellApplication {
  name = "inform6-llvm-benchmark-life";
  runtimeInputs = [ coreutils diffutils gawk gnugrep glulxe glulxe-counted python3 ];
  text = ''
    BENCH_RUNS=''${BENCH_RUNS:-5}
    if [[ ! $BENCH_RUNS =~ ^[1-9][0-9]*$ ]]; then
        echo "FAIL  life (BENCH_RUNS must be a positive integer)"
        exit 1
    fi

    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-life.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM

    coverage_re='^LLVM: backends direct=([0-9]+) classic=([0-9]+) fallback=([0-9]+)$'
    coverage_line=$(grep -aE "$coverage_re" ${directBuild}/compile.log || true)
    if [ "$(grep -acE "$coverage_re" ${directBuild}/compile.log)" -ne 1 ] || \
       [[ ! $coverage_line =~ $coverage_re ]]; then
        echo "FAIL  life (missing direct backend coverage)"
        exit 1
    fi
    direct_routines=''${BASH_REMATCH[1]}
    classic_routines=''${BASH_REMATCH[2]}
    fallback_routines=''${BASH_REMATCH[3]}
    direct_names=$(awk -F '\t' '
        $3 == "backend=direct" {
            sub(/^name=/, "", $2)
            names = names (names ? "," : "") $2
        }
        END { print names }
    ' ${directBuild}/compile.log)
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
    run_timed direct ${direct} "$work/life.direct.log" 1 || exit 1
    check_output "$work/life.direct.log"
    classic_elapsed=$(elapsed_ms "$work/life.classic.log") || exit 1
    direct_elapsed=$(elapsed_ms "$work/life.direct.log") || exit 1
    classic_times=("$classic_elapsed")
    direct_times=("$direct_elapsed")

    for ((run = 2; run <= BENCH_RUNS; run++)); do
        classic_log="$work/life.classic.$run.log"
        direct_log="$work/life.direct.$run.log"
        if ((run % 2 == 0)); then
            run_timed classic ${classic} "$classic_log" "$run" || exit 1
            run_timed direct ${direct} "$direct_log" "$run" || exit 1
        else
            run_timed direct ${direct} "$direct_log" "$run" || exit 1
            run_timed classic ${classic} "$classic_log" "$run" || exit 1
        fi
        check_output "$classic_log"
        check_output "$direct_log"
        classic_elapsed=$(elapsed_ms "$classic_log") || exit 1
        direct_elapsed=$(elapsed_ms "$direct_log") || exit 1
        classic_times+=("$classic_elapsed")
        direct_times+=("$direct_elapsed")
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
    echo "      direct:  $(timing_summary direct_times)"
    echo "      coverage: direct $direct_routines, classic $classic_routines, fallback $fallback_routines"
    echo "      direct routines: ''${direct_names:--}"

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

    COUNTED_RESULT=0
    count_story ${classic} "$work/life.classic.count.log" || exit 1
    classic_count=$COUNTED_RESULT
    count_story ${direct} "$work/life.direct.count.log" || exit 1
    direct_count=$COUNTED_RESULT
    direct_delta=$((direct_count - classic_count))
    direct_percent=$(awk -v delta="$direct_delta" -v base="$classic_count" \
        'BEGIN { printf "%+.2f%%", 100 * delta / base }')
    echo "      dynamic: classic $classic_count, direct $direct_count ($direct_percent)"
    if [ "$histogram" -eq 1 ]; then
        echo "      opcodes (direct - classic dispatches):"
        python3 ${./attrib.py} opcodes "$work/life.classic.count.log" \
            "$work/life.direct.count.log" 8
    else
        echo "      opcodes: skipped (glulxe-counted lacks --opcode-histogram)"
    fi
  '';
}
