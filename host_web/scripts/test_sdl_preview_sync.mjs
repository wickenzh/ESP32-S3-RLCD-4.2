// 验证上游快照变化传播到部署图片和缓存版本，缺图时禁止回退旧副本。
import assert from 'node:assert/strict';
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildSdlPreviews } from './build_sdl_previews.mjs';

const host = fileURLToPath(new URL('../', import.meta.url));
const temp = await mkdtemp(path.join(os.tmpdir(), 'sdl-sync-'));
const source = path.join(temp, 'previews');
const output = path.join(temp, 'site');
try {
  await cp(path.join(host, 'assets/screens'), source, { recursive: true });
  await mkdir(output);
  await cp(path.join(host, 'index.html'), path.join(output, 'index.html'));
  const resetWorker = () => cp(path.join(host, 'sw.js'), path.join(output, 'sw.js'));
  await resetWorker();
  const first = await buildSdlPreviews(source, output);
  const replacement = await readFile(path.join(source, 'weather_clock_gallery.png'));
  await writeFile(path.join(source, 'weather_clock_main.png'), replacement);
  await resetWorker();
  const second = await buildSdlPreviews(source, output);
  assert.notEqual(first, second);
  assert.deepEqual(await readFile(path.join(output, 'assets/screens/weather_clock_main.png')), replacement);
  assert((await readFile(path.join(output, 'sw.js'), 'utf8')).includes(second));
  await resetWorker();
  assert.equal(await buildSdlPreviews(source, output), second);
  await rm(path.join(source, 'weather_clock_main.png'));
  await resetWorker();
  await assert.rejects(buildSdlPreviews(source, output), { code: 'ENOENT' });
  await writeFile(path.join(source, 'weather_clock_main.png'), 'not a PNG');
  await assert.rejects(buildSdlPreviews(source, output), /Invalid 400x300/);
  console.log('SDL upstream update, cache revision and missing/corrupt image tests passed');
} finally {
  await rm(temp, { recursive: true, force: true });
}
