{
  description = "A flake for a Node.js development environment with pnpm and TypeScript support.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    git-hooks.url = "github:cachix/git-hooks.nix";
  };

  outputs = { nixpkgs, flake-utils, git-hooks, ... }: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs { inherit system; };
      fixedNode = import ./nix/fixed-node.nix { inherit pkgs system; };
      preCommit = import ./nix/pre-commit.nix { inherit pkgs git-hooks system fixedNode; src = ./.; };
      createProject = import ./nix/create-project.nix { inherit pkgs; };
      scaffoldApp = import ./nix/scaffold-app.nix { inherit pkgs fixedNode; };
    in
    {
      formatter = pkgs.nixfmt;
      checks = {
        pre-commit = preCommit;
      };
      packages = {
        create-project = createProject;
        scaffold-app = scaffoldApp;
      };
      apps = {
        create-project = {
          type = "app";
          program = "${createProject}/bin/create-project";
        };
        scaffold-app = {
          type = "app";
          program = "${scaffoldApp}/bin/scaffold-app";
        };
      };
      devShells.default = pkgs.mkShell {
        packages = [
          fixedNode.nodejs
          fixedNode.pnpm
          pkgs.age
          pkgs.betterleaks
          pkgs.prek
          pkgs.semgrep
          pkgs.sops
          pkgs.typescript-language-server
          pkgs.zizmor
        ];
        shellHook = ''
          ${preCommit.shellHook}
          echo "node: $(node -v)"
          echo "pnpm: $(pnpm -v)"
        '';
      };
    }
  );
}
