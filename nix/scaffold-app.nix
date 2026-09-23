{ pkgs, fixedNode }:

pkgs.writeShellApplication {
  name = "scaffold-app";
  runtimeInputs = [
    fixedNode.nodejs
    fixedNode.pnpm
  ];
  text = ''
    set -euo pipefail
    FRAMEWORK="''${1:-}"
    if [ -z "$FRAMEWORK" ]; then
      echo "Usage: nix run .#scaffold-app -- <vite|next|astro>" >&2
      exit 1
    fi
    case "$FRAMEWORK" in
      vite)
        exec pnpm create vite .
        ;;
      next|astro)
        ;;
      *)
        echo "Unsupported framework: $FRAMEWORK (expected: vite, next, astro)" >&2
        exit 1
        ;;
    esac
    PROJECT_DIR="$PWD"
    PROJECT_NAME="$(basename "$PROJECT_DIR")"
    BACKUP_DIR="$(cd .. && pwd)/.''${PROJECT_NAME}-template-backup-$(date +%s)"
    SKIPPED_DUPLICATES_FILE="$(mktemp)"
    if [ -e "$BACKUP_DIR" ]; then
      echo "Error: $BACKUP_DIR already exists." >&2
      exit 1
    fi
    mkdir "$BACKUP_DIR"

    restore() {
      if [ -d "$BACKUP_DIR" ]; then
        for path in "$BACKUP_DIR"/* "$BACKUP_DIR"/.[!.]* "$BACKUP_DIR"/..?*; do
          [ -e "$path" ] || continue
          filename="$(basename "$path")"
          destination="$PROJECT_DIR/$filename"
          if [ -e "$destination" ]; then
            prefixed_filename="template_$filename"
            prefixed_destination="$PROJECT_DIR/$prefixed_filename"
            if [ -e "$prefixed_destination" ]; then
              i=1
              while [ -e "$PROJECT_DIR/template_''${i}_$filename" ]; do
                i=$((i + 1))
              done
              prefixed_filename="template_''${i}_$filename"
              prefixed_destination="$PROJECT_DIR/$prefixed_filename"
            fi
            mv "$path" "$prefixed_destination"
            printf '%s -> %s\n' "$filename" "$prefixed_filename" >> "$SKIPPED_DUPLICATES_FILE"
            continue
          fi
          mv "$path" "$PROJECT_DIR/"
        done
        rm -rf "$BACKUP_DIR"
        if [ -s "$SKIPPED_DUPLICATES_FILE" ]; then
          echo " Some template files conflicted with scaffold-generated files and were restored with a prefix:" >&2
          sed 's/^/  - /' "$SKIPPED_DUPLICATES_FILE" >&2
        fi
      fi
      rm -f "$SKIPPED_DUPLICATES_FILE"
    }

    trap 'status=$?; restore; exit "$status"' EXIT
    find "$PROJECT_DIR" -mindepth 1 -maxdepth 1 \
      ! -name .git \
      -exec mv {} "$BACKUP_DIR/" \;

    case "$FRAMEWORK" in
      next)
        pnpm create next-app@latest .
        ;;
      astro)
        pnpm create astro@latest .
        ;;
    esac

    status=$?
    trap - EXIT
    restore
    exit "$status"
  '';
}

