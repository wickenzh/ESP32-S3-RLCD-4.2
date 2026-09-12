// 验证模拟器构建身份与产物哈希绑定，拒绝部分旧文件混入部署。
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { cp, mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { copyWebSimulator } from './copy_web_simulator.mjs';
const temp = await mkdtemp(path.join(os.tmpdir(), 'sim-artifact-'));
try {
  const source = path.join(temp, 'source'), output = path.join(temp, 'site');
  await mkdir(source); await mkdir(output);
  const artifacts = {};
  for (const name of ['weather-clock.js', 'weather-clock.wasm', 'portal.html']) {
    const bytes = Buffer.from(name);
    artifacts[name] = createHash('sha256').update(bytes).digest('hex');
    await writeFile(path.join(source, name), bytes);
  }
  const meta = { firmwareVersion: 'v1.6.4', sourceCommit: 'a'.repeat(40), sourceDigest: 'b'.repeat(64), builtAt: '2026-01-01T00:00:00Z', artifacts };
  await writeFile(path.join(source, 'build-info.json'), JSON.stringify(meta));
  await cp(new URL('../sw.js', import.meta.url), path.join(output, 'sw.js'));
  await copyWebSimulator(source, output);
  const before = await readFile(path.join(output, 'sw.js'), 'utf8');
  assert(before.includes('./simulator/weather-clock.wasm'));
  assert(!before.includes('const SIMULATOR_REVISION = "local";'));
  await writeFile(path.join(source, 'weather-clock.wasm'), 'old-build');
  await assert.rejects(copyWebSimulator(source, output), /artifact mismatch/);
  assert.equal(await readFile(path.join(output, 'sw.js'), 'utf8'), before);
  console.log('Simulator artifact hash and deployment cache tests passed');
} finally { await rm(temp, { recursive: true, force: true }); }
