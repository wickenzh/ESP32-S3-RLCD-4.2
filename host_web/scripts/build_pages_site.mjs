import { createHash } from "node:crypto";
import { cp, mkdir, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { buildSdlPreviews } from "./build_sdl_previews.mjs";

const SOURCE_REPOSITORY = "wickenzh/ESP32-S3-RLCD-4.2";
const SOURCE_ROOT = fileURLToPath(new URL("../", import.meta.url));
const OUTPUT_ROOT = path.resolve(process.argv[2] || path.join(SOURCE_ROOT, "_site"));
if (OUTPUT_ROOT === SOURCE_ROOT || SOURCE_ROOT.startsWith(`${OUTPUT_ROOT}${path.sep}`)) {
  throw new Error("Site output must not contain the source directory");
}
const requestedLimit = Number(process.env.FIRMWARE_RELEASE_LIMIT || 10);
const releaseLimit = Math.min(10, Math.max(1, Number.isFinite(requestedLimit) ? requestedLimit : 10));

function expectedSha256(asset) {
  const digest = String(asset?.digest || "").trim().toLowerCase();
  return digest.startsWith("sha256:") ? digest.slice("sha256:".length) : "";
}

function findReleaseAsset(release, merged) {
  return release.assets?.find((asset) => {
    const name = String(asset?.name || "");
    return name.toLowerCase().endsWith(".bin") && /_merged\.bin$/i.test(name) === merged;
  });
}

function normalizeAsset(asset) {
  const name = path.basename(String(asset?.name || ""));
  const sha256 = expectedSha256(asset);
  const size = Number(asset?.size);
  const downloadUrl = String(asset?.browser_download_url || "");
  if (!name.endsWith(".bin") || !/^[a-f0-9]{64}$/.test(sha256) || !Number.isSafeInteger(size) || size <= 0) return undefined;
  if (!downloadUrl.startsWith(`https://github.com/${SOURCE_REPOSITORY}/releases/download/`)) return undefined;
  return { name, sha256, size, downloadUrl };
}

async function fetchWithRetry(url, options, label) {
  let lastError;
  for (let attempt = 1; attempt <= 4; attempt += 1) {
    try {
      const response = await fetch(url, { ...options, signal: AbortSignal.timeout(120000) });
      if (response.ok || (response.status < 500 && response.status !== 429)) return response;
      lastError = new Error(`${label} HTTP ${response.status}`);
    } catch (error) {
      lastError = error;
    }
    if (attempt < 4) {
      const delayMs = 1500 * (2 ** (attempt - 1));
      process.stdout.write(`${label} failed, retrying in ${delayMs} ms (${attempt}/4)\n`);
      await new Promise((resolve) => setTimeout(resolve, delayMs));
    }
  }
  throw lastError || new Error(`${label} failed`);
}

async function fetchJson(url) {
  const headers = {
    Accept: "application/vnd.github+json",
    "User-Agent": "weather-clock-pages-deploy"
  };
  if (process.env.GH_API_TOKEN) headers.Authorization = `Bearer ${process.env.GH_API_TOKEN}`;
  const response = await fetchWithRetry(url, { headers }, "GitHub API request");
  if (!response.ok) throw new Error(`GitHub API ${response.status}: ${url}`);
  return response.json();
}

async function downloadVerifiedAsset(asset, destination) {
  const response = await fetchWithRetry(asset.downloadUrl, {
    headers: { "User-Agent": "weather-clock-pages-deploy" },
    redirect: "follow"
  }, `Firmware download ${asset.name}`);
  if (!response.ok) throw new Error(`Firmware download ${response.status}: ${asset.name}`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.byteLength !== asset.size) {
    throw new Error(`Firmware size mismatch: ${asset.name} ${bytes.byteLength} != ${asset.size}`);
  }
  const actualSha256 = createHash("sha256").update(bytes).digest("hex");
  if (actualSha256 !== asset.sha256) {
    throw new Error(`Firmware SHA256 mismatch: ${asset.name}`);
  }
  await writeFile(destination, bytes);
}

async function copyStaticSite() {
  await rm(OUTPUT_ROOT, { recursive: true, force: true });
  await mkdir(OUTPUT_ROOT, { recursive: true });
  for (const name of ["index.html", "app.js", "styles.css", "sw.js", "assets", "vendor", ".nojekyll"]) {
    await cp(path.join(SOURCE_ROOT, name), path.join(OUTPUT_ROOT, name), { recursive: true });
  }
  await mkdir(path.join(OUTPUT_ROOT, "firmware"), { recursive: true });
}

async function buildFirmwareMirror() {
  const releases = await fetchJson(`https://api.github.com/repos/${SOURCE_REPOSITORY}/releases?per_page=30`);
  if (!Array.isArray(releases)) throw new Error("GitHub Releases response is not an array");
  const candidates = releases
    .filter((release) => !release.draft && !release.prerelease)
    .map((release) => ({
      version: String(release.tag_name || "").trim(),
      notes: String(release.name || release.body || "").trim(),
      app: normalizeAsset(findReleaseAsset(release, false)),
      merged: normalizeAsset(findReleaseAsset(release, true))
    }))
    .filter((release) => /^v?[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$/.test(release.version) && release.app && release.merged)
    .slice(0, releaseLimit);
  if (candidates.length === 0) throw new Error("No complete GitHub Release firmware set found");

  const firmwareRoot = path.join(OUTPUT_ROOT, "firmware", "releases");
  await mkdir(firmwareRoot, { recursive: true });
  const items = [];
  for (const release of candidates) {
    const releaseRoot = path.join(firmwareRoot, release.version);
    await mkdir(releaseRoot, { recursive: true });
    for (const asset of [release.app, release.merged]) {
      process.stdout.write(`Mirroring ${release.version} ${asset.name}\n`);
      await downloadVerifiedAsset(asset, path.join(releaseRoot, asset.name));
    }
    const manifestAsset = (asset) => ({
      name: asset.name,
      url: `./firmware/releases/${release.version}/${asset.name}`,
      sha256: asset.sha256,
      size: asset.size
    });
    items.push({
      version: release.version,
      notes: release.notes,
      app: manifestAsset(release.app),
      merged: manifestAsset(release.merged)
    });
  }
  const manifest = {
    source: `https://github.com/${SOURCE_REPOSITORY}/releases`,
    generated_at: new Date().toISOString(),
    items
  };
  await writeFile(path.join(OUTPUT_ROOT, "firmware", "releases.json"), `${JSON.stringify(manifest, null, 2)}\n`);
}

await copyStaticSite();
await buildSdlPreviews(
  process.env.SDL_PREVIEW_SOURCE || path.resolve(SOURCE_ROOT, "../assets/previews"),
  OUTPUT_ROOT
);
await buildFirmwareMirror();
process.stdout.write(`Pages site ready: ${OUTPUT_ROOT}\n`);
