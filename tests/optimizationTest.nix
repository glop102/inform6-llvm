{ lib, runCommand, writeShellApplication, coreutils, diffutils, gnugrep
, glulxe, glulxe-counted, inform6-llvm, compiledStories }:

let
  compileLog = runCommand "optimization-llvm.compile.log" { } ''
    work=$(mktemp -d)
    cd "$work"
    ${lib.getExe inform6-llvm} -G '$LLVM=3' \
      ${../stories/optimization-regressions.inf} opt.ulx >"$out" 2>&1
  '';
  classic = compiledStories.classic.optimization-regressions;
  llvm = compiledStories.llvm.optimization-regressions;
  faultClassic = compiledStories.classic.faulting-read;
  faultLlvm = compiledStories.llvm.faulting-read;
in
writeShellApplication {
  name = "inform6-llvm-test-optimization";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe glulxe-counted ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-opt.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    if grep -aqE '^! LLVM: (bailed on|could not lower) ' ${compileLog}; then
        echo "FAIL  optimization (unexpected LLVM bailout)"
        grep -aE '^! LLVM: (bailed on|could not lower) ' ${compileLog} |
            while IFS= read -r line; do printf '      %s\n' "$line"; done
        fail=1
    fi

    stats_re='^LLVM: optimized ([0-9]+) of ([0-9]+) captured routines \(([0-9]+) not lifted, ([0-9]+) not lowered\); ([0-9]+) instructions -> ([0-9]+)$'
    stats_count=$(grep -acE "$stats_re" ${compileLog})
    stats_line=$(grep -aE "$stats_re" ${compileLog} || true)
    if [ "$stats_count" -ne 1 ] || [[ ! $stats_line =~ $stats_re ]]; then
        echo "FAIL  optimization (missing or malformed aggregate statistics)"
        fail=1
    else
        optimized=''${BASH_REMATCH[1]}
        captured=''${BASH_REMATCH[2]}
        not_lifted=''${BASH_REMATCH[3]}
        not_lowered=''${BASH_REMATCH[4]}
        insts_in=''${BASH_REMATCH[5]}
        insts_out=''${BASH_REMATCH[6]}
        if [ "$optimized" -ne 12 ] || [ "$captured" -ne 12 ] || \
           [ "$not_lifted" -ne 0 ] || [ "$not_lowered" -ne 0 ]; then
            echo "FAIL  optimization (lowering coverage: $stats_line)"
            fail=1
        fi
        if [ "$insts_in" -ne 156 ] || [ "$insts_out" -gt 175 ]; then
            echo "FAIL  optimization (aggregate instruction bound: $stats_line)"
            fail=1
        fi
    fi

    check_routine() {
        local name=$1 expected_in=$2 max_out=$3
        local re count line
        re="^LLVM: routine ''${name}: ([0-9]+) instructions -> ([0-9]+)$"
        count=$(grep -acE "$re" ${compileLog})
        line=$(grep -aE "$re" ${compileLog} || true)
        if [ "$count" -ne 1 ] || [[ ! $line =~ $re ]]; then
            echo "FAIL  optimization ($name has missing or malformed statistics)"
            fail=1
            return
        fi
        if [ "''${BASH_REMATCH[1]}" -ne "$expected_in" ] || \
           [ "''${BASH_REMATCH[2]}" -gt "$max_out" ]; then
            echo "FAIL  optimization ($name instruction bound: $line)"
            fail=1
        fi
    }

    check_routine Opt_StoreFusion 7 8
    check_routine Opt_CompareReturn 5 2
    check_routine Opt_SelectReturn 5 4
    check_routine Opt_BooleanTree 5 10
    check_routine Opt_LoopPhi 9 11
    check_routine Opt_InductionSelect 13 19
    check_routine Opt_BranchLayout 13 17
    check_routine Opt_SwitchOrder 12 14
    check_routine Opt_SwitchShared 8 11

    run_story() {
        local story=$1 log=$2 status
        if timeout 30 glulxe "$story" </dev/null >"$log" 2>&1; then
            return 0
        else
            status=$?
        fi
        echo "FAIL  optimization ($(basename "$story") interpreter exited $status)"
        return 1
    }

    run_story ${classic} "$work/classic.log" || fail=1
    run_story ${llvm} "$work/llvm.log" || fail=1
    if [ "$fail" -eq 0 ] && ! cmp -s "$work/classic.log" "$work/llvm.log"; then
        echo "FAIL  optimization (classic and LLVM transcripts differ)"
        fail=1
    fi
    if [ "$fail" -eq 0 ] && ! grep -aq '^done\.$' "$work/llvm.log"; then
        echo "FAIL  optimization (completion marker missing)"
        fail=1
    fi

    run_faulting_story() {
        local story=$1 log=$2 status
        if timeout 30 glulxe "$story" </dev/null >"$log" 2>&1; then
            status=0
        else
            status=$?
        fi
        if [ "$status" -eq 124 ]; then
            echo "FAIL  optimization ($(basename "$story") timed out)"
            return 1
        fi
        FAULT_STATUS=$status
    }

    FAULT_STATUS=0
    if run_faulting_story ${faultClassic} "$work/fault-classic.log"; then
        classic_fault_status=$FAULT_STATUS
    else
        fail=1
    fi
    if run_faulting_story ${faultLlvm} "$work/fault-llvm.log"; then
        llvm_fault_status=$FAULT_STATUS
    else
        fail=1
    fi
    if [ "$fail" -eq 0 ] && \
       { [ "$classic_fault_status" -ne "$llvm_fault_status" ] || \
         ! cmp -s "$work/fault-classic.log" "$work/fault-llvm.log"; }; then
        echo "FAIL  optimization (classic and LLVM fault behavior differs)"
        fail=1
    fi
    if [ "$fail" -eq 0 ] && \
       ! grep -aq 'Memory access out of range' "$work/fault-llvm.log"; then
        echo "FAIL  optimization (expected memory fault missing)"
        fail=1
    fi

    run_counted() {
        local story=$1 count_log=$2 status line re
        if timeout 30 glulxe-counted "$story" </dev/null >/dev/null 2>"$count_log"; then
            :
        else
            status=$?
            echo "FAIL  optimization ($(basename "$story") counted interpreter exited $status)"
            return 1
        fi
        re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
        line=$(grep -aE "$re" "$count_log" || true)
        if [ "$(grep -acE "$re" "$count_log")" -ne 1 ] || [[ ! $line =~ $re ]]; then
            echo "FAIL  optimization ($(basename "$story") has malformed dynamic count)"
            return 1
        fi
        COUNTED_RESULT=''${BASH_REMATCH[1]}
    }

    COUNTED_RESULT=0
    if run_counted ${classic} "$work/classic.count"; then
        classic_count=$COUNTED_RESULT
    else
        fail=1
    fi
    if run_counted ${llvm} "$work/llvm.count"; then
        llvm_count=$COUNTED_RESULT
    else
        fail=1
    fi
    if [ "$fail" -eq 0 ] && \
       { [ "$classic_count" -ne 396 ] || [ "$llvm_count" -gt 444 ]; }; then
        echo "FAIL  optimization (dynamic instruction bound: classic $classic_count, LLVM $llvm_count)"
        fail=1
    fi

    if [ "$fail" -eq 0 ]; then
        echo "ok    optimization (dynamic: classic $classic_count, LLVM $llvm_count)"
    fi
    exit "$fail"
  '';
}
