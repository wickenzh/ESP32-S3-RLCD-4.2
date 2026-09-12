// 将权威SDL快照放入部署产物，并按图片内容更新缓存标识。
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

export async function buildSdlPreviews(sourceDirectory, outputDirectory) {
  const html = await readFile(path.join(outputDirectory, 'index.html'), 'utf8');
  const names = [...html.matchAll(/src="\.\/assets\/screens\/(weather_clock_[a-z_]+\.png)"/g)].map(match => match[1]);
  if (names.length !== 8 || new Set(names).size !== 8) throw new Error('Expected eight unique SDL preview images');
  const images = [];
  const hash = createHash('sha256');
  for (const name of names) {
    const bytes = await readFile(path.join(sourceDirectory, name));
    if (bytes.length < 24 || bytes.subarray(0, 8).toString('hex') !== '89504e470d0a1a0a' ||
        bytes.readUInt32BE(16) !== 400 || bytes.readUInt32BE(20) !== 300) {
      throw new Error(`Invalid 400x300 SDL preview: ${name}`);
    }
    hash.update(name).update(bytes);
    images.push({ name, bytes });
  }
  const revision = hash.digest('hex');
  const destination = path.join(outputDirectory, 'assets/screens');
  await mkdir(destination, { recursive: true });
  for (const image of images) await writeFile(path.join(destination, image.name), image.bytes);
  const workerPath = path.join(outputDirectory, 'sw.js');
  const worker = await readFile(workerPath, 'utf8');
  const marker = 'const SDL_PREVIEW_REVISION = "local";';
  if (!worker.includes(marker)) throw new Error('Missing SDL cache revision marker');
  await writeFile(workerPath, worker.replace(marker, `const SDL_PREVIEW_REVISION = "${revision}";`));
  return revision;
}
