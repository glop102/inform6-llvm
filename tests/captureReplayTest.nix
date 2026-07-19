{ lib, writeShellApplication, diffutils, compiledStories }:

let
  names = [ "computation-roundtrip" "veneer-compliance" "glulxercise" "cloak" ];
  stories = map (name: {
    inherit name;
    classic = compiledStories.forkClassic.${name};
    capture = compiledStories.capture.${name};
  }) names;
in
writeShellApplication {
  name = "inform6-llvm-test-capture-replay";
  runtimeInputs = [ diffutils ];
  text = ''
    fail=0

    ${lib.concatMapStringsSep "\n" (story: ''
      if cmp -s ${story.classic} ${story.capture}; then
          echo "ok    ${story.name}"
      else
          echo "FAIL  ${story.name} (story files differ)"
          fail=1
      fi
    '') stories}

    exit "$fail"
  '';
}
