#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="$ROOT_DIR/RLCD_CLOCK"
BUILD_DIR="$PROJECT_DIR/build"
VERSION="${1:-}"

if [[ -z "$VERSION" ]]; then
  VERSION="$(sed -n 's/.*APP_VERSION = "\(v[^"]*\)".*/\1/p' "$PROJECT_DIR/main/main.cpp" | head -n 1)"
fi

if [[ -z "$VERSION" ]]; then
  echo "Unable to detect version. Usage: $0 v0.0.x" >&2
  exit 1
fi

REMOTE_URL="$(git -C "$ROOT_DIR" config --get remote.origin.url)"
if [[ ! "$REMOTE_URL" =~ ^https?:// ]]; then
  echo "Unsupported origin URL: $REMOTE_URL" >&2
  exit 1
fi

REMOTE_WITHOUT_PROTO="${REMOTE_URL#http://}"
REMOTE_WITHOUT_PROTO="${REMOTE_WITHOUT_PROTO#https://}"
GITEA_HOST="${REMOTE_WITHOUT_PROTO%%/*}"
REPO_PATH="${REMOTE_WITHOUT_PROTO#*/}"
REPO_PATH="${REPO_PATH%.git}"
REPO_OWNER="${REPO_PATH%%/*}"
REPO_NAME="${REPO_PATH#*/}"
GITEA_BASE="${REMOTE_URL%%://*}://$GITEA_HOST"
API_BASE="$GITEA_BASE/api/v1/repos/$REPO_OWNER/$REPO_NAME"

APP_BIN="$BUILD_DIR/weather_clock.bin"
BOOTLOADER_BIN="$BUILD_DIR/bootloader/bootloader.bin"
PARTITION_BIN="$BUILD_DIR/partition_table/partition-table.bin"
FLASH_ARGS="$BUILD_DIR/flash_args"

for file in "$APP_BIN" "$BOOTLOADER_BIN" "$PARTITION_BIN" "$FLASH_ARGS"; do
  if [[ ! -f "$file" ]]; then
    echo "Missing build artifact: $file" >&2
    echo "Run: cd \"$PROJECT_DIR\" && idf.py build" >&2
    exit 1
  fi
done

AUTH_ARGS=()
if [[ -n "${GITEA_TOKEN:-}" ]]; then
  AUTH_ARGS=(-H "Authorization: token $GITEA_TOKEN")
elif [[ -n "${GITEA_USER:-}" && -n "${GITEA_PASSWORD:-}" ]]; then
  AUTH_ARGS=(-u "$GITEA_USER:$GITEA_PASSWORD")
else
  echo "Set GITEA_TOKEN or GITEA_USER/GITEA_PASSWORD before publishing." >&2
  exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/weather-clock-release.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

PACKAGE_DIR="$WORK_DIR/ESP32-S3-RLCD-4.2-$VERSION"
mkdir -p "$PACKAGE_DIR"

cp "$APP_BIN" "$PACKAGE_DIR/weather_clock.bin"
cp "$BOOTLOADER_BIN" "$PACKAGE_DIR/bootloader.bin"
cp "$PARTITION_BIN" "$PACKAGE_DIR/partition-table.bin"
cp "$FLASH_ARGS" "$PACKAGE_DIR/flash_args"

cat > "$PACKAGE_DIR/README_FLASH.txt" <<EOF
ESP32-S3 RLCD Weather Clock $VERSION

Flash command:
cd "$PROJECT_DIR" && . /Users/zhwickner/esp/esp-idf/export.sh && idf.py -p PORT flash monitor

Manual esptool offsets:
0x0     bootloader.bin
0x8000  partition-table.bin
0x10000 weather_clock.bin
EOF

MERGED_BIN="$WORK_DIR/weather_clock_${VERSION}_merged.bin"
if command -v esptool.py >/dev/null 2>&1; then
  esptool.py --chip esp32s3 merge_bin \
    -o "$MERGED_BIN" \
    --flash_mode dio \
    --flash_size 16MB \
    --flash_freq 80m \
    0x0 "$BOOTLOADER_BIN" \
    0x8000 "$PARTITION_BIN" \
    0x10000 "$APP_BIN" >/dev/null
fi

ZIP_FILE="$WORK_DIR/weather_clock_${VERSION}_flash_package.zip"
(cd "$WORK_DIR" && zip -qr "$ZIP_FILE" "ESP32-S3-RLCD-4.2-$VERSION")

README_LINE="$(grep -F -- "- \`$VERSION\`" "$ROOT_DIR/README.md" | head -n 1 | sed 's/^- //')"
COMMIT_SHA="$(git -C "$ROOT_DIR" rev-parse --short HEAD)"
BUILD_TIME="$(date '+%Y-%m-%d %H:%M:%S %z')"

BODY="版本：$VERSION

提交：$COMMIT_SHA
构建时间：$BUILD_TIME

版本说明：
${README_LINE:-本版本包含固件更新。}

下载说明：
- weather_clock_${VERSION}.bin：App 固件。
- weather_clock_${VERSION}_flash_package.zip：包含 bootloader、分区表、App 固件和烧录说明。
- weather_clock_${VERSION}_merged.bin：合并后的完整镜像，可用于整包写入。"

RELEASE_JSON="$WORK_DIR/release.json"
HTTP_CODE="$(curl -sS -o "$RELEASE_JSON" -w "%{http_code}" "${AUTH_ARGS[@]}" \
  "$API_BASE/releases/tags/$VERSION")"

if [[ "$HTTP_CODE" == "200" ]]; then
  RELEASE_ID="$(jq -r '.id' "$RELEASE_JSON")"
  curl -sS -X PATCH "${AUTH_ARGS[@]}" \
    -H "Content-Type: application/json" \
    -d "$(jq -n --arg name "$VERSION" --arg body "$BODY" '{name:$name, body:$body, draft:false, prerelease:false}')" \
    "$API_BASE/releases/$RELEASE_ID" >/dev/null
elif [[ "$HTTP_CODE" == "404" ]]; then
  curl -sS -X POST "${AUTH_ARGS[@]}" \
    -H "Content-Type: application/json" \
    -d "$(jq -n --arg tag "$VERSION" --arg name "$VERSION" --arg body "$BODY" '{tag_name:$tag, name:$name, body:$body, draft:false, prerelease:false}')" \
    "$API_BASE/releases" > "$RELEASE_JSON"
  RELEASE_ID="$(jq -r '.id' "$RELEASE_JSON")"
else
  echo "Failed to query release $VERSION, HTTP $HTTP_CODE" >&2
  cat "$RELEASE_JSON" >&2
  exit 1
fi

ASSETS_JSON="$WORK_DIR/assets.json"
curl -sS "${AUTH_ARGS[@]}" "$API_BASE/releases/$RELEASE_ID/assets" > "$ASSETS_JSON"

delete_asset_if_exists() {
  local name="$1"
  local id
  id="$(jq -r --arg name "$name" '.[] | select(.name == $name) | .id' "$ASSETS_JSON" | head -n 1)"
  if [[ -n "$id" && "$id" != "null" ]]; then
    curl -sS -X DELETE "${AUTH_ARGS[@]}" "$API_BASE/releases/$RELEASE_ID/assets/$id" >/dev/null
  fi
}

upload_asset() {
  local path="$1"
  local name="$2"
  delete_asset_if_exists "$name"
  curl -sS -X POST "${AUTH_ARGS[@]}" \
    -F "attachment=@$path" \
    "$API_BASE/releases/$RELEASE_ID/assets?name=$name" >/dev/null
  echo "Uploaded $name"
}

upload_asset "$APP_BIN" "weather_clock_${VERSION}.bin"
upload_asset "$ZIP_FILE" "weather_clock_${VERSION}_flash_package.zip"
if [[ -f "$MERGED_BIN" ]]; then
  upload_asset "$MERGED_BIN" "weather_clock_${VERSION}_merged.bin"
fi

echo "Published $VERSION to $GITEA_BASE/$REPO_OWNER/$REPO_NAME/releases/tag/$VERSION"
