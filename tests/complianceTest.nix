{ lib, writeShellApplication, coreutils, diffutils, gnugrep, glulxe, compiledStories }:

let
  names = [
    "computation-roundtrip"
    "metaclass-region-regression"
    "veneer-compliance"
    "cloak"
  ];
  stories = map (name: {
    inherit name;
    classic = compiledStories.classic.${name};
    direct = compiledStories.direct.${name};
    input = if name == "cloak" then ./cloak.walk else "/dev/null";
  }) names;
  glulxercise = {
    classic = compiledStories.classic.glulxercise;
    direct = compiledStories.direct.glulxercise;
    input = ./glulxercise.walk;
  };
in
writeShellApplication {
  name = "inform6-llvm-test-compliance";
  runtimeInputs = [ coreutils diffutils gnugrep glulxe ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-compliance.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    fail=0

    run_story() {
        local seconds=$1 story=$2 input=$3 log=$4 status
        if timeout "$seconds" glulxe "$story" < "$input" > "$log" 2>&1; then
            return 0
        else
            status=$?
        fi
        echo "FAIL  $(basename "$story") (interpreter exited $status)"
        return 1
    }

    check() {
        local name=$1 classic=$2 direct=$3 input=$4
        local classic_log="$work/$name.classic.log"
        local direct_log="$work/$name.direct.log"
        if ! run_story 30 "$classic" "$input" "$classic_log"; then
            fail=1; return
        fi
        if ! run_story 30 "$direct" "$input" "$direct_log"; then
            fail=1; return
        fi
        if ! cmp -s <(grep -av '^Release .*Serial number' "$classic_log") \
                   <(grep -av '^Release .*Serial number' "$direct_log"); then
            echo "FAIL  $name (direct transcript differs)"
            fail=1
        else
            echo "ok    $name"
        fi
    }

    ${lib.concatMapStringsSep "\n" (story:
      "check ${story.name} ${story.classic} ${story.direct} ${story.input}") stories}

    # Glulxercise is self-checking. Its layout-dependent addresses legitimately
    # differ after optimization: computed code addresses are documented as
    # unsupported under optimization (see REVIEW.md), so accept only the
    # documented jumpabs and catch-token failures rather than comparing
    # transcripts. These counts pin the out-of-contract set exactly.
    classic_log="$work/glulxercise.classic.log"
    direct_log="$work/glulxercise.direct.log"
    if ! run_story 60 ${glulxercise.classic} ${glulxercise.input} "$classic_log"; then
        fail=1
    elif ! run_story 60 ${glulxercise.direct} ${glulxercise.input} "$direct_log"; then
        fail=1
    elif ! grep -aq 'Goodbye' "$classic_log" || ! grep -aq 'Goodbye' "$direct_log"; then
        echo "FAIL  glulxercise (a run crashed or hung)"
        fail=1
    elif grep -aq 'FAIL' "$classic_log" || \
         [ "$(grep -ac 'All tests passed' "$classic_log")" -ne 3 ]; then
        echo "FAIL  glulxercise (classic build fails its own checks)"
        fail=1
    else
        # Catch tokens encode absolute stack offsets, and glulxercise
        # hardcodes the harness chain's classic frame sizes
        # (allstackdepth = 148); direct lowering legitimately changes
        # frame layouts, so the token sub-checks join jumpabs in the
        # out-of-contract set. Every catch VALUE check must still pass.
        bad=$(grep -a 'FAIL' "$direct_log" \
            | grep -av 'jumpabs test=' | grep -av 'token=' || true)
        fail_count=$(grep -ac 'FAIL' "$direct_log" || true)
        jumpabs_count=$(grep -ac 'jumpabs test=.*FAIL' "$direct_log" || true)
        token_count=$(grep -ac 'token=.*FAIL' "$direct_log" || true)
        if [ -n "$bad" ] || [ "$fail_count" -ne 11 ] || \
           [ "$jumpabs_count" -ne 1 ] || [ "$token_count" -ne 10 ]; then
            echo "FAIL  glulxercise (direct build failure set changed)"
            if [ -n "$bad" ]; then
                while IFS= read -r line; do printf '      %s\n' "$line"; done <<<"$bad"
            fi
            fail=1
        else
            echo "ok    glulxercise"
        fi
    fi

    exit "$fail"
  '';
}
