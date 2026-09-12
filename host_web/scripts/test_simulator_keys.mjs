// 验证虚拟按键长按、释放和取消事件不会重复或意外触发。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../simulator-ui.js', import.meta.url), 'utf8');
const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
assert.ok(html.indexOf('id="simKey"') < html.indexOf('id="simBoot"'));
let now = 0;
const timers = new Map();
const calls = [];
const target = () => ({
  listeners: {}, disabled: false,
  addEventListener(name, handler) { this.listeners[name] = handler; },
  setPointerCapture() {}, contains() { return false; },
});
const buttons = { simBoot: target(), simKey: target() };
const device = target();
const window = target();
const context = vm.createContext({
  runtime: {}, failed: false, running: () => true, heldKeys: new Map(),
  performance: { now: () => now },
  setTimeout(fn, delay) { const id = {}; timers.set(id, { fn, at: now + delay }); return id; },
  clearTimeout(id) { timers.delete(id); },
  press: (key, long) => calls.push([key, long]), window,
  document: { getElementById: id => buttons[id], querySelector: () => device },
});
vm.runInContext(source.slice(source.indexOf('function cancelKey('), source.indexOf("document.getElementById('simReset').addEventListener")), context);
const advance = ms => {
  now += ms;
  for (const [id, timer] of timers) if (timer.at <= now) { timers.delete(id); timer.fn(); }
};
const down = (button = buttons.simKey) => button.listeners.pointerdown({ button: 0, isPrimary: true, pointerId: 1 });
down(); advance(1199); assert.equal(calls.length, 0);
advance(1); assert.deepEqual(calls, [[1, true]]);
advance(5000); buttons.simKey.listeners.pointerup();
assert.equal(calls.length, 1, 'holding and releasing must not repeat long press or emit short press');
down(); advance(100); buttons.simKey.listeners.pointerup();
assert.deepEqual(calls.at(-1), [1, false]);
for (const cancel of ['pointercancel', 'lostpointercapture']) {
  down(); buttons.simKey.listeners[cancel](); advance(1300); buttons.simKey.listeners.pointerup();
}
down(); window.listeners.blur(); advance(1300);
assert.equal(calls.length, 2, 'cancelled input must not trigger actions');
down(buttons.simBoot); advance(100); buttons.simBoot.listeners.pointerup();
assert.deepEqual(calls.at(-1), [0, false]);
const keyEvent = { target: { matches: () => false }, code: 'KeyK', preventDefault() {} };
device.listeners.keydown(keyEvent); advance(600); device.listeners.keydown(keyEvent);
advance(600); assert.deepEqual(calls.at(-1), [1, true]);
device.listeners.keyup(keyEvent); advance(2000);
assert.equal(calls.length, 4, 'keyboard repeat and keyup must not repeat long press');
console.log('Simulator key order, short/long press, cancellation and keyboard tests passed.');
