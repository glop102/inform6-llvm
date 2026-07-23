{ lib, runCommand, writeShellApplication, coreutils, gnugrep, python3
, glulxe-counted, glulxe-profiled, inform6-llvm, inform6-upstream, inform6lib }:

# Full-library dynamic benchmark: cloak's scripted walkthrough compared
# between upstream classic codegen and direct LLVM generation. Unlike the corpus
# test's pass/fail ceilings, this surfaces the attribution behind the
# totals by default — per-routine self-op deltas and the opcode-mix diff —
# so a regression names its hot routines and shapes instead of hiding
# behind an aggregate. Phase 4.1 was misdirected by static rankings until
# exactly this data was collected by hand; keep it visible.

let
  cloakArgs = "+include_path=${inform6lib} +language_name=english";
  # "$!asm" traces cost nothing at runtime and give the routine-address
  # map the attribution join needs; the story files are byte-identical
  # to untraced builds.
  upstreamBuild = runCommand "cloak-bench-upstream" { } ''
    mkdir "$out"
    ${lib.getExe inform6-upstream} -G '$!asm' ${cloakArgs} \
      ${../stories/cloak.inf} "$out/story.ulx" >"$out/asm.log" 2>&1
  '';
  directBuild = runCommand "cloak-bench-direct" { } ''
    mkdir "$out"
    ${lib.getExe inform6-llvm} -G '$LLVM=1' '$!asm' ${cloakArgs} \
      ${../stories/cloak.inf} "$out/story.ulx" >"$out/asm.log" 2>&1
  '';
in
writeShellApplication {
  name = "inform6-llvm-benchmark-cloak";
  runtimeInputs = [ coreutils gnugrep python3 glulxe-counted glulxe-profiled ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-cloak-bench.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM

    count_story() {
        local story=$1 log=$2 line re
        if ! timeout 120 glulxe-counted --opcode-histogram "$story" \
                < ${./cloak.walk} >/dev/null 2>"$log"; then
            echo "FAIL  cloak-bench ($(basename "$story") counted run failed)"
            exit 1
        fi
        re='^GLULXE_INSTRUCTION_COUNT=([0-9]+)$'
        line=$(grep -aE "$re" "$log" || true)
        if [[ ! $line =~ $re ]]; then
            echo "FAIL  cloak-bench ($(basename "$story") malformed count)"
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

    echo "ok    cloak-bench"
    echo "      dynamic: upstream $up, direct $direct ($(percent "$direct"))"

    timeout 120 glulxe-profiled --profile "$work/up.prof" \
        ${upstreamBuild}/story.ulx < ${./cloak.walk} >/dev/null 2>&1 || {
        echo "FAIL  cloak-bench (upstream profiled run failed)"; exit 1; }
    timeout 120 glulxe-profiled --profile "$work/direct.prof" \
        ${directBuild}/story.ulx < ${./cloak.walk} >/dev/null 2>&1 || {
        echo "FAIL  cloak-bench (direct profiled run failed)"; exit 1; }

    echo "      routines (direct - upstream self ops):"
    python3 ${./attrib.py} routines \
        ${upstreamBuild}/asm.log "$work/up.prof" \
        ${directBuild}/asm.log "$work/direct.prof" 10
    echo "      opcodes (direct - upstream dispatches):"
    python3 ${./attrib.py} opcodes "$work/up.count" "$work/direct.count" 10
  '';
}
