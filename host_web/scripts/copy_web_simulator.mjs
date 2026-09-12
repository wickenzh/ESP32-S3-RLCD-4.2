// 核对WASM构建产物完整性并绑定部署缓存，防止混合不同构建文件。
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function copyWebSimulator(source, output) {
  const manifestBytes = await readFile(path.join(source, 'build-info.json'));
  const manifest = JSON.parse(manifestBytes);
  if (!manifest.firmwareVersion || !/^[0-9a-f]{40}$/.test(manifest.sourceCommit) || !/^[0-9a-f]{64}$/.test(manifest.sourceDigest)) throw new Error('Invalid simulator build identity');
  const files = ['weather-clock.js', 'weather-clock.wasm', 'portal.html'];
  const assets = [];
  for (const name of files) {
    const bytes = await readFile(path.join(source, name));
    if (createHash('sha256').update(bytes).digest('hex') !== manifest.artifacts?.[name]) throw new Error(`Simulator artifact mismatch: ${name}`);
    assets.push({ name, bytes });
  }
  const folder = path.join(output, 'simulator');
  await mkdir(folder, { recursive: true });
  for (const file of assets) await writeFile(path.join(folder, file.name), file.bytes);
  await writeFile(path.join(folder, 'build-info.json'), manifestBytes);
  const revision = createHash('sha256').update(manifestBytes).digest('hex');
  const workerPath = path.join(output, 'sw.js');
  const worker = await readFile(workerPath, 'utf8');
  if (!worker.includes('const SIMULATOR_REVISION = "local";') || !worker.includes('const SIMULATOR_ASSETS = [];')) throw new Error('Missing simulator cache markers');
  await writeFile(workerPath, worker
    .replace('const SIMULATOR_REVISION = "local";', `const SIMULATOR_REVISION = "${revision}";`)
    .replace('const SIMULATOR_ASSETS = [];', `const SIMULATOR_ASSETS = ${JSON.stringify([...files, 'build-info.json'].map(name => './simulator/' + name))};`));
}
