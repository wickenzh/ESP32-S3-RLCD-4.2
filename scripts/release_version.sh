#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="$ROOT_DIR/RLCD_CLOCK"
VERSION="${1:-}"
MESSAGE="${2:-}"

if [[ -z "$VERSION" || -z "$MESSAGE" ]]; then
  echo "Usage: $0 v0.0.x \"Commit message\"" >&2
  exit 1
fi

APP_VERSION="$(sed -n 's/.*APP_VERSION = "\(v[^"]*\)".*/\1/p' "$PROJECT_DIR/main/main.cpp" | head -n 1)"
SIM_VERSION="$(sed -n 's/.*APP_VERSION = "\(v[^"]*\)".*/\1/p' "$PROJECT_DIR/simulator/main.cpp" | head -n 1)"

if [[ "$APP_VERSION" != "$VERSION" ]]; then
  echo "main.cpp APP_VERSION is $APP_VERSION, expected $VERSION" >&2
  exit 1
fi

if [[ "$SIM_VERSION" != "$VERSION" ]]; then
  echo "simulator APP_VERSION is $SIM_VERSION, expected $VERSION" >&2
  exit 1
fi

if ! grep -Fq -- "- \`$VERSION\`" "$ROOT_DIR/README.md"; then
  echo "README.md does not contain a version record for $VERSION" >&2
  exit 1
fi

cd "$PROJECT_DIR"
. /Users/zhwickner/esp/esp-idf/export.sh
idf.py build
cmake --build simulator/build
"$ROOT_DIR/scripts/generate_previews.sh"

cd "$ROOT_DIR"
git diff --check
git add -A

if git diff --cached --quiet; then
  echo "No staged changes to commit"
else
  git commit -m "$MESSAGE"
fi

if git rev-parse "$VERSION" >/dev/null 2>&1; then
  echo "Tag $VERSION already exists"
else
  git tag "$VERSION"
fi

git push origin main
git push origin "$VERSION"

cd "$PROJECT_DIR"
idf.py build
cmake --build simulator/build
cd "$ROOT_DIR"

"$ROOT_DIR/scripts/publish_gitea_release.sh" "$VERSION"

echo "Released $VERSION"
