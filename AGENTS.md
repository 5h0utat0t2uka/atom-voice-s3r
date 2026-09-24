# Project layout

- `web/`: Next.js application, configuration, and tests. Follow `web/AGENTS.md` when editing it.
- `firmware/`: Arduino firmware and device tools.
- `infra/`: Terraform configuration for Vercel.
- `nix/`: Shared development environment and pre-commit hooks.

Run `pnpm dev`, `pnpm test`, `pnpm typecheck`, and `pnpm build` from the repository root.
The root package forwards these commands to `web/`. Keep the pnpm workspace and lockfile at the repository root.
