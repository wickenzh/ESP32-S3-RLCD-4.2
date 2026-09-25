import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { ESPLoader } from '../vendor/esptool-js/0.5.6/bundle.js';
import { tr, LocalizedError } from '../i18n.js';

const source = readFileSync(new URL('../app.js', import.meta.url), 'utf8');
function extract(name, next) {
  return source.slice(source.indexOf(`async function ${name}(`), source.indexOf(`\nfunction ${next}(`));
}
for (const failure of ['', 'flash', 'connect']) {
  const events = [];
  const elements = new Map();
  class Loader {
    constructor() {
      this.chip = { readMac: async () => 'test' };
      this.DETECTED_FLASH_SIZES_NUM = { 24: 16 * 1024 * 1024 };
    }
    async main() { events.push('connect'); if (failure === 'connect') throw Error('connect failed'); return 'ESP32-S3'; }
    async readFlashId() { events.push('flash-id'); if (failure === 'flash') throw Error('flash failed'); return 0x1840ef; }
    debug() {}
    async readFlash() { events.push('partition'); return new Uint8Array(4096); }
  }
  // Exercise the bundled library implementation, not an invented chip API.
  Loader.prototype.getFlashSize = ESPLoader.prototype.getFlashSize;
  const context = vm.createContext({
    tr, LocalizedError, console: { info() {}, warn() {} },
    navigator: { serial: { requestPort: async () => ({}) } },
    $: id => { if (!elements.has(id)) elements.set(id, { value: '115200' }); return elements.get(id); },
    setText: (element, render) => { element.textContent = render(); },
    setProgress() {}, describePort: () => 'USB test',
    importEsptool: async () => ({ ESPLoader: Loader, Transport: class { async disconnect() { events.push('disconnect'); } } }),
    normalizeFlashCapacity: value => value,
    formatBytes: String, selectedFirmwareSize: () => 0,
    parsePartitionTable: () => [], renderPartitionTable() {}, renderFirmwareTargets() {},
    findOtaPartition() {}, updateFirmwareWriteButton() {}, updateFirmwareInstallSummary() {}, hintNextStep() {},
    resetDeviceAfterFlash: async () => { events.push('reset'); },
    PARTITION_TABLE_OFFSET: 0x8000, PARTITION_TABLE_SIZE: 4096
  });
  vm.runInContext('let firmwareDevicePort, firmwarePartitions, firmwareChipVerified, firmwareFlashSizeBytes, firmwareFlashSizeText;\n' + extract('inspectFirmwareDevice', 'showFirmwareOptionMessage'), context);
  await context.inspectFirmwareDevice();
  assert.deepEqual(events.slice(-2), ['reset', 'disconnect']);
  assert.equal(events.includes('partition'), !failure);
  if (failure) assert.match(elements.get('#flashResult').textContent, /failed/);
  else assert.equal(vm.runInContext('firmwareFlashSizeBytes', context), 16 * 1024 * 1024);
}
console.log('Bundled Flash ID capacity, partition read, error preservation and reset cleanup passed.');
