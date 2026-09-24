variable "team_id" {
  type        = string
  description = "Existing Vercel team ID."
  validation {
    condition     = startswith(var.team_id, "team_")
    error_message = "Use the existing team_ ID."
  }
}

variable "project_id" {
  type        = string
  description = "Existing project to import, never create a replacement."
  validation {
    condition     = startswith(var.project_id, "prj_")
    error_message = "Use the existing prj_ ID."
  }
}

variable "project_name" {
  type        = string
  description = "Current Vercel project name."
}

variable "environment_variable_ids" {
  type = object({
    openai_env_id = string
    device_env_id = string
  })
  description = "IDs of the two existing Production environment variables (not their values)."
}

variable "import_firewall" {
  type        = bool
  default     = true
  description = "Import the existing active firewall configuration. False only if none exists."
}

variable "environment_targets" {
  type = object({
    openai_api_key = set(string)
    device_token   = set(string)
  })
  default = {
    openai_api_key = ["production"]
    device_token   = ["production"]
  }
  description = "Preserve the existing targets during import. Vercel disallows sensitive=true for development."
  validation {
    condition = alltrue([for targets in values(var.environment_targets) :
      contains(targets, "production") && alltrue([for target in targets : contains(["production", "preview", "development"], target)])
    ])
    error_message = "Targets must include production and contain only supported Vercel environments."
  }
}

variable "secret_versions" {
  type = object({
    openai_api_key = number
    device_token   = number
  })
  default = {
    openai_api_key = 1
    device_token   = 1
  }
  description = "Increment the matching version whenever a SOPS secret changes."
  validation {
    condition     = alltrue([for v in values(var.secret_versions) : v >= 1 && floor(v) == v])
    error_message = "Secret versions must be positive integers."
  }
}

variable "openai_api_key" {
  type      = string
  sensitive = true
  ephemeral = true
  validation {
    condition     = length(trimspace(var.openai_api_key)) > 0
    error_message = "OPENAI_API_KEY is required in .enc.env."
  }
}

variable "device_token" {
  type      = string
  sensitive = true
  ephemeral = true
  validation {
    condition     = can(regex("^[a-f0-9]{64}$", var.device_token))
    error_message = "DEVICE_TOKEN must contain 64 lowercase hexadecimal characters."
  }
}
