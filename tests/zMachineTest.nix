{ lib, runCommand, writeShellApplication, coreutils, diffutils
, libxml2
, inform6-llvm, inform6-upstream }:

let
  upstream = runCommand "z-machine-baseline-upstream.z5" { } ''
    ${lib.getExe inform6-upstream} \
      ${../stories/z-machine-baseline.inf} "$out"
  '';
  fork = runCommand "z-machine-baseline-fork.z5" { } ''
    ${lib.getExe inform6-llvm} '$LLVM=4' \
      ${../stories/z-machine-baseline.inf} "$out"
  '';
  mkDebug = name: compiler: flags: runCommand name { } ''
    cp ${../stories/z-machine-baseline.inf} fixture.inf
    ${lib.getExe compiler} ${flags} -k \
      fixture.inf story >compile.log
    cp gameinfo.dbg "$out"
  '';
  zDebugUpstream = mkDebug "z-machine-baseline-upstream.dbg"
    inform6-upstream "";
  zDebugFork = mkDebug "z-machine-baseline-fork.dbg"
    inform6-llvm "'$LLVM=4'";
  glulxDebugUpstream = mkDebug "glulx-baseline-upstream.dbg"
    inform6-upstream "-G";
  glulxDebugFork = mkDebug "glulx-baseline-fork.dbg"
    inform6-llvm "-G '$LLVM=4'";
in
writeShellApplication {
  name = "inform6-llvm-test-z-machine";
  runtimeInputs = [ coreutils diffutils libxml2 ];
  text = ''
    work=$(mktemp -d "''${TMPDIR:-/tmp}/inform6-debug.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM

    extract_sequence_points() {
        local debug_file=$1 output=$2 count i xpath value
        count=$(xmllint --xpath 'count(//sequence-point)' "$debug_file")
        : >"$output"
        for ((i=1; i<=count; i++)); do
            xpath="concat(normalize-space(string((//sequence-point)[$i]/ancestor::routine[1]/identifier)), '|', normalize-space(string((//sequence-point)[$i]/address)), '|', string((//sequence-point)[$i]/source-code-location/file-index), '|', string((//sequence-point)[$i]/source-code-location/file-position), '|', string((//sequence-point)[$i]/source-code-location/line), '|', string((//sequence-point)[$i]/source-code-location/character))"
            value=$(xmllint --xpath "$xpath" "$debug_file")
            printf '%s\n' "$value" >>"$output"
        done
    }

    # Compare fork sequence points against a pinned-upstream compile of the
    # same fixture instead of checked-in golden values, so an upstream bump
    # or fixture edit re-derives both sides. The extraction must be
    # non-empty, or a debug-format change could blank both sides and pass.
    check_sequence_points() {
        local label=$1 upstream_dbg=$2 fork_dbg=$3
        extract_sequence_points "$upstream_dbg" "$work/$label-upstream.tsv"
        extract_sequence_points "$fork_dbg" "$work/$label-fork.tsv"
        if ! [ -s "$work/$label-upstream.tsv" ]; then
            echo "FAIL  $label (no sequence points extracted from upstream debug file)"
            return 1
        fi
        if ! diff "$work/$label-upstream.tsv" "$work/$label-fork.tsv"; then
            echo "FAIL  $label (debug sequence-point locations differ from upstream)"
            return 1
        fi
    }

    if ! cmp -s ${upstream} ${fork}; then
        echo "FAIL  z-machine (upstream and fork story files differ)"
        exit 1
    fi
    check_sequence_points z-machine ${zDebugUpstream} ${zDebugFork}
    check_sequence_points glulx ${glulxDebugUpstream} ${glulxDebugFork}
    echo "ok    z-machine"
  '';
}
