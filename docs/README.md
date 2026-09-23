# Frontend Template with Nix  
Template for setting up a development environment for front-end projects using direnv, flake.nix, and pnpm.  
This template includes basic security measures:  
- [Dependabot](https://docs.github.com/ja/code-security/tutorials/secure-your-dependencies/dependabot-quickstart-guide) for dependency auto updates  
- [GitHub Copilot](https://github.com/features/copilot) for code review with AI (optional)
- [OSV-Scanner](https://github.com/google/osv-scanner) for existing vulnerability scanning  
- [semgrep](https://github.com/semgrep/semgrep) for static application security testing
- [zizmor](https://github.com/zizmorcore/zizmor) for GitHub Actions static analysis  
- [Betterleaks](https://github.com/betterleaks/betterleaks) for secret scanning  
- [SOPS](https://github.com/getsops/sops) for secret encryption  

# Requirements  
- **Nix with flakes enabled**  
  If you have not installed Nix yet:  
  Install via [Determinate Nix installer](https://github.com/DeterminateSystems/nix-installer) or [Official Nix installer](https://github.com/NixOS/nix-installer)  

- **`gh` authentication**  
  If you have not authenticated `gh` yet:  
  ``` nix
  # Check authentication status 
  nix shell nixpkgs#gh -c gh auth status  

  # Run the following command to login
  nix shell nixpkgs#gh -c gh auth login  
  ```


## 1. リポジトリを作成
クローン先のディレクトリに移動
``` sh
cd path/to/parent-directory
```

以下のコマンドでリポジトリを作成  
``` sh
nix run github:5h0utat0t2uka/template#create-project -- OWNER REPO VISIBILITY COPILOT_REVIEW
```

引数の内容は下記  

| Argument | Required | Default | Description |
|:---|:---|:---|:---|
| `OWNER`          | required | -        | GitHub のユーザー名もしくは組織名 |
| `REPO`           | required | -        | GitHub のリポジトリ名 |
| `VISIBILITY`     | optional | `public` | GitHub の可視性を `public`, `private`, `internal` のいずれかで指定 |
| `COPILOT_REVIEW` | optional | `false`  | GitHub Copilot のレビュー可否を `true`もしくは`false` で指定 |

> [!TIP]
> スクリプトの内容は [`nix/create-project.nix`](../nix/create-project.nix) を参照

## 2. プロジェクトの設定
``` sh
cd REPO
git switch -c dev
```

- [`nix/fixed-node.nix`](../nix/fixed-node.nix) の `nodeVersion`, `pnpmVersion` をプロジェクトに合わせて変更  
- [`pnpm-workspace.yaml`](../pnpm-workspace.yaml) の内容をプロジェクトに合わせて変更  
- Dependabotの設定  
  - [`.github/dependabot.yml`](../.github/dependabot.yml.template) の `assignees`を変更  
  - PRラベルを作成:  
  ``` sh
  gh label create "dependencies" --color "#C7C7C7" 
  gh label create "github-actions" --color "#474747" 
  gh label create "npm" --color "#CC3534" 
  gh label create "nix" --color "#4D6FB7" 
  ```

テンプレートのDependabot, CIの設定内容は下記  

| File | Schedule | Description | Auto merge |
|:---|:---|:---|:---|
| [`dependabot.yml`](../.github/dependabot.yml.templrte) | 毎週日曜日の AM 04:00 | GitHub Actions, npm, flake.lock のバージョンを更新してPR作成 | 未設定 |
| [`ci.yml`](../.github/workflows/ci.yml)                | `main` ブランチへのPR | - 追加・更新された依存関係に対してOSSVデータベースから脆弱性を確認<br>- GitHub ActionsのLintや、シークレットの漏洩を確認<br>- `pnpm install --frozen-lockfile`を行い`lint`, `build`まで実行 | 未設定 |

## 3. `.envrc`を作成
``` sh
echo 'watch_dir nix
watch_file pnpm-lock.yaml
watch_file pnpm-workspace.yaml
use flake' >> .envrc
```

## 4. `.envrc`の許可
``` sh
direnv allow

# node と pnpm のバージョン確認
node -v
pnpm -v
```

## 5. スキャフォールド  
フレームワークによってプロジェクトルートの既存ファイルがスキャフォールドを止めるため、下記のコマンドで一時的に既存ファイルを親ディレクトリに退避させる薄いラッパースクリプトを実行   

> [!TIP]
> スクリプトの内容は [`nix/scaffold-app.nix`](../nix/scaffold-app.nix) を参照

``` sh
# Vite (Select "Ignore files and continue" when prompted)
nix run .#scaffold-app -- vite

# Next.js
nix run .#scaffold-app -- next

# Astro
nix run .#scaffold-app -- astro
```

## 6. `pnpm`バージョンの明示
`package.json` の `packageManager` に追記
``` json
{
  "packageManager": "pnpm@10.33.2",
}
```

`pnpm`のバージョンが`11.0.0`以降の場合
``` json
{
  "devEngines": {
    "packageManager": {
      "name": "pnpm",
      "version": ">=11.0.0"
      "onFail": "error"
    }
  }
}
```

## 7. `.gitignore`に追記
フレームワークにより生成される内容は異なるため、プロジェクトに合わせて調整  
``` sh
echo '.direnv
.pre-commit-config.yaml
.env' >> .gitignore
```

## 8. リモートに反映  
- `dev`にコミットしてプッシュ  
``` sh
git add .
git commit -m "setup project"
git push -u origin dev
```

- PR作成後`main`にマージ  
``` sh
gh pr create --base main --head dev --fill
gh pr checks --watch
gh pr merge --squash --delete-branch
```

- ローカルをリモートの`main`に揃える  
``` sh
git switch main
git pull --ff-only --prune origin main
```
``` sh
git fetch --prune origin
git switch -c <branch-name> origin/main
```
