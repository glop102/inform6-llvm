# Project packages, compiled stories, and their test and benchmark consumers.
flakeInputs: final: prev:
let
  testApps = {
    compliance = final.callPackage ./tests/complianceTest.nix { };
    corpus = final.callPackage ./tests/corpusTest.nix {
      inform6lib = flakeInputs.inform6lib;
    };
    directIr = final.callPackage ./tests/directIrTest.nix { };
    optimization = final.callPackage ./tests/optimizationTest.nix { };
    zMachine = final.callPackage ./tests/zMachineTest.nix { };
  };
  benchmarkApps = {
    life = final.callPackage ./tests/lifeBenchmark.nix { };
    cloak = final.callPackage ./tests/cloakBenchmark.nix {
      inform6lib = flakeInputs.inform6lib;
    };
    advent = final.callPackage ./tests/adventBenchmark.nix {
      inform6-testing = flakeInputs.inform6-testing;
    };
  };
in {
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

  glulxe-counted = final.glulxe.overrideAttrs (old: {
    pname = "glulxe-counted";
    patches = (old.patches or [ ]) ++ [
      ./patches/glulxe-instruction-count.patch
    ];
    postInstall = (old.postInstall or "") + ''
      mv "$out/bin/glulxe" "$out/bin/glulxe-counted"
    '';
  });

  # Upstream glulxe's own profiler: per-function call and self-op counts
  # dumped as XML via "--profile <file>". Benchmarks join it against an
  # inform6 "$!asm" trace for per-routine dynamic attribution.
  glulxe-profiled = final.glulxe.overrideAttrs (old: {
    pname = "glulxe-profiled";
    env = (old.env or { }) // { NIX_CFLAGS_COMPILE = "-DVM_PROFILING=1"; };
    postInstall = (old.postInstall or "") + ''
      mv "$out/bin/glulxe" "$out/bin/glulxe-profiled"
    '';
  });

  inform6-upstream = final.stdenv.mkDerivation {
    pname = "inform6-upstream";
    version = "6.45-${flakeInputs.inform6-upstream.shortRev or "d1066bc"}";
    src = flakeInputs.inform6-upstream;
    nativeBuildInputs = [ final.gnumake ];
    # Fortified glibc realpath() aborts whenever its destination buffer is
    # smaller than PATH_MAX; upstream's debug-file writer (-k) uses a
    # PATHLEN=512 buffer, so every -k compile dies under fortify. Keep the
    # oracle source unpatched and drop the hardening flag instead.
    # _DEFAULT_SOURCE keeps realpath() declared under the makefile's strict
    # --std=c11 once the fortify wrapper header no longer declares it.
    hardeningDisable = [ "fortify" ];
    env.NIX_CFLAGS_COMPILE = "-D_DEFAULT_SOURCE";
    installPhase = ''
      runHook preInstall
      install -Dm755 inform6 "$out/bin/inform6"
      runHook postInstall
    '';
    meta.mainProgram = "inform6";
  };

  inform6-llvm = final.stdenv.mkDerivation {
    pname = "inform6-llvm";
    version = "unstable";
    src = final.lib.fileset.toSource {
      root = ./.;
      fileset = final.lib.fileset.unions [
        ./Makefile
        ./src
      ];
    };
    nativeBuildInputs = [
      final.gnumake
      final.llvmPackages_21.clang
      final.llvmPackages_21.llvm.dev
    ];
    buildInputs = [ final.llvmPackages_21.llvm ];
    makeFlags = [
      "CC=${final.llvmPackages_21.clang}/bin/clang"
      "LLVM_CONFIG=${final.llvmPackages_21.llvm.dev}/bin/llvm-config"
      "WITH_LLVM=1"
    ];
    installPhase = ''
      runHook preInstall
      install -Dm755 inform6-llvm "$out/bin/inform6-llvm"
      runHook postInstall
    '';
    meta.mainProgram = "inform6-llvm";
  };

  compiledStories = final.callPackage ./stories {
    inform6lib = flakeInputs.inform6lib;
  };

  inherit testApps benchmarkApps;

}
