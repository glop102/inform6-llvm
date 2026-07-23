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
  # Regression canary for deferred-lowering address timing. Three
  # historically silent miscompiles are exercised at once:
  # - "Switches g" runs after Main__ is compiled and stashed; the latched
  #   deferral decision must survive it (traced routines defer as
  #   classic-captured code rather than emitting eagerly at the code-area
  #   start, where the Glulx header's start-function field points).
  # - An asterisk-traced routine likewise must not displace Main__.
  # - "Replace X Y" must resolve Y to X's *first* definition even though
  #   routine addresses are assigned at end of pass.
  deferredTimingSource = writeText "direct-ir-deferred-timing.inf" ''
    Switches g;
    Replace Orig NewOrig;
    [ Orig; return 100; ];
    [ Traced * x; return x + 5; ];
    [ Orig; return NewOrig() + 1; ];
    [ Main win dummy;
        @setiosys 2 0;
        @copy 0 sp;
        @copy 3 sp;
        @copy 0 sp;
        @copy 0 sp;
        @copy 0 sp;
        @glk $0023 5 win;
        @copy win sp;
        @glk $002F 1 dummy;
        print "orig=", Orig(), " traced=", Traced(2), "^";
    ];
  '';
  # Regression canary for AUTOGEN embedded-routine symbol collisions.
  # The user object is deliberately named nameless_obj__6: with the four
  # metaclass prototypes it is object #5, so the nameless object that
  # follows is #6 and auto-names to exactly the same identifier. Both
  # embedded 'foo' routines then mangle to "nameless_obj__6.foo"; without
  # the uniquify guard the second assign_symbol silently aliases both
  # property values to one routine ("ALIASED"). Compared behaviorally
  # against upstream, which must print "distinct".
  autogenCollisionSource = writeText "direct-ir-autogen-collision.inf" ''
    Property foo;
    Object nameless_obj__6 "decoy"
      with foo [; print "decoy foo^"; ];
    Object "unnamed"
      with foo [; print "unnamed foo^"; ];
    [ Main o a b win dummy;
        @setiosys 2 0;
        @copy 0 sp;
        @copy 3 sp;
        @copy 0 sp;
        @copy 0 sp;
        @copy 0 sp;
        @glk $0023 5 win;
        @copy win sp;
        @glk $002F 1 dummy;
        objectloop (o provides foo) {
            print (name) o, ": "; o.foo();
            if (a == 0) a = o.foo; else b = o.foo;
        }
        if (a == b) print "ALIASED^"; else print "distinct^";
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
  defaultMode = runCommand "direct-ir-default-mode" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G \
      ${../stories/direct-ir-phase1.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  upstream = runCommand "direct-ir-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G \
      ${../stories/direct-ir-phase1.inf} "$out"
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
  deferredTimingDirect = runCommand "direct-ir-deferred-timing" { } ''
    mkdir "$out"
    ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${deferredTimingSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  deferredTimingUpstream = runCommand "direct-ir-deferred-timing-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G ${deferredTimingSource} "$out"
  '';
  autogenCollisionDirect = runCommand "direct-ir-autogen-collision" { } ''
    mkdir "$out"
    ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${autogenCollisionSource} "$out/story.ulx" >"$out/compile.log" 2>&1
  '';
  autogenCollisionUpstream = runCommand "direct-ir-autogen-collision-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G ${autogenCollisionSource} "$out"
  '';
  memoryStrictDirect = runCommand "direct-ir-memory-strict" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-memory.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  memoryStrictUpstream = runCommand "direct-ir-memory-strict-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -G \
      ${../stories/direct-ir-memory.inf} "$out"
  '';
  memoryLooseDirect = runCommand "direct-ir-memory-loose" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -~S -G '$LLVM=4' \
      ${../stories/direct-ir-memory.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  memoryLooseUpstream = runCommand "direct-ir-memory-loose-upstream.ulx" { } ''
    ${lib.getExe inform6-upstream} -~S -G \
      ${../stories/direct-ir-memory.inf} "$out"
  '';
  # Parser bookkeeping is separate from shadow-event storage: with
  # retention disabled (I6_LLVM_SHADOW=0), bookkeeping alone must carry
  # parsing, and a story with zero fallbacks must come out byte-identical.
  noShadow = runCommand "direct-ir-no-shadow" { } ''
    mkdir "$out"
    I6_LLVM_SHADOW=0 I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-memory.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  # A story whose routines genuinely fall back cannot be emitted without
  # the retained shadow stream: the compile must fail cleanly, with the
  # retention error and no output file, never a silent empty routine.
  # Raw code bytes are the one construct that always rejects direct IR
  # (they have no instruction-level meaning to translate).
  noShadowFallbackSource = writeText "direct-ir-no-shadow-fallback.inf" ''
    [ RawBytes;
        @ -> 0;    ! a nop, emitted as a raw code byte
        return 7;
    ];
    [ Main; @setiosys 2 0; print RawBytes(), "^"; ];
  '';
  noShadowFallback = runCommand "direct-ir-no-shadow-fallback.compile.log" { } ''
    work=$(mktemp -d)
    set +e
    I6_LLVM_SHADOW=0 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${noShadowFallbackSource} "$work/story.ulx" >"$out" 2>&1
    status=$?
    set -e
    test "$status" -ne 0
    test ! -e "$work/story.ulx"
  '';
  # Former fixed limits of the direct path, now dynamic: the fixture
  # exceeds every old cap (symbolic-stack depth, call arity, switch
  # nesting, lowered frame slots) and must still build all-direct and
  # match the classic backend's transcript. See the story's header.
  limitsDirect = runCommand "direct-ir-limits" { } ''
    mkdir "$out"
    I6_LLVM_DIAGNOSTICS=1 ${lib.getExe inform6-llvm} -G '$LLVM=4' \
      ${../stories/direct-ir-limits.inf} "$out/story.ulx" \
      >"$out/compile.log" 2>&1
  '';
  limitsClassic = runCommand "direct-ir-limits-classic.ulx" { } ''
    ${lib.getExe inform6-llvm} -G '$LLVM=0' \
      ${../stories/direct-ir-limits.inf} "$out"
  '';
in
writeShellApplication {
  name = "inform6-llvm-test-direct-ir";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe glulxe-counted ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-direct.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    direct_re=$'^LLVM-BACKEND\tname=Direct_(Constant|Parameter|Assignment|Branch|Arithmetic|Bitwise|GlobalRead|GlobalWrite|Chain|AssignmentValue|Symbol|Divide|Remainder|NonnegativeDivide|Divisor|Modulus|Compare|CompareAssignment|CompareOr|CompareOrOrder|CompareOrPredicates|Logical|If|IfElse|ShortCircuit|LogicalBranches|LogicalNegation|While|Do|DoReturn|For|ForInc|Switch|SwitchLoop|NestedSwitch|Infinite|Unreachable|UnreachableLoop|SharedReturn|PreInc|PostInc|PreDec|PostDec|GlobalInc|EvalOrder|Comma|Wrap|Inline|CalleePair|CalleeSum|Note|Call|CallNested|CallOrder|CallWide|CallWideMixed|CallCondition|CallIndirect|CallVoid|Random|Catch|StackAsm|CustomAsm)\tbackend=direct\tstage=lower\tinput=[0-9]+\temitted=[0-9]+\treason=-$'
    if [ "$(grep -acE "$direct_re" ${direct}/compile.log)" -ne 63 ]; then
        echo "FAIL  direct-ir (supported routines did not all use direct IR)"
        fail=1
    fi
    if grep -aq '^LLVM-BACKEND' ${quietDirect}/compile.log; then
        echo "FAIL  direct-ir (quiet direct mode emitted per-routine diagnostics)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=74 fallback=0$' \
        ${direct}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (aggregate backend totals are incorrect)"
        fail=1
    fi
    if grep -aq '^LLVM: direct fallbacks' ${direct}/compile.log; then
        echo "FAIL  direct-ir (fallback stage totals appeared on a zero-fallback story)"
        fail=1
    fi

    # Phase 5: a plain compile (no $LLVM setting) selects the direct
    # backend and produces the same bytes as an explicit $LLVM=4 compile.
    if [ "$(grep -ac '^LLVM: backends direct=74 fallback=0$' \
        ${defaultMode}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (default mode did not select the direct backend)"
        fail=1
    fi
    if ! cmp -s ${defaultMode}/story.ulx ${direct}/story.ulx; then
        echo "FAIL  direct-ir (default mode output differs from explicit direct mode)"
        fail=1
    fi

    # Phase 6: parser bookkeeping is independent of shadow-event storage.
    if ! cmp -s ${noShadow}/story.ulx ${memoryStrictDirect}/story.ulx; then
        echo "FAIL  direct-ir (no-shadow output differs from shadowed direct build)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=70 fallback=0$' \
        ${noShadow}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (no-shadow backend totals are incorrect)"
        fail=1
    fi
    if ! grep -aq 'shadow retention disabled (I6_LLVM_SHADOW=0)' \
        ${noShadowFallback}; then
        echo "FAIL  direct-ir (no-shadow fallback did not report the retention error)"
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
    check_routine Direct_CompareOr 7 5
    check_routine Direct_CompareOrOrder 16 1
    check_routine Direct_CompareOrPredicates 39 24
    check_routine Direct_Logical 6 10
    check_routine Direct_If 3 4
    check_routine Direct_IfElse 5 5
    check_routine Direct_ShortCircuit 10 4
    check_routine Direct_LogicalBranches 10 4
    check_routine Direct_LogicalNegation 8 4
    check_routine Direct_While 9 20
    check_routine Direct_Do 3 7
    check_routine Direct_DoReturn 1 1
    check_routine Direct_For 7 9
    check_routine Direct_ForInc 8 12
    check_routine Direct_Switch 17 29
    check_routine Direct_SwitchLoop 11 21
    check_routine Direct_NestedSwitch 7 7
    check_routine Direct_Infinite 1 1
    check_routine Direct_Unreachable 1 1
    check_routine Direct_UnreachableLoop 1 1
    check_routine Direct_SharedReturn 4 4
    check_routine Direct_GlobalInc 6 2
    check_routine Direct_Inline 2 1
    check_routine Direct_CalleePair 3 3
    check_routine Direct_CalleeSum 9 9
    check_routine Direct_Note 3 5
    check_routine Direct_Call 2 2
    check_routine Direct_CallNested 3 3
    check_routine Direct_CallOrder 7 8
    check_routine Direct_CallWide 8 13
    check_routine Direct_CallWideMixed 7 9
    check_routine Direct_CallCondition 4 5
    check_routine Direct_CallIndirect 2 2
    check_routine Direct_CallVoid 3 4
    if ! grep -aq $'name=Main\tbackend=direct\tstage=lower' \
        ${direct}/compile.log; then
        echo "FAIL  direct-ir (Main with inline assembly did not use direct IR)"
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
    if [ "$upstream_count" -ne 756 ] || [ "$direct_count" -gt 855 ]; then
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

    set +e
    timeout 30 glulxe ${deferredTimingUpstream} </dev/null \
        >"$work/deferred-timing-upstream.log" 2>&1
    deferred_upstream_status=$?
    timeout 30 glulxe ${deferredTimingDirect}/story.ulx </dev/null \
        >"$work/deferred-timing-direct.log" 2>&1
    deferred_direct_status=$?
    set -e
    if [ "$deferred_upstream_status" -eq 124 ] || \
       [ "$deferred_direct_status" -eq 124 ] || \
       [ "$deferred_upstream_status" -ne "$deferred_direct_status" ] || \
       ! cmp -s "$work/deferred-timing-upstream.log" \
                "$work/deferred-timing-direct.log"; then
        echo "FAIL  direct-ir (deferred address-timing behavior differs)"
        fail=1
    fi

    set +e
    timeout 30 glulxe ${autogenCollisionUpstream} </dev/null \
        >"$work/autogen-collision-upstream.log" 2>&1
    collision_upstream_status=$?
    timeout 30 glulxe ${autogenCollisionDirect}/story.ulx </dev/null \
        >"$work/autogen-collision-direct.log" 2>&1
    collision_direct_status=$?
    set -e
    if [ "$collision_upstream_status" -eq 124 ] || \
       [ "$collision_direct_status" -eq 124 ] || \
       [ "$collision_upstream_status" -ne "$collision_direct_status" ] || \
       ! cmp -s "$work/autogen-collision-upstream.log" \
                "$work/autogen-collision-direct.log"; then
        echo "FAIL  direct-ir (AUTOGEN symbol-collision behavior differs)"
        fail=1
    fi
    if ! grep -aq '^distinct$' "$work/autogen-collision-direct.log"; then
        echo "FAIL  direct-ir (AUTOGEN collision fixture did not print distinct)"
        fail=1
    fi

    mem_re=$'^LLVM-BACKEND\tname=Mem_(Note|WordRead|WordWrite|ByteRW|ArrInc|Order|Computed|Has|Hasnt|In|Notin|Ofclass|Provides|PropRead|PropWrite|PropInc|PropAddr|PropLen|PropCall|Parent|Child|Sibling|Random|RandomVar|Print|PrintChar|PrintOrder|PrintRet|PrintObj|Style|GiveMove|Quit|Shifts|ShiftVar)\tbackend=direct\tstage=lower\tinput=[0-9]+\temitted=[0-9]+\treason=-$'
    if [ "$(grep -acE "$mem_re" ${memoryStrictDirect}/compile.log)" -ne 34 ]; then
        echo "FAIL  direct-ir (strict memory routines did not all use direct IR)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=70 fallback=0$' \
        ${memoryStrictDirect}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (strict memory backend totals are incorrect)"
        fail=1
    fi
    if grep -aq '^LLVM: direct fallbacks' ${memoryStrictDirect}/compile.log; then
        echo "FAIL  direct-ir (strict memory fallback stage totals are incorrect)"
        fail=1
    fi
    # Classic-by-policy no longer exists: every routine builds direct IR.
    if grep -aq $'backend=classic\t' ${memoryStrictDirect}/compile.log; then
        echo "FAIL  direct-ir (strict memory has a classic-by-policy routine)"
        fail=1
    fi
    if [ "$(grep -acE "$mem_re" ${memoryLooseDirect}/compile.log)" -ne 34 ]; then
        echo "FAIL  direct-ir (unchecked memory routines did not all use direct IR)"
        fail=1
    fi
    if [ "$(grep -ac '^LLVM: backends direct=60 fallback=0$' \
        ${memoryLooseDirect}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (unchecked memory backend totals are incorrect)"
        fail=1
    fi
    if grep -aq '^LLVM: direct fallbacks' ${memoryLooseDirect}/compile.log; then
        echo "FAIL  direct-ir (unchecked memory fallback stage totals are incorrect)"
        fail=1
    fi
    timeout 30 glulxe ${memoryStrictUpstream} </dev/null \
        >"$work/mem-strict-upstream.log" 2>&1 || fail=1
    timeout 30 glulxe ${memoryStrictDirect}/story.ulx </dev/null \
        >"$work/mem-strict-direct.log" 2>&1 || fail=1
    if ! cmp -s "$work/mem-strict-upstream.log" "$work/mem-strict-direct.log"; then
        echo "FAIL  direct-ir (strict memory transcripts differ)"
        fail=1
    fi
    if ! grep -aq 'Programming error' "$work/mem-strict-direct.log"; then
        echo "FAIL  direct-ir (strict memory fixture did not hit its bounds error)"
        fail=1
    fi
    timeout 30 glulxe ${memoryLooseUpstream} </dev/null \
        >"$work/mem-loose-upstream.log" 2>&1 || fail=1
    timeout 30 glulxe ${memoryLooseDirect}/story.ulx </dev/null \
        >"$work/mem-loose-direct.log" 2>&1 || fail=1
    if ! cmp -s "$work/mem-loose-upstream.log" "$work/mem-loose-direct.log"; then
        echo "FAIL  direct-ir (unchecked memory transcripts differ)"
        fail=1
    fi
    run_counted ${memoryStrictUpstream} "$work/mem-strict-upstream.count"
    mem_strict_upstream=$COUNTED_RESULT
    run_counted ${memoryStrictDirect}/story.ulx "$work/mem-strict-direct.count"
    mem_strict_direct=$COUNTED_RESULT
    if [ "$mem_strict_upstream" -ne 1519 ] || [ "$mem_strict_direct" -gt 2231 ]; then
        echo "FAIL  direct-ir (strict memory dynamic bound: upstream $mem_strict_upstream, direct $mem_strict_direct)"
        fail=1
    fi
    run_counted ${memoryLooseUpstream} "$work/mem-loose-upstream.count"
    mem_loose_upstream=$COUNTED_RESULT
    run_counted ${memoryLooseDirect}/story.ulx "$work/mem-loose-direct.count"
    mem_loose_direct=$COUNTED_RESULT
    if [ "$mem_loose_upstream" -ne 938 ] || [ "$mem_loose_direct" -gt 1265 ]; then
        echo "FAIL  direct-ir (unchecked memory dynamic bound: upstream $mem_loose_upstream, direct $mem_loose_direct)"
        fail=1
    fi

    # Former fixed limits are dynamic: every old cap exceeded, zero
    # fallbacks, classic-identical behavior, and the >250-slot routine
    # carries a multi-pair locals format (C1 04 FF 04 ...).
    if [ "$(grep -ac '^LLVM: backends direct=9 fallback=0$' \
        ${limitsDirect}/compile.log)" -ne 1 ]; then
        echo "FAIL  direct-ir (limits story backend totals are incorrect)"
        fail=1
    fi
    timeout 30 glulxe ${limitsDirect}/story.ulx </dev/null \
        >"$work/limits-direct.log" 2>&1 || fail=1
    timeout 30 glulxe ${limitsClassic} </dev/null \
        >"$work/limits-classic.log" 2>&1 || fail=1
    if ! cmp -s "$work/limits-direct.log" "$work/limits-classic.log"; then
        echo "FAIL  direct-ir (limits story transcripts differ)"
        fail=1
    fi
    if ! grep -aq '^slotpressure=9090200$' "$work/limits-direct.log"; then
        echo "FAIL  direct-ir (limits story printed wrong results)"
        fail=1
    fi
    if ! od -An -v -tx1 ${limitsDirect}/story.ulx | tr -d ' \n' \
        | grep -q c104ff04; then
        echo "FAIL  direct-ir (limits story lacks a multi-pair locals frame)"
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
