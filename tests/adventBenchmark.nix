{ lib, runCommand, writeShellApplication, coreutils, gnugrep, python3
, glulxe-counted, glulxe-profiled, inform6-llvm, inform6-upstream, inform6-testing }:

# Full-game dynamic benchmark: the Colossal Cave (Advent) walkthrough compared
# between upstream classic codegen and direct LLVM generation. This is the
# second real-game corpus point beside cloak (see BENCH_IDEAS.md group 3) and,
# at ~7.3M instructions over a start-to-victory playthrough, a far deeper one.
# Source and library are pulled from the pinned erkyrath/Inform6-Testing input
# (Advent.inf is authored and regression-tested there against its bundled
# 6/11 library); nothing is vendored. Unlike cloak, Advent lands 3 direct-IR
# fallbacks, so this benchmark is also the standing witness that the fallback
# safety valve is load-bearing on a real game.
#
# Determinism: Advent's dwarves move on the interpreter RNG. glulxe's built-in
# "--rngseed 1" fixes the seed to the value the walkthrough was authored for,
# so counts are stable across runs and comparable across builds.

let
  adventArgs = "+include_path=${inform6-testing}/i6lib-611";
  adventSrc = "${inform6-testing}/src/Advent.inf";
  # "$!asm" traces cost nothing at runtime and give the routine-address map
  # the attribution join needs; the story files are byte-identical to
  # untraced builds.
  upstreamBuild = runCommand "advent-bench-upstream" { } ''
    mkdir "$out"
    ${lib.getExe inform6-upstream} -G '$!asm' ${adventArgs} \
      ${adventSrc} "$out/story.ulx" >"$out/asm.log" 2>&1
  '';
  directBuild = runCommand "advent-bench-direct" { } ''
    mkdir "$out"
    ${lib.getExe inform6-llvm} -G '$LLVM=1' '$!asm' ${adventArgs} \
      ${adventSrc} "$out/story.ulx" >"$out/asm.log" 2>&1
  '';
in
writeShellApplication {
  name = "inform6-llvm-benchmark-advent";
  runtimeInputs = [ coreutils gnugrep python3 glulxe-counted glulxe-profiled ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-advent-bench.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM

    count_story() {
        local story=$1 log=$2 line re
        if ! timeout 300 glulxe-counted --rngseed 1 --opcode-histogram "$story" \
                < ${./advent.walk} >/dev/null 2>"$log"; then
            echo "FAIL  advent-bench ($(basename "$story") counted run failed)"
            exit 1
        fi
        re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
        line=$(grep -aE "$re" "$log" || true)
        if [[ ! $line =~ $re ]]; then
            echo "FAIL  advent-bench ($(basename "$story") malformed count)"
            exit 1
        fi
        COUNTED_RESULT=''${BASH_REMATCH[1]}
    }

    COUNTED_RESULT=0
    count_story ${upstreamBuild}/story.ulx "$work/up.count"
    up=$COUNTED_RESULT
    count_story ${directBuild}/story.ulx "$work/direct.count"
    direct=$COUNTED_RESULT
    percent() {
        awk -v d="$(($1 - up))" -v base="$up" \
            'BEGIN { printf "%+.2f%%", 100 * d / base }'
    }

    echo "ok    advent-bench"
    echo "      dynamic: upstream $up, direct $direct ($(percent "$direct"))"

    timeout 300 glulxe-profiled --rngseed 1 --profile "$work/up.prof" \
        ${upstreamBuild}/story.ulx < ${./advent.walk} >/dev/null 2>&1 || {
        echo "FAIL  advent-bench (upstream profiled run failed)"; exit 1; }
    timeout 300 glulxe-profiled --rngseed 1 --profile "$work/direct.prof" \
        ${directBuild}/story.ulx < ${./advent.walk} >/dev/null 2>&1 || {
        echo "FAIL  advent-bench (direct profiled run failed)"; exit 1; }

    echo "      routines (direct - upstream self ops):"
    python3 ${./attrib.py} routines \
        ${upstreamBuild}/asm.log "$work/up.prof" \
        ${directBuild}/asm.log "$work/direct.prof" 10
    echo "      opcodes (direct - upstream dispatches):"
    python3 ${./attrib.py} opcodes "$work/up.count" "$work/direct.count" 10
  '';
}
