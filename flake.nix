{
  description = "pebble_deck - a terminal-inspired Pebble watchface";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    pebble.url = "github:pebble-dev/pebble.nix";
  };

  outputs = { self, nixpkgs, flake-utils, pebble, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pebble.pebbleEnv.${system} {
          packages = with pkgs; [
            gnumake
          ];

          shellHook = ''
            if [ -f package.json ] && [ ! -d node_modules ]; then
              echo "Installing npm dependencies..."
              npm install
            fi
            export PEBBLE_EMULATOR='emery'
            echo "Pebble dev environment loaded. Use 'pebble build' to compile."
          '';
        };
      });
}
