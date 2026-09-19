// 验证真实 GIF 预览函数的类型边界、字节保留与异步覆盖保护。
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { tr, LocalizedError } from '../i18n.js';

const source = await readFile(new URL('../app.js', import.meta.url), 'utf8');
const start = source.indexOf('async function setGifOriginalPreview(');
const end = source.indexOf('\nfunction nowText(', start);
assert(start >= 0 && end > start);
class ImageElement {
  removeAttribute(name) { if (name === 'src') this.src = undefined; }
}
const image = new ImageElement();
let element = image;
const created = [];
const revoked = [];
const context = vm.createContext({
  tr, LocalizedError,
  Uint8Array,
  HTMLImageElement: ImageElement,
  document: { getElementById: () => element },
  URL: {
    createObjectURL(blob) { created.push(blob); return `blob:test-${created.length}`; },
    revokeObjectURL(url) { revoked.push(url); }
  }
});
vm.runInContext(`let gifOriginalUrl; let gifPreviewRequest = 0;\n${source.slice(start, end)}`, context);
const preview = context.setGifOriginalPreview;
const bytes = Buffer.from('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7', 'base64');
assert.equal(await preview(new Blob([bytes], { type: 'text/html' })), true);
assert.equal(created[0].type, 'image/gif');
assert.deepEqual(Buffer.from(await created[0].arrayBuffer()), bytes);
assert.equal(image.src, 'blob:test-1');
await assert.rejects(preview(new Blob(['<html>not a GIF</html>'], { type: 'image/gif' })), /有效的 GIF/);
assert.equal(image.src, undefined);
assert.deepEqual(revoked, ['blob:test-1']);
assert.equal(created.length, 1);
await assert.rejects(preview(new Blob(['GIF'])), /有效的 GIF/);
element = {};
await assert.rejects(preview(new Blob([bytes])), /图片元素不可用/);
element = image;
const gif87 = Buffer.from(bytes);
gif87.write('GIF87a', 0);
assert.equal(await preview(new Blob([gif87])), true);
let finishHeader;
const slowFile = { slice: () => ({ arrayBuffer: () => new Promise(resolve => { finishHeader = resolve; }) }) };
const slow = preview(slowFile);
await preview(new Blob([bytes]));
const latest = image.src;
finishHeader(bytes.subarray(0, 6));
assert.equal(await slow, false);
assert.equal(image.src, latest);
console.log('GIF preview boundaries, unchanged bytes and stale request tests passed');
