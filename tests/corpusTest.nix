{ lib, runCommand, writeShellApplication, writeText, coreutils, diffutils, gnugrep
, glulxe, glulxe-counted, inform6-llvm, inform6-upstream, inform6lib }:

# Phase 4 corpus and compilation-mode gates: full-library and compliance
# stories compile through direct IR at exactly asserted coverage, and the
# one mode that bypasses LLVM capture (debug files) stays classical with
# focused proof.

let
  cloakArgs = "+include_path=${inform6lib} +language_name=english";
  cloakDirect = runCommand "corpus-cloak-direct" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${cloakArgs} ${../stories/cloak.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  cloakUpstream = runCommand "corpus-cloak-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G ${cloakArgs} \
      ${../stories/cloak.inf} "$out"
  '';
  glulxerciseDirect = runCommand "corpus-glulxercise-direct" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/glulxercise.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  debugDirect = runCommand "corpus-debug-direct" { } ''
    mkdir "$out"
    cd "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G -k '$LLVM=4' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  debugClassic = runCommand "corpus-debug-classic" { } ''
    mkdir "$out"
    cd "$out"
    ${lib.getExe inform6-llvm} -G -k '$LLVM=0' \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  tracedSource = writeText "corpus-traced.inf" ''
    [ Plain x; return x + 1; ];
    [ Traced * x; return x + 2; ];
    [ Main; @setiosys 2 0; print Plain(1), " ", Traced(2), "^"; ];
  '';
  tracedDirect = runCommand "corpus-traced-direct" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${tracedSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  tracedUpstream = runCommand "corpus-traced-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G ${tracedSource} "$out"
  '';
in
writeShellApplication {
  name = "inform6-llvm-test-corpus";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe glulxe-counted ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-corpus.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    # Full library game: coverage is asserted exactly so a routine that
    # silently stops (or starts) building directly is visible.
    if [ "$(grep -ac '^LLVM: backends direct=548 fallback=0$' \
        ${cloakDirect}/compile.log)" -ne 1 ]; then
        echo "FAIL  corpus (cloak direct coverage changed)"
        grep -a '^LLVM: backends' ${cloakDirect}/compile.log || true
        fail=1
    fi
    if grep -aq '^LLVM: direct fallbacks' ${cloakDirect}/compile.log; then
        echo "FAIL  corpus (cloak fallback stage totals changed)"
        grep -a '^LLVM: direct fallbacks' ${cloakDirect}/compile.log || true
        fail=1
    fi
    # Classic-by-policy no longer exists: any classic record here is a
    # construct silently opting out of direct IR.
    if grep -aq $'backend=classic\t' ${cloakDirect}/compile.log; then
        echo "FAIL  corpus (cloak has a classic-by-policy routine)"
        fail=1
    fi
    timeout 60 glulxe ${cloakUpstream} < ${./cloak.walk} \
        >"$work/cloak-up.log" 2>&1 || fail=1
    timeout 60 glulxe ${cloakDirect}/story.ulx < ${./cloak.walk} \
        >"$work/cloak-d.log" 2>&1 || fail=1
    if ! cmp -s <(grep -av '^Release .*Serial number' "$work/cloak-up.log") \
               <(grep -av '^Release .*Serial number' "$work/cloak-d.log"); then
        echo "FAIL  corpus (cloak upstream and direct transcripts differ)"
        fail=1
    fi
    run_counted() {
        local story=$1 input=$2 log=$3 line re
        if ! timeout 120 glulxe-counted "$story" < "$input" \
            >/dev/null 2>"$log"; then
            echo "FAIL  corpus ($(basename "$story") counted run failed)"
            fail=1
            COUNTED_RESULT=-1
            return
        fi
        re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
        line=$(grep -aE "$re" "$log" || true)
        if [[ ! $line =~ $re ]]; then
            echo "FAIL  corpus ($(basename "$story") has malformed count)"
            fail=1
            COUNTED_RESULT=-1
            return
        fi
        COUNTED_RESULT=''${BASH_REMATCH[1]}
    }
    run_counted ${cloakUpstream} ${./cloak.walk} "$work/cloak-up.count"
    cloak_up=$COUNTED_RESULT
    run_counted ${cloakDirect}/story.ulx ${./cloak.walk} "$work/cloak-d.count"
    cloak_d=$COUNTED_RESULT
    if [ "$cloak_up" -ne 164995 ] || [ "$cloak_d" -gt 256197 ]; then
        echo "FAIL  corpus (cloak dynamic bound: upstream $cloak_up, direct $cloak_d)"
        fail=1
    fi

    # Glulxercise under direct IR: every routine, including the raw
    # custom-opcode and catch/throw torture tests, builds direct IR.
    # The out-of-contract set is the documented jumpabs case plus the
    # catch-token sub-checks, which hardcode classic frame sizes (see
    # complianceTest.nix). Pin the coverage and the failure set exactly.
    if [ "$(grep -ac '^LLVM: backends direct=232 fallback=0$' \
        ${glulxerciseDirect}/compile.log)" -ne 1 ]; then
        echo "FAIL  corpus (glulxercise direct coverage changed)"
        grep -a '^LLVM: backends' ${glulxerciseDirect}/compile.log || true
        fail=1
    fi
    if grep -aq '^LLVM: direct fallbacks' ${glulxerciseDirect}/compile.log; then
        echo "FAIL  corpus (glulxercise fallback stage totals changed)"
        grep -a '^LLVM: direct fallbacks' ${glulxerciseDirect}/compile.log || true
        fail=1
    fi
    if grep -aq $'backend=classic\t' ${glulxerciseDirect}/compile.log; then
        echo "FAIL  corpus (glulxercise has a classic-by-policy routine)"
        fail=1
    fi
    timeout 60 glulxe ${glulxerciseDirect}/story.ulx < ${./glulxercise.walk} \
        >"$work/gx.log" 2>&1 || fail=1
    if ! grep -aq 'Goodbye' "$work/gx.log"; then
        echo "FAIL  corpus (glulxercise direct run crashed or hung)"
        fail=1
    else
        bad=$(grep -a 'FAIL' "$work/gx.log" \
            | grep -av 'jumpabs test=' | grep -av 'token=' || true)
        fail_count=$(grep -ac 'FAIL' "$work/gx.log" || true)
        jumpabs_count=$(grep -ac 'jumpabs test=.*FAIL' "$work/gx.log" || true)
        token_count=$(grep -ac 'token=.*FAIL' "$work/gx.log" || true)
        if [ -n "$bad" ] || [ "$fail_count" -ne 11 ] || \
           [ "$jumpabs_count" -ne 1 ] || [ "$token_count" -ne 10 ]; then
            echo "FAIL  corpus (glulxercise direct failure set changed)"
            if [ -n "$bad" ]; then
                while IFS= read -r line; do printf '      %s\n' "$line"; done <<<"$bad"
            fi
            fail=1
        fi
    fi

    # Debug-file builds bypass LLVM entirely (sequence points don't
    # survive reordering): no pipeline output, byte-identical to classic.
    if grep -aq 'LLVM' ${debugDirect}/compile.log; then
        echo "FAIL  corpus (debug build engaged the LLVM pipeline)"
        fail=1
    fi
    if ! cmp -s ${debugDirect}/story.ulx ${debugClassic}/story.ulx; then
        echo "FAIL  corpus (debug build is not byte-identical to classic)"
        fail=1
    fi

    # Asterisk-traced routines build direct IR like their neighbors (the
    # trace preamble feeds the builder); the trace output matches upstream.
    if ! grep -aq $'name=Traced\tbackend=direct\tstage=lower' \
        ${tracedDirect}/compile.log; then
        echo "FAIL  corpus (asterisked routine did not build direct IR)"
        fail=1
    fi
    if ! grep -aq $'name=Plain\tbackend=direct\tstage=lower' \
        ${tracedDirect}/compile.log; then
        echo "FAIL  corpus (routine beside an asterisked one lost direct IR)"
        fail=1
    fi
    timeout 30 glulxe ${tracedUpstream} </dev/null >"$work/tr-up.log" 2>&1 || fail=1
    timeout 30 glulxe ${tracedDirect}/story.ulx </dev/null >"$work/tr-d.log" 2>&1 || fail=1
    if ! cmp -s "$work/tr-up.log" "$work/tr-d.log"; then
        echo "FAIL  corpus (asterisk trace transcripts differ)"
        fail=1
    fi

    if [ "$fail" -eq 0 ]; then
        echo "ok    corpus"
    fi
    exit "$fail"
  '';
}
