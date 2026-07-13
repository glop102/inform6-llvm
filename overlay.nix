# Overlay providing the headless Glulx interpreter used by the test suite:
# glulxe (the reference interpreter) linked against CheapGlk (stdio Glk).
flakeInputs: final: prev: {
  cheapglk = final.stdenv.mkDerivation {
    pname = "cheapglk";
    version = flakeInputs.cheapglk.shortRev or "unstable";
    src = flakeInputs.cheapglk;
    dontConfigure = true;
    installPhase = ''
      runHook preInstall
      install -Dm644 libcheapglk.a "$out/lib/libcheapglk.a"
      mkdir -p "$out/include"
      cp *.h "$out/include/"
      runHook postInstall
    '';
  };

  glulxe = final.stdenv.mkDerivation {
    pname = "glulxe-cheapglk";
    version = flakeInputs.glulxe.shortRev or "unstable";
    src = flakeInputs.glulxe;
    dontConfigure = true;
    buildPhase = ''
      runHook preBuild
      $CC -O2 -DOS_UNIX -I${final.cheapglk}/include \
        main.c files.c vm.c exec.c float.c funcs.c gestalt.c heap.c \
        operand.c osdepend.c profile.c search.c serial.c string.c \
        glkop.c accel.c unixstrt.c unixautosave.c \
        -L${final.cheapglk}/lib -lcheapglk -lm \
        -o glulxe
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm755 glulxe "$out/bin/glulxe"
      runHook postInstall
    '';
  };
}
