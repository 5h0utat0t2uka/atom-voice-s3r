# Declarative imports are recorded in state during apply, after reviewing the plan.
# Keeping these blocks is safe: Terraform skips resources already in state.
import {
  to = vercel_project.app
  id = "${var.team_id}/${var.project_id}"
}

import {
  to = vercel_project_environment_variable.openai_api_key
  id = "${var.team_id}/${var.project_id}/${var.environment_variable_ids.openai_env_id}"
}

import {
  to = vercel_project_environment_variable.device_token
  id = "${var.team_id}/${var.project_id}/${var.environment_variable_ids.device_env_id}"
}

import {
  for_each = var.import_firewall ? toset(["existing"]) : toset([])
  to       = vercel_firewall_config.app
  id       = "${var.team_id}/${var.project_id}"
}
