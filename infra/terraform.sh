#!/bin/sh
set -eu
umask 077

# SOPS supplies these to the process; no plaintext .tfvars or command-line secrets.
: "${VERCEL_API_TOKEN:?Add VERCEL_API_TOKEN to .enc.env}"
: "${OPENAI_API_KEY:?Missing OPENAI_API_KEY}"
: "${DEVICE_TOKEN:?Missing DEVICE_TOKEN}"
export TF_VAR_openai_api_key="$OPENAI_API_KEY"
export TF_VAR_device_token="$DEVICE_TOKEN"
# Provider debug logs may contain secrets. Normal plan output still shows changes.
unset TF_LOG TF_LOG_PATH TF_LOG_CORE TF_LOG_PROVIDER
unset OPENAI_API_KEY DEVICE_TOKEN SSID PASS REALTIME_TOKEN_URL

exec terraform -chdir=infra "$@"
