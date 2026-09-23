{ pkgs, git-hooks, system, fixedNode, src }:

git-hooks.lib.${system}.run {
  inherit src;
  package = pkgs.prek;
  hooks = {
    detect-private-keys.enable = true;
    check-merge-conflicts.enable = true;
    check-added-large-files = {
      enable = true;
      args = [ "--maxkb=500" ];
    };
    node-tests = {
      enable = true;
      name = "node-test";
      entry = "${fixedNode.pnpm}/bin/pnpm test";
      files = "^(apps|packages)/.*\\.tsx?$";
      pass_filenames = false;
    };
    # biome = {
    #   enable = true;
    #   name = "biome";
    #   entry = "${pkgs.biome}/bin/biome check --write --files-ignore-unknown=true --no-errors-on-unmatched";
    #   pass_filenames = true;
    # };
    # oxfmt = {
    #   enable = true;
    #   package = pkgs.oxfmt;
    #   settings.mode = "write";
    # };
    # oxlint = {
    #   enable = true;
    #   name = "oxlint";
    #   package = pkgs.oxlint;
    #   entry = "${pkgs.oxlint}/bin/oxlint";
    #   files = "\\.(js|jsx|mjs|cjs|ts|tsx|mts|cts)$";
    # };
    betterleaks = {
      enable = true;
      name = "betterleaks";
      entry = "${pkgs.betterleaks}/bin/betterleaks git --pre-commit --redact --staged --verbose";
      pass_filenames = false;
    };
    semgrep = {
      enable = true;
      name = "semgrep";
      entry = "${pkgs.semgrep}/bin/semgrep --config=p/default --config=p/typescript --config=p/javascript --config=p/react --metrics=off --error .";
      pass_filenames = false;
    };
    zizmor = {
      enable = true;
      name = "zizmor";
      entry = "${pkgs.zizmor}/bin/zizmor --collect=workflows,actions .";
      files = "^\\.github/(workflows|actions)/.*";
      pass_filenames = false;
    };
  };
}
