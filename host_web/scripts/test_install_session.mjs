import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { tr } from '../i18n.js';
const source = readFileSync(new URL('../app.js', import.meta.url), 'utf8');
const fn = source.slice(source.indexOf('async function writeBinaryWithEsptool('), source.indexOf('\nasync function writeAssets('));
for (const fail of [false, true]) {
  const events = [];
  const session = {
    transport: { disconnect: async () => events.push('close') },
    loader: { main: async () => { throw Error('Unexpected reconnect'); }, writeFlash: async options => {
      assert.equal(options.flashMode, 'keep');
      assert.equal(options.flashFreq, 'keep');
      assert.equal(options.fileArray[0].address, 0);
      assert.equal(options.fileArray[0].data, '\x01\x02');
      events.push('write');
      if (fail) throw Error('USB disconnected');
    } }
  };
  const c = vm.createContext({ tr, Uint8Array, console, navigator: { serial: {} },
    importEsptool: async () => ({}), $: () => ({}), setText() {}, describePort: () => 'test',
    uint8ArrayToBinaryString: bytes => String.fromCharCode(...bytes),
    resetDeviceAfterFlash: async () => events.push('reset'), firmwareSession: session
  });
  vm.runInContext(fn, c);
  const operation = c.writeBinaryWithEsptool({ data: new Uint8Array([1, 2]), offset: 0, devicePort: {}, log() {}, session });
  if (fail) await assert.rejects(operation, /USB disconnected/); else await operation;
  assert.deepEqual(events, ['write', 'reset', 'close']);
}
console.log('Same-session write and success/failure cleanup passed.');
