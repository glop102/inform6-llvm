{
  description = "Inform 6 compiler with an LLVM-based code generator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    # Headless Glulx interpreter for transcript-comparison tests (M3+):
    # glulxe (the reference interpreter) linked against CheapGlk (stdio Glk).
    cheapglk-src = {
      url = "github:erkyrath/cheapglk";
      flake = false;
    };
    glulxe-src = {
      url = "github:erkyrath/glulxe";
      flake = false;
    };
    # Inform 6 standard library, for tests that compile full library games.
    inform6lib-src = {
      url = "gitlab:DavidGriffith/inform6lib";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, cheapglk-src, glulxe-src, inform6lib-src }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        llvmPackages = pkgs.llvmPackages_21;

        cheapglk = pkgs.stdenv.mkDerivation {
          pname = "cheapglk";
          version = cheapglk-src.shortRev or "unstable";
          src = cheapglk-src;
          dontConfigure = true;
          installPhase = ''
            runHook preInstall
            install -Dm644 libcheapglk.a "$out/lib/libcheapglk.a"
            mkdir -p "$out/include"
            cp *.h "$out/include/"
            runHook postInstall
          '';
        };

        glulxe = pkgs.stdenv.mkDerivation {
          pname = "glulxe-cheapglk";
          version = glulxe-src.shortRev or "unstable";
          src = glulxe-src;
          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            $CC -O2 -DOS_UNIX -I${cheapglk}/include \
              main.c files.c vm.c exec.c float.c funcs.c gestalt.c heap.c \
              operand.c osdepend.c profile.c search.c serial.c string.c \
              glkop.c accel.c unixstrt.c unixautosave.c \
              -L${cheapglk}/lib -lcheapglk -lm \
              -o glulxe
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 glulxe "$out/bin/glulxe"
            runHook postInstall
          '';
        };
      in
      {
        packages = {
          inherit cheapglk glulxe;
        };

        devShells.default = pkgs.mkShell {
          packages = [
            # LLVM libraries, headers, and llvm-config
            llvmPackages.llvm.dev
            llvmPackages.llvm
            llvmPackages.clang
            llvmPackages.lldb

            # Build tools
            pkgs.gnumake
            pkgs.cmake
            pkgs.pkg-config

            # Debug / inspection helpers
            pkgs.gdb
            pkgs.valgrind

            # Glulx interpreter for running compiled story files in tests
            glulxe
          ];

          shellHook = ''
            export PS1="(dev) $PS1"
            # Inform 6 standard library for the test scripts (+include_path)
            export INFORM6_LIB=${inform6lib-src}
            echo "inform6-llvm devshell: $(llvm-config --version) at $(llvm-config --prefix)"
          '';
        };
      });
}
