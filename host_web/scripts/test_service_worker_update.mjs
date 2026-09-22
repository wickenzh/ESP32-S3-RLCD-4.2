// Regression test for stale navigation responses after a Service Worker update.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const sw = await readFile(new URL('../sw.js', import.meta.url), 'utf8');
const app = await readFile(new URL('../app.js', import.meta.url), 'utf8');
const handlers = {};
const cachedIndex = new Response('old cached index');
const networkIndex = new Response('new network index');
const context = {
  URL,
  Response,
  Promise,
  fetch: async () => networkIndex.clone(),
  self: {
    location: { origin: 'https://example.test', pathname: '/ESP32-S3-RLCD-4.2/sw.js' },
    addEventListener: (name, handler) => { handlers[name] = handler; },
    clients: { claim() {} },
    skipWaiting() {}
  },
  caches: {
    match: async () => cachedIndex.clone(),
    open: async () => ({ put: async () => {} }),
    keys: async () => []
  }
};
vm.runInNewContext(sw, context);
let responsePromise;
handlers.fetch({
  request: { method: 'GET', mode: 'navigate', url: 'https://example.test/ESP32-S3-RLCD-4.2/' },
  respondWith: (promise) => { responsePromise = promise; }
});
assert.equal(await (await responsePromise).text(), 'new network index');
assert.match(app, /navigator\.serviceWorker\.addEventListener\("controllerchange"/);
assert.match(app, /registration\.update\(\)/);
console.log('Service Worker navigation update and controller refresh contract passed.');
