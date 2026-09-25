// Keep the visible Host Web version, runtime version source and offline cache revision in sync.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const root = new URL('../', import.meta.url);
const [html, app, serviceWorker, readme] = await Promise.all([
  readFile(new URL('index.html', root), 'utf8'),
  readFile(new URL('app.js', root), 'utf8'),
  readFile(new URL('sw.js', root), 'utf8'),
  readFile(new URL('README.md', root), 'utf8')
]);
const version = app.match(/const HOST_WEB_VERSION = "([^"]+)"/)?.[1];
assert.equal(version, 'v1.0.4');
assert.match(html, new RegExp(`id="hostVersion">${version.replaceAll('.', '\\.')}`));
assert.match(readme, new RegExp(`网页 ${version.replaceAll('.', '\\.')}`));
assert.match(serviceWorker, /CACHE_NAME = .*v78-/);
console.log(`Host Web version ${version}, display marker and cache revision are synchronized.`);
