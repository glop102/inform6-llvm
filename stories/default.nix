{ lib, runCommand, inform6-llvm, inform6-upstream, inform6lib }:

let
  storyNames = [
    "cloak"
    "computation-roundtrip"
    "faulting-read"
    "glulxercise"
    "life"
    "metaclass-region-regression"
    "optimization-regressions"
    "veneer-compliance"
  ];
  modes = {
    classic = {
      compiler = inform6-upstream;
      llvmMode = null;
    };
    forkClassic = {
      compiler = inform6-llvm;
      llvmMode = 0;
    };
    capture = {
      compiler = inform6-llvm;
      llvmMode = 1;
    };
    llvm = {
      compiler = inform6-llvm;
      llvmMode = 2;
    };
  };
  mkStory = mode: name:
    let config = modes.${mode};
    in runCommand "${name}-${mode}.ulx" { } ''
      ${lib.getExe config.compiler} -G \
        ${lib.optionalString (name == "cloak") "+include_path=${inform6lib} +language_name=english"} \
        ${lib.optionalString (config.llvmMode != null) "'$LLVM=${toString config.llvmMode}'"} \
        ${./.}/${name}.inf "$out"
    '';
in
lib.mapAttrs
  (mode: _: lib.genAttrs storyNames (mkStory mode))
  modes
