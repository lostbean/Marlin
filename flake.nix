{
  description = "Marlin firmware dev environment (PlatformIO build for Two Trees BlueR / MKS Robin Nano)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    flake-utils.url = "github:numtide/flake-utils";

    treefmt-nix.url = "github:numtide/treefmt-nix";
    treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      treefmt-nix,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # treefmt config.
        #
        # IMPORTANT (this is a fork of upstream Marlin): we deliberately format
        # ONLY Nix files. The Marlin C/C++/Python tree is left untouched so we
        # never reflow upstream code and create merge conflicts. The repo's own
        # `.editorconfig` already encodes the C++ (2-space) / Python (4-space)
        # house style for editors — that is the "format only what I touch" path
        # for firmware code. Do NOT enable clang-format / ruff / prettier here.
        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";

          programs.nixfmt.enable = true; # Nix (RFC-style). Our files only.

          # Belt-and-suspenders: even though only nixfmt is on, scope it to the
          # repo's own Nix files and explicitly keep the firmware tree out.
          settings.global.excludes = [
            "Marlin/**"
            "buildroot/**"
            "ini/**"
            "config/**"
            "docs/**"
            "docker/**"
            "*.h"
            "*.cpp"
            "*.c"
            "*.ino"
            "*.py"
          ];
        };
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            platformio # `pio run` — build/upload firmware
            python3 # buildroot helper scripts
            lefthook # git hook runner (format gate)
          ];

          # PlatformIO wants a writable core dir and downloads its own
          # toolchains/frameworks at build time. Keep its cache inside the repo
          # (gitignored) so builds are self-contained and don't touch $HOME.
          shellHook = ''
            export PLATFORMIO_CORE_DIR="$PWD/.pio-core"
            echo "Marlin dev shell — PlatformIO $(pio --version 2>/dev/null || echo '(run: pio --version)')"
            echo "  build:  pio run                 (default env: mks_robin_nano_v1v2)"
            echo "  upload: pio run -t upload"
            echo "  clean:  pio run -t clean"
          '';
        };

        # `nix fmt` runs treefmt — Nix files only (see note above).
        formatter = treefmtEval.config.build.wrapper;

        # `nix flake check` verifies the Nix files are formatted.
        checks.formatting = treefmtEval.config.build.check ./.;
      }
    );
}
