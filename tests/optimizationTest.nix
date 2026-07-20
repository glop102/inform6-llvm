{ lib, runCommand, writeShellApplication, coreutils, diffutils, gnugrep
, glulxe, glulxe-counted, inform6-llvm, compiledStories }:

let
  compileLog = runCommand "optimization-llvm.compile.log" { } ''
    work=$(mktemp -d)
    cd "$work"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/optimization-regressions.inf} opt.ulx >"$out" 2>&1
  '';
  jumpabsLlvmLog = runCommand "jumpabs-warning-llvm.compile.log" { } ''
    work=$(mktemp -d)
    cd "$work"
    ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/jumpabs-warning.inf} jumpabs.ulx >"$out" 2>&1
  '';
  jumpabsClassicLog = runCommand "jumpabs-warning-classic.compile.log" { } ''
    work=$(mktemp -d)
    cd "$work"
    ${lib.getExe inform6-llvm} -G '$LLVM=0' \
      ${../stories/jumpabs-warning.inf} jumpabs.ulx >"$out" 2>&1
  '';
  classic = compiledStories.classic.optimization-regressions;
  direct = compiledStories.direct.optimization-regressions;
  faultClassic = compiledStories.classic.faulting-read;
  faultDirect = compiledStories.direct.faulting-read;
in
writeShellApplication {
  name = "inform6-llvm-test-optimization";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe glulxe-counted ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-opt.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    jumpabs_warning='LLVM optimization does not preserve generated code addresses used by @jumpabs'
    if [ "$(grep -acF "$jumpabs_warning" ${jumpabsLlvmLog})" -ne 1 ]; then
        echo "FAIL  optimization (missing or duplicate jumpabs warning)"
        fail=1
    fi
    if grep -aqF "$jumpabs_warning" ${jumpabsClassicLog}; then
        echo "FAIL  optimization (jumpabs warning emitted without optimization)"
        fail=1
    fi

    backend_re=$'^LLVM-BACKEND\tname=[^[:space:]]+\tbackend=direct\tstage=lower\tinput=[0-9]+\temitted=[0-9]+\treason=-$'
    if [ "$(grep -acE "$backend_re" ${compileLog})" -ne 15 ]; then
        echo "FAIL  optimization (backend-origin records do not cover direct routines)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=15 classic=0 fallback=0$' \
        ${compileLog})" -ne 1 ]; then
        echo "FAIL  optimization (aggregate backend totals are incorrect)"
        fail=1
    fi

    check_routine() {
        local name=$1 expected_in=$2 max_out=$3
        local re count line
        re="^LLVM: direct routine ''${name}: ([0-9]+) instructions -> ([0-9]+)$"
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

    check_routine Opt_StoreFusion 7 7
    check_routine Opt_CompareReturn 5 2
    check_routine Opt_SelectReturn 5 4
    check_routine Opt_BooleanTree 5 6
    check_routine Opt_LoopPhi 9 10
    check_routine Opt_InductionSelect 13 17
    check_routine Opt_BranchLayout 13 16
    check_routine Opt_SwitchOrder 12 14
    check_routine Opt_SwitchShared 8 12
    check_routine Opt_GlobalCoalesce 5 5
    check_routine Opt_CoalesceClobber 5 5

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
    run_story ${direct} "$work/direct.log" || fail=1
    if [ "$fail" -eq 0 ] && ! cmp -s "$work/classic.log" "$work/direct.log"; then
        echo "FAIL  optimization (classic and direct transcripts differ)"
        fail=1
    fi
    if [ "$fail" -eq 0 ] && ! grep -aq '^done\.$' "$work/direct.log"; then
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
    if run_faulting_story ${faultDirect} "$work/fault-direct.log"; then
        direct_fault_status=$FAULT_STATUS
    else
        fail=1
    fi
    if [ "$fail" -eq 0 ] && \
       { [ "$classic_fault_status" -ne "$direct_fault_status" ] || \
          ! cmp -s "$work/fault-classic.log" "$work/fault-direct.log"; }; then
        echo "FAIL  optimization (classic and direct fault behavior differs)"
        fail=1
    fi
    if [ "$fail" -eq 0 ] && \
        ! grep -aq 'Memory access out of range' "$work/fault-direct.log"; then
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
    if run_counted ${direct} "$work/direct.count"; then
        direct_count=$COUNTED_RESULT
    else
        fail=1
    fi
    if [ "$fail" -eq 0 ] && \
        { [ "$classic_count" -ne 422 ] || [ "$direct_count" -gt 444 ]; }; then
        echo "FAIL  optimization (dynamic instruction bound: classic $classic_count, direct $direct_count)"
        fail=1
    fi

    if [ "$fail" -eq 0 ]; then
        echo "ok    optimization (dynamic: classic $classic_count, direct $direct_count)"
    fi
    exit "$fail"
  '';
}
