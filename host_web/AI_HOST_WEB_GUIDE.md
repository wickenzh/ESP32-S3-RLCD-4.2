# WeatherClock Host Web AI Guide

This document is for future AI agents or developers taking over `host_web/`.

## Interactive Simulator

Keep the simulator aligned to the page content edges. Use 18px page headings,
15px subsection headings, 12px muted metadata, and 16px control spacing. The
portal toggle has a fixed width to avoid moving when its text changes. Portal
form styling intentionally follows the firmware, not the dark host UI.

Device view is the default. The portal button next to the source link toggles
between device and portal without resetting either. Portal HTML loads on first
open; hidden device rendering and pending key holds are paused/cancelled.

`simulator-ui.js` lazily loads the WASM target built from `RLCD_CLOCK/simulator`.
The firmware renderer is shared where available; `web_demo_state.h` supplies
fake state and does not mutate NVS or call production services. The independent
portal reuses the firmware HTML/CSS with a demo-only script, an opaque sandbox,
and CSP blocking network and form navigation. Its trusted build artifact is
fetched through the parent service worker for offline use, then assigned to
srcdoc. Never interpolate user input into that document.

Pages requires generated artifacts with matching hashes; missing/corrupt files
fail the build. `test_simulator_artifacts.mjs`, `test_pages_site.mjs`, native
`web_demo_state_test` and browser offline/key checks cover this boundary.

The device toolbar places KEY on the left and BOOT on the right. Holding KEY
for 1.2 seconds dispatches one long-press event before release; release must not
also dispatch a short press. Mouse capture cancellation, focus loss and tab
changes cancel pending holds. Keyboard K/B uses the same timing. Run
`node host_web/scripts/test_simulator_keys.mjs` for the input regression checks.
This browser adapter does not change physical firmware button handling.

## Scope

Quick configuration stays on the `settings` hash and uses `quick-config.js` to
generate a fixed `http://192.168.4.1/save` URL with URLSearchParams. It never
fetches, navigates, logs credentials or persists input. The inert dialog-method
form and initially disabled submit button prevent native network submission if
the module fails. Editing invalidates output; page exit/reset clears it.
Match firmware field capacities (UTF-8), the 159-byte encoded field limit and
512-byte request URI limit. API key/host blanks require existing device values;
city and backup SSID blanks clear those settings. Empty main SSID selects fixed
offline time (2024–2035). Do not change firmware or WCA1 for this feature.
Run `node host_web/scripts/test_quick_config.mjs` and verify zero network traffic
and script-failure behavior with dummy data only.
The two groups share five subgrid rows (heading plus four inputs), keeping
corresponding inputs aligned despite different help lengths. Safety notices
use three separate complete sentences, not forced line breaks inside a paragraph.

Work in this branch is limited to `host_web/`.

Read the repository-root AGENTS.md, TASK.md, HANDOFF.md and docs/DECISIONS.md before development. Ordinary changes follow the same rules as firmware: validate, commit and push Gitea only. Keep firmware versions unchanged for web-only work.

With explicit one-time authorization, use the unified source publisher with GITHUB_CLOCK_SOURCE_ONLY=1. The only active public repository is `wickenzh/ESP32-S3-RLCD-4.2`, with firmware under `RLCD_CLOCK/` and this app under `host_web/`. The old `_Web` repository retains its code and history, but its Pages site was removed and deployment workflow disabled at the user's request on 2026-09-09. Do not re-enable or publish the legacy site without explicit authorization.

## Product Focus

The host web app is mainly a browser-based resource tool for WeatherClock:

- Convert a user GIF into the device main-page GIF resource.
- Convert user still images into gallery-page image resources.
- Preview converted 1-bit results before writing.
- Build `custom_assets.bin`.
- Write `custom_assets.bin` to the ESP32-S3 `assets` flash partition.

Firmware flashing and serial logs exist only as auxiliary tools.

## Device Resource Format

The firmware reads a custom asset package from the `assets` partition:

- Partition name: `assets`
- Offset: read dynamically from the device partition table.
- Size: read dynamically from the device partition table; WCA1 package generation still keeps a `2M` upper bound.
- Package filename used by the app: `custom_assets.bin`

Package magic/version:

- Magic: `WCA1`, little-endian `0x31414357`
- Version: `1`

Supported entry types:

- `1`: `main_gif`
  - Fixed size: `84 x 84`
  - Fixed frames: `60`
  - Continuous full-frame 1-bit bitstream. Do not pad each row.
  - Frame size: `84 * 84 / 8 = 882` bytes.
  - Total payload size: `882 * 60 = 52920` bytes.
  - `bytes_per_row` should be `0`; firmware does not use row stride for GIF.
- `2`: `gallery_image`
  - Fixed size: `220 x 208`
  - Up to `24` images.
  - Packed 1-bit rows.
  - Row stride: `ceil(220 / 8) = 28` bytes.
  - Payload size per image: `28 * 208 = 5824` bytes.
- `3`: `weather_city`
  - Optional UTF-8 fallback text payload, `1..31` bytes.
  - `index`, `width`, `height`, `frame_count`, and `bytes_per_row` are `0`.
  - Omit this entry when the user leaves the city empty. It is only a fallback and does not write NVS.
- `4`: `ota_manifest_url`
  - Optional UTF-8 fallback text payload, `1..255` bytes.
  - `index`, `width`, `height`, `frame_count`, and `bytes_per_row` are `0`.
  - Omit this entry when empty. If the user enters a base URL, normalize it to `/firmware/latest.json`.

The browser app builds the header, entry table, payload, header CRC32, payload CRC32, and per-entry CRC32 in `host_web/app.js`.

## Current UI

Tab navigation uses hash routes: #screens opens the SDL gallery directly; other routes are #assets, #writer, #firmware, #serial and #settings. activateTab validates against actual tab IDs, updates history without an anchor scroll, and restores state on hashchange/popstate. Empty or unknown hashes show assets. Keep route changes independent from resource generation and device actions. Cache suffix v53 includes routing.

Six tabs include `界面预览` (`screens`) after serial and before settings. Eight 400x300 SDL snapshots are served from assets/screens. GitHub deployment MUST use repository-root previews via SDL_PREVIEW_SOURCE; previews/** pushes trigger Pages. build_sdl_previews.mjs selects the eight names referenced by index.html, validates and copies the canonical PNGs, then embeds their aggregate SHA256 in SW v52's cache name. Missing/corrupt images fail deployment instead of falling back to stale bundled files. Local full-repo builds default to assets/previews; sync_sdl_previews.py is only for standalone local preview copies. Run test_sdl_preview_sync.mjs to verify upstream changes and cache invalidation. These are snapshots, not live device telemetry; normal page reopen after deployment loads the updated worker.

Next-step guidance uses one transient target and one 2400ms timer. Only enabled, visible controls are hinted after successful operations. User click/input/change, tab changes and package invalidation clear the hint. CSS pulses an outline-like shadow three times; reduced-motion uses a static outline. No autonomous serial connection, write or tab navigation is allowed. Run scripts/test_next_step_hint.mjs for eligibility/replacement/expiry. Cache v50 includes this change; ordinary development keeps the displayed/firmware versions unchanged. The privacy notice covers local image/GIF/config processing, not offline availability of remote firmware downloads.

Web v0.0.29 / cache v47 removes the ESP Web Tools fallback entry, loader, bundled dependency and example manifest. Main esptool-js flashing remains. Number only primary steps: resource creation 1 select, 2 convert, 3 build (BIN download optional); resource writing 1 inspect device, 2 write; firmware 1 source/version, 2 inspect partitions, 3 download/verify or select custom file, 4 flash. Refresh and clearing are unnumbered alternatives, never required steps.

Since web v0.0.30 / cache v48, successful WCA1 generation stays on the creation tab without moving focus. A green notice distinguishes generated from written and offers an explicit go-to-writer button. Only that button activates writer and focuses inspection. Invalidation hides the notice; never auto-navigate while creating resources or automatically connect/write a device.

Version v0.0.27 uses a default dark desktop theme. Validate at 1440x900 and 1024x768; mobile is not a supported acceptance target per the user's scope. Keep preview pixels unchanged: light canvas backgrounds represent device output, not missing dark styling. Keep hover geometry stable, visible keyboard focus, reduced-motion support, and table overflow inside its own wrapper. Business code in app.js is unchanged by this theme update. The isolated Service Worker cache suffix is v45; its prefix and cleanup ownership remain unchanged.

The app has six tabs, ordered as 资源制作, 资源写入, 固件烧录, 串口日志, 界面预览, 设置:

- `资源制作`: Primary tab and default view. Handles GIF and still-image conversion.
- `资源写入`: Selects a Web Serial device, reads the ESP-IDF partition table from `0x8000`, verifies `assets` as `data / subtype 0x40`, then writes generated `custom_assets.bin` to the actual `assets` address from the device partition table.
- `固件烧录`: Auxiliary serial flashing. `merged` firmware is written only to `0x0`. OTA App firmware is written only to dynamically discovered `ota_0` / `ota_1` partitions after reading the device partition table. Never use a fixed App slot address.
- `串口日志`: Auxiliary serial log and manual command console.
- `界面预览`: The eight primary SDL page snapshots.
- `设置`: Optional fallback config written into `custom_assets.bin` as WCA1 text entries. Weather city is used only when device NVS has no manual city. OTA manifest URL is a fallback after firmware built-in OTA sources. These settings do not write NVS.

Do not reintroduce Wi-Fi provisioning, default AP/IP panels, device info sidebars, OTA manifest reading, or notes pages unless the user asks.

All baud-rate selectors currently default to `115200`.

Update the displayed web version in `index.html` on every user-visible development change; do not change the firmware version for web-only updates.

## GitHub Pages Preview

Use the deployed GitHub Pages URL for preview and testing:

```text
https://wickenzh.github.io/ESP32-S3-RLCD-4.2/
```

Do not add local HTTP/HTTPS preview servers back into `host_web/` unless the user explicitly asks.

## Important Files

- `index.html`: Static UI structure.
- `styles.css`: Layout and visual styling.
- `app.js`: All client-side conversion, package building, serial writing, and flashing logic.
- `README.md`: User-facing usage/deployment summary.

## Known Constraints

- GIF original previews validate GIF87a/GIF89a bytes before creating an image/gif Blob, require HTMLImageElement, preserve animation bytes, and reject stale header reads. Run node host_web/scripts/test_gif_preview.mjs for these boundaries. Pages mock fetch uses parsed HTTPS hostname/path checks and rejects unexpected requests. CodeQL alerts #9/#10 require a future authorized GitHub scan to confirm closure; do not dismiss them merely because local tests pass. Cache v49 contains this hardening; displayed and firmware versions remain unchanged for ordinary development.

- GIF decoding first uses the local parser in `app.js`, including global/local palettes, transparency, interlaced frames, and GIF disposal modes. Keep this local path so conversion preview works on intranet Gitea/GitHub Pages without a runtime CDN dependency.
- If local GIF decoding fails, the app tries the browser `ImageDecoder` API, then falls back to a single-frame image preview repeated across 60 frames.
- GIF conversion must output exactly 60 frames. The local decoder samples those frames evenly across the full GIF playback timeline using frame delays, so long animations are represented from start to end instead of only taking the first 60 source frames.
- Converted GIF preview is animated by replaying the 60 converted 1-bit frames on the preview canvas.
- Original GIF preview uses a normal `<img>` element with an object URL, so the uploaded GIF should animate before conversion.
- File selection immediately draws the uploaded GIF first frame or the currently selected still image on the source canvas so users can see what was loaded before conversion.
- Still images support selecting up to 24 files and converting them together. The preview selector shows one chosen image at a time; the converted preview is reconstructed from that image's packed 1-bit data.
- Still image conversion applies a configurable edge fade before 1-bit packing. The default is 18 px and blends the four edges toward white so non-transparent image backgrounds do not leave a hard rectangle on the device screen.
- The resource writer's erase action writes an erased 4 KB sector header (`0xFF`) at the actual `assets` address read from the device partition table, invalidating the custom asset package so firmware falls back to built-in resources after reset.
- Web Serial and browser flashing need Chrome/Edge or another Chromium browser with serial support.
- The serial writing path uses a repository-local pinned copy of `esptool-js 0.5.6` under `host_web/vendor/`; do not replace it with runtime CDN imports. Preserve its license and update the service-worker asset list when changing versions. `esptool-js` expects file data as a binary string, so `Uint8Array` payloads are converted before calling `writeFlash`.
- After `writeFlash`, the app explicitly pulses serial RTS/DTR signals to reset the ESP32-S3. Keep this behavior unless hardware reset wiring changes.
- The resource writer must keep the partition-table preflight: read flash `0x8000..0x8FFF`, parse 32-byte ESP-IDF partition entries, and only enable resource write/erase after finding `assets` with type `data`, subtype `0x40`, and enough space for the generated resource package.
- Firmware flashing uses GitHub Release assets from `wickenzh/ESP32-S3-RLCD-4.2` as the sole online source. GitHub's final Release asset CDN does not expose CORS headers for JavaScript byte access, so the page must never fetch Release binaries directly. `.github/workflows/static.yml` runs `scripts/build_pages_site.mjs` to mirror the latest 10 complete releases into the ephemeral Pages artifact, after checking size and GitHub asset `digest` (`sha256:...`); do not commit mirrored bin files to Git history. The deployed page reads same-origin `firmware/releases.json` and same-origin firmware bytes, then verifies size and SHA256 again in browser memory before enabling flashing. Online firmware remains fully automatic; only the separate custom-firmware source uses a file picker. Accept the merged asset only for the `0x0` target and the App asset only for dynamically discovered App targets.
- WeatherClock v1.5.x adds a `model` partition. The web host must not write App binaries to `assets`, `model`, `nvs`, bootloader, partition-table, or any fixed legacy address. Future standalone model flashing must also discover `model` address and size from the device partition table.
