{ lib, runCommand, writeShellApplication, writeText, coreutils, diffutils, gnugrep
, glulxe, glulxe-counted, inform6-llvm, inform6-upstream }:

let
  divisionOverflowSource = writeText "direct-ir-division-overflow.inf" ''
    [ Main x;
        x = $80000000;
        return x / -1;
    ];
  '';
  remainderOverflowSource = writeText "direct-ir-remainder-overflow.inf" ''
    [ Main x;
        x = $80000000;
        return x % -1;
    ];
  '';
  uncheckedDivisionSource = writeText "direct-ir-unchecked-division.inf" ''
    [ Main x y;
        x = 1;
        y = 0;
        return x / y;
    ];
  '';
  uncheckedRemainderSource = writeText "direct-ir-unchecked-remainder.inf" ''
    [ Main x y;
        x = 1;
        y = 0;
        return x % y;
    ];
  '';
  direct = runCommand "direct-ir-build" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  quietDirect = runCommand "direct-ir-quiet-build" { } ''
    mkdir "$out"
    ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  upstream = runCommand "direct-ir-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G \
      ${../stories/direct-ir-phase1.inf} "$out"
  '';
  forkClassic = runCommand "direct-ir-fork-classic.ulx" { } ''
    ${lib.getExe inform6-llvm} -G '$LLVM=0' \
      ${../stories/direct-ir-phase1.inf} "$out"
  '';
  forcedFallback = runCommand "direct-ir-forced-fallback" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 I6_LLVM_DIRECT_FAIL=1 \
      ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  forcedLowerFallback = runCommand "direct-ir-forced-lower-fallback" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 I6_LLVM_DIRECT_FAIL=lower \
      ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  compileError = runCommand "direct-ir-error.compile.log" { } ''
    work=$(mktemp -d)
    set +e
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-error.inf} "$work/error.ulx" >"$out" 2>&1
    status=$?
    set -e
    test "$status" -ne 0
    test ! -e "$work/error.ulx"
  '';
  divisionOverflowDirect = runCommand "direct-ir-division-overflow" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${divisionOverflowSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  divisionOverflowUpstream = runCommand "direct-ir-division-overflow-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G ${divisionOverflowSource} "$out"
  '';
  remainderOverflowDirect = runCommand "direct-ir-remainder-overflow" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${remainderOverflowSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  remainderOverflowUpstream = runCommand "direct-ir-remainder-overflow-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G ${remainderOverflowSource} "$out"
  '';
  uncheckedDivisionDirect = runCommand "direct-ir-unchecked-division" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -~S -G '$LLVM=4' \
      ${uncheckedDivisionSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  uncheckedDivisionUpstream = runCommand "direct-ir-unchecked-division-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -~S -G ${uncheckedDivisionSource} "$out"
  '';
  uncheckedRemainderDirect = runCommand "direct-ir-unchecked-remainder" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -~S -G '$LLVM=4' \
      ${uncheckedRemainderSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  uncheckedRemainderUpstream = runCommand "direct-ir-unchecked-remainder-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -~S -G ${uncheckedRemainderSource} "$out"
  '';
in
writeShellApplication {
  name = "inform6-llvm-test-direct-ir";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe glulxe-counted ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-direct.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    direct_re=$'^LLVM-BACKEND\tname=Direct_(Constant|Parameter|Assignment|Branch|Arithmetic|Bitwise|GlobalRead|GlobalWrite|Chain|AssignmentValue|Symbol|Divide|Remainder|NonnegativeDivide|Divisor|Modulus|Compare|CompareAssignment|CompareOr|CompareOrOrder|CompareOrPredicates|PreInc|PostInc|PreDec|PostDec|GlobalInc|EvalOrder|Comma|Wrap)\tbackend=direct\tstage=lower\tinput=[0-9]+\temitted=[0-9]+\treason=-$'
    if [ "$(grep -acE "$direct_re" ${direct}/compile.log)" -ne 29 ]; then
        echo "FAIL  direct-ir (supported routines did not all use direct IR)"
        fail=1
    fi
    if grep -aq '^LLVM-BACKEND' ${quietDirect}/compile.log; then
        echo "FAIL  direct-ir (quiet direct mode emitted per-routine diagnostics)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=29 lifted=0 fallback=13$' \
        ${direct}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (aggregate backend totals are incorrect)"
        fail=1
    fi

    check_routine() {
        local name=$1 expected_in=$2 max_out=$3 re line
        re="^LLVM: direct routine ''${name}: ([0-9]+) instructions -> ([0-9]+)$"
        line=$(grep -aE "$re" ${direct}/compile.log || true)
        if [ "$(grep -acE "$re" ${direct}/compile.log)" -ne 1 ] || \
           [[ ! $line =~ $re ]] || \
           [ "''${BASH_REMATCH[1]:--1}" -ne "$expected_in" ] || \
           [ "''${BASH_REMATCH[2]:-9999}" -gt "$max_out" ]; then
            echo "FAIL  direct-ir ($name instruction bound: $line)"
            fail=1
        fi
    }
    check_routine Direct_Arithmetic 4 4
    check_routine Direct_Divide 2 2
    check_routine Direct_Remainder 2 2
    check_routine Direct_NonnegativeDivide 3 9
    check_routine Direct_Divisor 7 9
    check_routine Direct_Modulus 7 9
    check_routine Direct_Compare 35 30
    check_routine Direct_CompareAssignment 6 2
    check_routine Direct_CompareOr 7 3
    check_routine Direct_CompareOrOrder 16 1
    check_routine Direct_CompareOrPredicates 39 24
    check_routine Direct_GlobalInc 6 2
    if ! grep -aq $'name=Main\tbackend=classic-fallback\tstage=direct-build' \
        ${direct}/compile.log; then
        echo "FAIL  direct-ir (unsupported routine did not report fallback)"
        fail=1
    fi
    if ! grep -aq $'name=Direct_Inline\tbackend=classic-fallback\tstage=direct-build' \
        ${direct}/compile.log; then
        echo "FAIL  direct-ir (inline assembly did not force fallback)"
        fail=1
    fi
    if ! grep -aq $'name=Direct_Inline\tbackend=classic-fallback\tstage=direct-build\tinput=-1\temitted=-1\treason=inline assembly' \
        ${direct}/compile.log; then
        echo "FAIL  direct-ir (inline assembly fallback reason is missing)"
        fail=1
    fi
    if ! grep -aq $'name=Direct_Logical\tbackend=classic-fallback\tstage=direct-build\tinput=-1\temitted=-1\treason=unsupported expression operator' \
        ${direct}/compile.log; then
        echo "FAIL  direct-ir (unsupported expression diagnostic is missing)"
        fail=1
    fi
    timeout 30 glulxe ${upstream} </dev/null >"$work/upstream.log" 2>&1 || fail=1
    timeout 30 glulxe ${direct}/story.ulx </dev/null >"$work/direct.log" 2>&1 || fail=1
    if ! cmp -s "$work/upstream.log" "$work/direct.log"; then
        echo "FAIL  direct-ir (upstream and direct transcripts differ)"
        fail=1
    fi
    run_counted() {
        local story=$1 log=$2 line re
        if ! timeout 30 glulxe-counted "$story" </dev/null >/dev/null 2>"$log"; then
            echo "FAIL  direct-ir ($(basename "$story") counted run failed)"
            fail=1
            COUNTED_RESULT=-1
            return
        fi
        re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
        line=$(grep -aE "$re" "$log" || true)
        if [ "$(grep -acE "$re" "$log")" -ne 1 ] || [[ ! $line =~ $re ]]; then
            echo "FAIL  direct-ir ($(basename "$story") has malformed count)"
            fail=1
            COUNTED_RESULT=-1
            return
        fi
        COUNTED_RESULT=''${BASH_REMATCH[1]}
    }
    run_counted ${upstream} "$work/upstream.count"
    upstream_count=$COUNTED_RESULT
    run_counted ${direct}/story.ulx "$work/direct.count"
    direct_count=$COUNTED_RESULT
    if [ "$upstream_count" -ne 327 ] || [ "$direct_count" -gt 271 ]; then
        echo "FAIL  direct-ir (dynamic instruction bound: upstream $upstream_count, direct $direct_count)"
        fail=1
    fi
    if ! grep -aq $'name=Main\tbackend=direct\tstage=lower' \
        ${divisionOverflowDirect}/compile.log; then
        echo "FAIL  direct-ir (division overflow routine did not use direct IR)"
        fail=1
    fi
    set +e
    timeout 30 glulxe ${divisionOverflowUpstream} </dev/null \
        >"$work/overflow-upstream.log" 2>&1
    overflow_upstream_status=$?
    timeout 30 glulxe ${divisionOverflowDirect}/story.ulx </dev/null \
        >"$work/overflow-direct.log" 2>&1
    overflow_direct_status=$?
    set -e
    if [ "$overflow_upstream_status" -eq 124 ] || \
       [ "$overflow_direct_status" -eq 124 ] || \
       [ "$overflow_upstream_status" -ne "$overflow_direct_status" ] || \
       ! cmp -s "$work/overflow-upstream.log" "$work/overflow-direct.log"; then
        echo "FAIL  direct-ir (signed division overflow behavior differs)"
        fail=1
    fi
    if ! grep -aq $'name=Main\tbackend=direct\tstage=lower' \
        ${remainderOverflowDirect}/compile.log; then
        echo "FAIL  direct-ir (remainder overflow routine did not use direct IR)"
        fail=1
    fi
    set +e
    timeout 30 glulxe ${remainderOverflowUpstream} </dev/null \
        >"$work/remainder-upstream.log" 2>&1
    remainder_upstream_status=$?
    timeout 30 glulxe ${remainderOverflowDirect}/story.ulx </dev/null \
        >"$work/remainder-direct.log" 2>&1
    remainder_direct_status=$?
    set -e
    if [ "$remainder_upstream_status" -eq 124 ] || \
       [ "$remainder_direct_status" -eq 124 ] || \
       [ "$remainder_upstream_status" -ne "$remainder_direct_status" ] || \
       ! cmp -s "$work/remainder-upstream.log" "$work/remainder-direct.log"; then
        echo "FAIL  direct-ir (signed remainder overflow behavior differs)"
        fail=1
    fi

    check_unchecked_fault() {
        local name=$1 upstream_story=$2 direct_build=$3
        local upstream_status direct_status
        if ! grep -aq $'name=Main\tbackend=direct\tstage=lower' \
            "$direct_build/compile.log"; then
            echo "FAIL  direct-ir ($name routine did not use direct IR)"
            fail=1
        fi
        set +e
        timeout 30 glulxe "$upstream_story" </dev/null \
            >"$work/$name-upstream.log" 2>&1
        upstream_status=$?
        timeout 30 glulxe "$direct_build/story.ulx" </dev/null \
            >"$work/$name-direct.log" 2>&1
        direct_status=$?
        set -e
        if [ "$upstream_status" -eq 124 ] || [ "$direct_status" -eq 124 ] || \
           [ "$upstream_status" -ne "$direct_status" ] || \
           ! cmp -s "$work/$name-upstream.log" "$work/$name-direct.log"; then
            echo "FAIL  direct-ir ($name VM fault behavior differs)"
            fail=1
        fi
    }
    check_unchecked_fault unchecked-division \
        ${uncheckedDivisionUpstream} ${uncheckedDivisionDirect}
    check_unchecked_fault unchecked-remainder \
        ${uncheckedRemainderUpstream} ${uncheckedRemainderDirect}

    if ! cmp -s ${upstream} ${forkClassic}; then
        echo "FAIL  direct-ir (upstream and fork classic story files differ)"
        fail=1
    fi

    if ! cmp -s ${forkClassic} ${forcedFallback}/story.ulx; then
        echo "FAIL  direct-ir (forced failure did not replay shadow assembly)"
        fail=1
    fi
    forced_total=$(grep -ac '^LLVM-BACKEND' ${forcedFallback}/compile.log)
    forced_fallback=$(grep -ac \
        $'backend=classic-fallback\tstage=direct-build' \
        ${forcedFallback}/compile.log)
    if [ "$forced_total" -lt 5 ] || [ "$forced_fallback" -ne "$forced_total" ]; then
        echo "FAIL  direct-ir (forced fallback diagnostics are incomplete)"
        fail=1
    fi
    if ! cmp -s ${forkClassic} ${forcedLowerFallback}/story.ulx; then
        echo "FAIL  direct-ir (lowering failure did not preserve shadow assembly)"
        fail=1
    fi
    if [ "$(grep -ac $'backend=classic-fallback\tstage=direct-lower' \
        ${forcedLowerFallback}/compile.log)" -ne 29 ]; then
        echo "FAIL  direct-ir (forced lowering fallback diagnostics are incomplete)"
        fail=1
    fi
    if ! grep -aq 'Error:' ${compileError}; then
        echo "FAIL  direct-ir (error fixture did not report a source error)"
        fail=1
    fi
    if ! grep -aq $'backend=classic-fallback\tstage=direct-abandon' \
        ${compileError}; then
        echo "FAIL  direct-ir (error fixture did not abandon direct IR)"
        fail=1
    fi

    if [ "$fail" -eq 0 ]; then
        echo "ok    direct-ir"
    fi
    exit "$fail"
  '';
}
