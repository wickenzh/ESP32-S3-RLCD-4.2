// Contract checks for the simplified full-install and advanced App-update UI.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const app = readFileSync(new URL('../app.js', import.meta.url), 'utf8');
const tabOrder = [...html.matchAll(/data-tab="([^"]+)"/g)].map(match => match[1]);
assert.deepEqual(tabOrder, ['firmware', 'assets', 'writer', 'serial', 'screens', 'settings']);
for (const id of ['firmwareInstallConnectBtn', 'firmwareInstallBtn', 'firmwareInstallConfirm', 'firmwareInstallConfirmBtn', 'firmwareInstallCancelBtn', 'firmwareInstallProgress', 'firmwareInstallNotesLink']) {
  assert.match(html, new RegExp(`id="${id}"`), id);
}
assert.match(html, /id="firmwareTarget"[\s\S]*?value="merged"/);
assert.match(html, /在线完整安装/);
assert.doesNotMatch(html, /<summary>高级固件烧录/);
assert.match(html, /firmwareInstallNotes/);
assert.match(html, /id="hostVersion">v1\.0\.5/);
assert.match(app, /const HOST_WEB_VERSION = "v1.0.5"/);
assert.match(html, /firmware-install-layout/);
assert.match(html, /firmware-notes-panel/);
assert.match(app, /firmwareInstallBusy/);
assert.match(app, /firmwareChipVerified/);
assert.match(app, /firmwareFlashSizeBytes/);
assert.match(app, /firmwareInstallSnapshot/);
assert.match(app, /firmwareInstallConfirmBtn/);
assert.match(app, /getFlashSize/);
assert.match(app, /!firmwareChipVerified \|\| !firmwareFlashSizeBytes/);
assert.match(app, /eraseAll: false/);
assert.doesNotMatch(app, /eraseAll:\s*true/);
assert.match(app, /return true;/);
assert.match(app, /return false;/);
assert.match(app, /summarizeFirmwareNotes/);
assert.match(app, /formatFirmwareNotes/);
assert.match(app, /releaseUrl/);
console.log('Firmware install UI order, confirmation, chip/flash guards, busy state and no-erase contract passed.');
