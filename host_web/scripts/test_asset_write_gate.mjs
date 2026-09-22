// Contract checks for the resource-writing prerequisite gate.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const app = readFileSync(new URL('../app.js', import.meta.url), 'utf8');
assert.match(html, /id="writerLockedNotice"/);
assert.match(html, /id="goToAssetsFromWriterBtn"/);
assert.match(html, /id="assetBaudRate" disabled/);
assert.match(html, /id="selectAssetDeviceBtn"[^>]*disabled/);
assert.match(html, /资源包尚未生成/);
assert.match(app, /const hasPackage = Boolean\(generatedAssetPackage\)/);
assert.match(app, /\$\("#writerLockedNotice"\)\.hidden = hasPackage/);
assert.match(app, /\$\("#selectAssetDeviceBtn"\)\.disabled = !\(hasSerial && hasPackage\)/);
assert.match(app, /goToAssetsFromWriterBtn/);
console.log('Resource writer prerequisite gate and guidance contract passed.');
