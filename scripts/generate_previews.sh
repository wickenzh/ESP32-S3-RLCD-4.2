#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="$ROOT_DIR/RLCD_CLOCK"
SDL_BIN="$PROJECT_DIR/simulator/build/weather_clock_sdl"
OUT_DIR="$ROOT_DIR/assets/previews"
FIXED_TIME="${WEATHER_CLOCK_PREVIEW_TIME:-1781350800}"

if [[ ! -x "$SDL_BIN" ]]; then
  echo "Missing SDL preview binary: $SDL_BIN" >&2
  echo "Run: cmake --build \"$PROJECT_DIR/simulator/build\"" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

generate_one() {
  local mode="$1"
  local name="$2"
  local ppm="$OUT_DIR/$name.ppm"
  local png="$OUT_DIR/$name.png"

  WEATHER_CLOCK_SDL_FIXED_TIME="$FIXED_TIME" \
  WEATHER_CLOCK_SDL_SCREENSHOT="$ppm" \
  WEATHER_CLOCK_SDL_MODE="$mode" \
    "$SDL_BIN"

  sips -s format png "$ppm" --out "$png" >/dev/null
  rm -f "$ppm"
  echo "Generated $png"
}

generate_one boot weather_clock_boot
generate_one main weather_clock_main
generate_one alert weather_clock_alert
generate_one low weather_clock_low_battery
generate_one settings weather_clock_settings
generate_one setup weather_clock_setup
