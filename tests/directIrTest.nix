{ lib, runCommand, writeShellApplication, coreutils, diffutils, gnugrep
, glulxe, inform6-llvm, inform6-upstream }:

let
  direct = runCommand "direct-ir-phase1-build" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  quietDirect = runCommand "direct-ir-phase1-quiet-build" { } ''
    mkdir "$out"
    ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  upstream = runCommand "direct-ir-phase1-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G \
      ${../stories/direct-ir-phase1.inf} "$out"
  '';
  forkClassic = runCommand "direct-ir-phase1-fork-classic.ulx" { } ''
    ${lib.getExe inform6-llvm} -G '$LLVM=0' \
      ${../stories/direct-ir-phase1.inf} "$out"
  '';
  forcedFallback = runCommand "direct-ir-phase1-forced-fallback" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 I6_LLVM_DIRECT_FAIL=1 \
      ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  forcedLowerFallback = runCommand "direct-ir-phase1-forced-lower-fallback" { } ''
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
in
writeShellApplication {
  name = "inform6-llvm-test-direct-ir";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-direct.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    direct_re=$'^LLVM-BACKEND\tname=Direct_(Constant|Parameter|Assignment|Branch)\tbackend=direct\tstage=lower\tinput=[0-9]+\temitted=[0-9]+\treason=-$'
    if [ "$(grep -acE "$direct_re" ${direct}/compile.log)" -ne 4 ]; then
        echo "FAIL  direct-ir (trivial routines did not all use direct IR)"
        fail=1
    fi
    if grep -aq '^LLVM-BACKEND' ${quietDirect}/compile.log; then
        echo "FAIL  direct-ir (quiet direct mode emitted per-routine diagnostics)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=4 lifted=0 fallback=4$' \
        ${direct}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (aggregate backend totals are incorrect)"
        fail=1
    fi
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

    timeout 30 glulxe ${upstream} </dev/null >"$work/upstream.log" 2>&1 || fail=1
    timeout 30 glulxe ${direct}/story.ulx </dev/null >"$work/direct.log" 2>&1 || fail=1
    if ! cmp -s "$work/upstream.log" "$work/direct.log"; then
        echo "FAIL  direct-ir (upstream and direct transcripts differ)"
        fail=1
    fi

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
        ${forcedLowerFallback}/compile.log)" -ne 4 ]; then
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
