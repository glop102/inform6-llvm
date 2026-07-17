{
  description = "Inform 6 compiler with an LLVM-based code generator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    # Headless Glulx interpreter for transcript-comparison tests (M3+):
    # glulxe (the reference interpreter) linked against CheapGlk (stdio Glk).
    cheapglk = {
      url = "github:erkyrath/cheapglk";
      flake = false;
    };
    glulxe = {
      url = "github:erkyrath/glulxe";
      flake = false;
    };
    # Inform 6 standard library, for tests that compile full library games.
    inform6lib = {
      url = "gitlab:DavidGriffith/inform6lib";
      flake = false;
    };
  };

  outputs = inputs@{ self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      (pkgs:
        let
          glulxe-counted = pkgs.glulxe.overrideAttrs (old: {
            pname = "glulxe-counted";
            patches = (old.patches or [ ]) ++ [
              ./patches/glulxe-instruction-count.patch
            ];
            postInstall = (old.postInstall or "") + ''
              mv "$out/bin/glulxe" "$out/bin/glulxe-counted"
            '';
          });
        in {
        packages = {
          inherit (pkgs) cheapglk glulxe;
          inherit glulxe-counted;
        };

        devShells.default = pkgs.mkShell {
          packages = [
            # LLVM libraries, headers, and llvm-config
            pkgs.llvmPackages_21.llvm.dev
            pkgs.llvmPackages_21.llvm
            pkgs.llvmPackages_21.clang
            pkgs.llvmPackages_21.lldb

            # Build tools
            pkgs.gnumake
            pkgs.cmake
            pkgs.pkg-config

            # Debug / inspection helpers
            pkgs.gdb
            pkgs.valgrind

            # Glulx interpreter for running compiled story files in tests
            pkgs.glulxe
            glulxe-counted
          ];

          shellHook = ''
            export PS1="(dev) $PS1"
            # Inform 6 standard library for the test scripts (+include_path)
            export INFORM6_LIB=${inputs.inform6lib}
            echo "inform6-llvm devshell: $(llvm-config --version) at $(llvm-config --prefix)"
            echo "inform6lib: ${inputs.inform6lib}"
          '';
        };
        }) (nixpkgs.legacyPackages.${system}.extend (import ./overlay.nix inputs)));
}
