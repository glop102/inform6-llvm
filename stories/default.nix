{ lib, runCommand, inform6-llvm, inform6lib }:

let
  compilerExe = lib.getExe inform6-llvm;
  storyNames = [
    "cloak"
    "computation-roundtrip"
    "glulxercise"
    "life"
    "metaclass-region-regression"
    "optimization-regressions"
    "veneer-compliance"
  ];
  modeValues = {
    classic = 0;
    capture = 1;
    llvm = 2;
  };
  mkStory = mode: name:
    runCommand "${name}-${mode}.ulx" { } ''
      ${compilerExe} -G \
        ${lib.optionalString (name == "cloak") "+include_path=${inform6lib} +language_name=english"} \
        '$LLVM=${toString modeValues.${mode}}' \
        ${./.}/${name}.inf "$out"
    '';
in
lib.mapAttrs
  (mode: _: lib.genAttrs storyNames (mkStory mode))
  modeValues
