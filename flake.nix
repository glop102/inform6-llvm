{
  description = "Inform 6 compiler with an LLVM-based code generator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        llvmPackages = pkgs.llvmPackages_21;
      in
      {
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
          ];

          shellHook = ''
            export PS1="(dev) $PS1" 
            echo "inform6-llvm devshell: $(llvm-config --version) at $(llvm-config --prefix)"
          '';
        };
      });
}
