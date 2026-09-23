{ pkgs }:

pkgs.writeShellApplication {
  name = "create-project";
  runtimeInputs = with pkgs; [
    gh
    git
    jq
  ];
  text = ''
    set -euo pipefail
    OWNER="''${1:?OWNER is required}"
    REPO="''${2:?REPO is required}"
    VISIBILITY="''${3:-public}"
    COPILOT_REVIEW="''${4:-false}"
    case "$VISIBILITY" in
      public)
        visibility_flag="--public"
        ruleset_enabled=true
        ;;
      private)
        visibility_flag="--private"
        ruleset_enabled=false
        ;;
      internal)
        visibility_flag="--internal"
        ruleset_enabled=false
        ;;
      *)
        echo "VISIBILITY must be public, private, or internal" >&2
        exit 1
        ;;
    esac
    case "$COPILOT_REVIEW" in
      true)
        copilot_code_review_rule_json='{
          "type": "copilot_code_review",
          "parameters": {
            "review_on_push": true,
            "review_draft_pull_requests": false
          }
        }'
        ;;
      false)
        copilot_code_review_rule_json='null'
        ;;
      *)
        echo "COPILOT_REVIEW must be true or false" >&2
        exit 1
        ;;
    esac
    create_ruleset() {
      jq -n \
        --argjson copilotRule "$copilot_code_review_rule_json" \
        '
        {
          name: "main protection",
          target: "branch",
          enforcement: "active",
          conditions: {
            ref_name: {
              include: ["refs/heads/main"],
              exclude: []
            }
          },
          rules: (
            [
              {
                type: "pull_request",
                parameters: {
                  required_approving_review_count: 0,
                  dismiss_stale_reviews_on_push: false,
                  require_code_owner_review: false,
                  require_last_push_approval: false,
                  required_review_thread_resolution: false,
                  allowed_merge_methods: ["squash", "merge", "rebase"]
                }
              },
              {
                type: "required_status_checks",
                parameters: {
                  strict_required_status_checks_policy: true,
                  do_not_enforce_on_create: true,
                  required_status_checks: [
                    { context: "OSV Scanner" },
                    { context: "Test" }
                  ]
                }
              },
              {
                type: "non_fast_forward"
              }
            ]
            + if $copilotRule == null then [] else [$copilotRule] end
          )
        }
        ' \
        | gh api \
          --method POST \
          -H "Accept: application/vnd.github+json" \
          -H "X-GitHub-Api-Version: 2026-03-10" \
          "/repos/$OWNER/$REPO/rulesets" \
          --input -
    }

    gh auth status >/dev/null
    gh repo create "$OWNER/$REPO" \
      --template 5h0utat0t2uka/template \
      "$visibility_flag" \
      --clone

    # Keep default GITHUB_TOKEN permissions read-only.
    # Individual workflows/jobs should request write permissions explicitly when needed.
    gh api \
      --method PUT \
      -H "Accept: application/vnd.github+json" \
      -H "X-GitHub-Api-Version: 2026-03-10" \
      "/repos/$OWNER/$REPO/actions/permissions/workflow" \
      -f default_workflow_permissions=read \
      -F can_approve_pull_request_reviews=false

    ruleset_configured=false
    if [ "$ruleset_enabled" = true ]; then
      create_ruleset
      ruleset_configured=true
    else
      echo " Skipped repository ruleset: private/internal repositories require GitHub Pro/Team/Enterprise or public visibility."
    fi

    cd "$REPO"
    git switch -c dev
    if [ -f .github/dependabot.yml.template ]; then
      sed "s|__ASSIGNEE__|$OWNER|g" .github/dependabot.yml.template > .github/dependabot.yml
      rm .github/dependabot.yml.template
      echo " Generated .github/dependabot.yml"
    else
      echo " .github/dependabot.yml.template not found; skipped dependabot.yml generation"
    fi
    echo " Created and cloned: $OWNER/$REPO"
    echo " Configured workflow permissions"
    if [ "$ruleset_configured" = true ]; then
      echo " Configured main branch ruleset"
    else
      echo " Skipped main branch ruleset"
    fi
    echo " Created local dev branch"
    echo ""
    echo " Next step:"
    echo "  cd $REPO"
    echo ""
  '';
}
