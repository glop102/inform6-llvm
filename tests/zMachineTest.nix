{ lib, runCommand, writeShellApplication, diffutils
, inform6-llvm, inform6-upstream }:

let
  upstream = runCommand "z-machine-baseline-upstream.z5" { } ''
    ${lib.getExe inform6-upstream} \
      ${../stories/z-machine-baseline.inf} "$out"
  '';
  fork = runCommand "z-machine-baseline-fork.z5" { } ''
    ${lib.getExe inform6-llvm} '$LLVM=2' \
      ${../stories/z-machine-baseline.inf} "$out"
  '';
in
writeShellApplication {
  name = "inform6-llvm-test-z-machine";
  runtimeInputs = [ diffutils ];
  text = ''
    if cmp -s ${upstream} ${fork}; then
        echo "ok    z-machine"
    else
        echo "FAIL  z-machine (upstream and fork story files differ)"
        exit 1
    fi
  '';
}
