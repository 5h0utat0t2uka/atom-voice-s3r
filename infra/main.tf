terraform {
  required_version = ">= 1.11, < 2.0"

  required_providers {
    vercel = {
      source  = "vercel/vercel"
      version = "= 5.16.0"
    }
  }

  # Single-maintainer local state, excluded from Git. Migrate the backend before sharing execution.
  backend "local" {}
}

# Authentication comes only from VERCEL_API_TOKEN in the process environment.
# Set team_id on each resource so project-scoped tokens need no team-directory access.
provider "vercel" {}

resource "vercel_project" "app" {
  name                                              = var.project_name
  team_id                                           = var.team_id
  framework                                         = "nextjs"
  root_directory                                    = "web"
  node_version                                      = "24.x"
  automatically_expose_system_environment_variables = true
  resource_config = {
    fluid                    = true
    function_default_regions = ["hnd1"]
  }
  git_repository = {
    type = "github"
    repo = "5h0utat0t2uka/atom-voice-s3r"
  }

  lifecycle {
    prevent_destroy = true
  }
}

resource "vercel_project_environment_variable" "openai_api_key" {
  project_id       = vercel_project.app.id
  team_id          = var.team_id
  key              = "OPENAI_API_KEY"
  target           = var.environment_targets.openai_api_key
  sensitive        = !contains(var.environment_targets.openai_api_key, "development")
  value_wo         = var.openai_api_key
  value_wo_version = var.secret_versions.openai_api_key

  lifecycle {
    prevent_destroy = true
  }
}

resource "vercel_project_environment_variable" "device_token" {
  project_id       = vercel_project.app.id
  team_id          = var.team_id
  key              = "DEVICE_TOKEN"
  target           = var.environment_targets.device_token
  sensitive        = !contains(var.environment_targets.device_token, "development")
  value_wo         = var.device_token
  value_wo_version = var.secret_versions.device_token

  lifecycle {
    prevent_destroy = true
  }
}

resource "vercel_firewall_config" "app" {
  project_id = vercel_project.app.id
  team_id    = var.team_id
  enabled    = true

  rules {
    rule {
      name        = "realtime-token"
      description = "Limit authenticated Realtime token issuance to 6 requests per 60 seconds."
      active      = true
      condition_group = [{
        conditions = [{
          type  = "rate_limit_api_id"
          op    = "eq"
          value = "realtime-token"
        }]
      }]
      action = {
        action = "rate_limit"
        rate_limit = {
          algo   = "fixed_window"
          window = 60
          limit  = 6
          keys   = ["header:x-vercel-rate-limit-key"]
          # The SDK distinguishes rate limiting (429) from access denial (403).
          action = "rate_limit"
        }
      }
    }
  }

  lifecycle {
    prevent_destroy = true
  }
}
