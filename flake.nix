{
  description = "Inform 6 compiler with an LLVM-based code generator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    # Headless Glulx interpreter for transcript-comparison tests:
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
    # Classic Glulx oracle, pinned to the source revision this fork started from.
    inform6-upstream = {
      url = "github:DavidKinder/Inform6/d1066bc214a45ee0f600d2ae7f94ad0210606317";
      flake = false;
    };
  };

  outputs = inputs@{ self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      (pkgs: {
        packages = {
          inherit (pkgs) cheapglk glulxe;
          inherit (pkgs) glulxe-counted glulxe-profiled inform6-llvm inform6-upstream;
          default = pkgs.inform6-llvm;
        };

        legacyPackages = {
          inherit (pkgs) compiledStories testApps benchmarkApps;
        };

        apps = {
          tests = {
            type = "app";
            program = pkgs.lib.getExe (pkgs.writeShellApplication {
              name = "inform6-llvm-tests";
              text = ''
                ${pkgs.lib.getExe pkgs.testApps.optimization}
                ${pkgs.lib.getExe pkgs.testApps.compliance}
                ${pkgs.lib.getExe pkgs.testApps.zMachine}
                ${pkgs.lib.getExe pkgs.testApps.directIr}
                ${pkgs.lib.getExe pkgs.testApps.corpus}
              '';
            });
          };

          benchmarks = {
            type = "app";
            program = pkgs.lib.getExe (pkgs.writeShellApplication {
              name = "inform6-llvm-benchmarks";
              text = ''
                ${pkgs.lib.getExe pkgs.benchmarkApps.life}
                ${pkgs.lib.getExe pkgs.benchmarkApps.cloak}
              '';
            });
          };
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
            pkgs.glulxe-counted
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
