{
  description = "Atom VoiceS3R firmware and Next.js development environment.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    git-hooks.url = "github:cachix/git-hooks.nix";
    arduino-ctags = {
      url = "github:arduino/ctags/5.8-arduino11";
      flake = false;
    };
  };

  outputs = { nixpkgs, flake-utils, git-hooks, arduino-ctags, ... }: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs { inherit system; };
      fixedNode = import ./nix/fixed-node.nix { inherit pkgs system; };
      arduino = import ./nix/arduino-cli.nix { inherit pkgs arduino-ctags; };
      preCommit = import ./nix/pre-commit.nix { inherit pkgs git-hooks system fixedNode; src = ./.; };
    in
    {
      formatter = pkgs.nixfmt;
      checks = {
        pre-commit = preCommit;
      };
      devShells.default = pkgs.mkShell {
        packages = [
          fixedNode.nodejs
          fixedNode.pnpm
          arduino.arduinoCli
          pkgs.age
          pkgs.betterleaks
          pkgs.prek
          pkgs.python3
          pkgs.semgrep
          pkgs.sops
          pkgs.typescript-language-server
          pkgs.zizmor
        ];
        # Pass this directory as tools.ctags.path when compiling sketches.
        ARDUINO_CTAGS_PATH = "${arduino.arduinoCtags}/bin";
        shellHook = ''
          ${preCommit.shellHook}
          echo "node: $(node -v)"
          echo "pnpm: $(pnpm -v)"
        '';
      };
    }
  );
}
