// 验证 Pages 同源镜像、哈希失败关闭和 Service Worker 缓存隔离。
import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { createHash } from 'node:crypto';

const directory = await mkdtemp(path.join(os.tmpdir(), 'clock-pages-test-'));
const bytes = new Uint8Array([1, 2, 3, 4]);
const digest = createHash('sha256').update(bytes).digest('hex');
let badHash = false;
globalThis.fetch = async (url) => {
  const parsed = new URL(url);
  if (parsed.protocol !== 'https:' || parsed.username || parsed.password || parsed.port) {
    throw new Error('Unexpected mock request URL');
  }
  if (parsed.hostname === 'api.github.com' && parsed.pathname === '/repos/wickenzh/ESP32-S3-RLCD-4.2/releases') {
    const releases = Array.from({ length: 12 }, (_, i) => ({
      tag_name: `v1.0.${12 - i}`, draft: false, prerelease: i === 0,
      assets: ['', '_merged'].map(suffix => ({
        name: `weather_clock_v1.0.${12 - i}${suffix}.bin`, size: bytes.length,
        digest: `sha256:${badHash ? '0'.repeat(64) : digest}`,
        browser_download_url: `https://github.com/wickenzh/ESP32-S3-RLCD-4.2/releases/download/v1.0.${12 - i}/firmware${suffix}.bin`
      }))
    }));
    return Response.json(releases);
  }
  if (parsed.hostname === 'github.com' && parsed.pathname.startsWith('/wickenzh/ESP32-S3-RLCD-4.2/releases/download/')) {
    return new Response(bytes);
  }
  throw new Error('Unexpected mock request URL');
};
try {
  const simulator = path.join(directory, 'simulator');
  await mkdir(simulator);
  const artifacts = {};
  for (const name of ['weather-clock.js', 'weather-clock.wasm', 'portal.html']) {
    await writeFile(path.join(simulator, name), bytes);
    artifacts[name] = digest;
  }
  process.env.SIMULATOR_BUILD_DIR = simulator;
  await writeFile(path.join(simulator, 'build-info.json'), JSON.stringify({ firmwareVersion: 'v1.0.0', sourceCommit: 'a'.repeat(40), sourceDigest: 'b'.repeat(64), artifacts }));
  for (const url of [
    'https://api.github.com.example.invalid/releases',
    'https://example.invalid/api.github.com',
    'https://example.invalid/?host=api.github.com',
    'http://api.github.com/repos/wickenzh/ESP32-S3-RLCD-4.2/releases',
    'https://api.github.com@other.invalid/releases'
  ]) {
    await assert.rejects(fetch(url), /Unexpected mock request URL/);
  }
  process.argv[2] = path.join(directory, 'site');
  await import('./build_pages_site.mjs?valid');
  const manifest = JSON.parse(await readFile(path.join(process.argv[2], 'firmware/releases.json')));
  assert.equal(manifest.items.length, 10);
  assert.equal(manifest.items[0].version, 'v1.0.11');
  assert.equal(manifest.items[0].app.sha256, digest);
  assert(!(await readdir(process.argv[2])).includes('scripts'));
  assert(!(await readdir(process.argv[2])).includes('AI_HOST_WEB_GUIDE.md'));
  const html = await readFile(path.join(process.argv[2], 'index.html'), 'utf8');
  const previews = [...html.matchAll(/src="(\.\/assets\/screens\/[^"?]+\.png)"/g)].map(match => match[1]);
  assert.equal(new Set(previews).size, 8);
  const sw = await readFile(path.join(process.argv[2], 'sw.js'), 'utf8');
  for (const name of [...Object.keys(artifacts), 'build-info.json']) {
    assert(sw.includes(`./simulator/${name}`));
    assert((await readFile(path.join(process.argv[2], 'simulator', name))).length);
  }
  for (const preview of previews) {
    const image = await readFile(path.join(process.argv[2], preview));
    assert.equal(image.subarray(1, 4).toString(), 'PNG');
    assert(sw.includes(`"${preview}"`), `Preview absent from offline cache: ${preview}`);
  }
  badHash = true;
  process.argv[2] = path.join(directory, 'failed-site');
  await assert.rejects(import('./build_pages_site.mjs?bad'), /SHA256 mismatch/);

  const handlers = {};
  const deleted = [];
  const prefix = 'weather-clock-unified:/ESP32-S3-RLCD-4.2/sw.js:';
  const context = { self: { location: { pathname: '/ESP32-S3-RLCD-4.2/sw.js' },
    addEventListener: (name, handler) => { handlers[name] = handler; }, clients: { claim() {} } },
    caches: { keys: async () => ['weather-clock-host-v43', prefix + 'old', vm.runInNewContext('CACHE_NAME', context)],
      delete: async key => { deleted.push(key); } } };
  vm.runInNewContext(await readFile(new URL('../sw.js', import.meta.url), 'utf8'), context);
  let complete;
  handlers.activate({ waitUntil: promise => { complete = promise; } });
  await complete;
  assert.deepEqual(deleted, [prefix + 'old']);
  console.log('Pages mirror and cache isolation tests passed');
} finally {
  delete process.env.SIMULATOR_BUILD_DIR;
  await rm(directory, { recursive: true, force: true });
}
