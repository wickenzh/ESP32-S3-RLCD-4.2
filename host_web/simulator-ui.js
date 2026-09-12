// 按需加载真实WASM模拟器，转发虚拟按键，控制显示生命周期与构建身份。
const panel = document.getElementById('screens');
const canvas = document.getElementById('simulatorCanvas');
const status = document.getElementById('simulatorStatus');
const build = document.getElementById('simulatorBuildInfo');
const controls = [...document.querySelectorAll('.virtual-device button, .virtual-device select')];
let runtime;
let loading;
let frame;
let lastFrame = 0;
let lastState = '';
let failed = false;
const heldKeys = new Map();
const running = () => panel.classList.contains('is-active') && !document.hidden;
const sceneNames = ['工作页面', '设置', '页面开关', '页面顺序', '关于本机', '网络检测', '检查更新', '配网提示', '天气预警', '低电量', '启动页'];
const pageNames = ['天气时钟', '图片时钟', '天气看板', '温湿时钟', '日历', '温湿历史', '小智 AI', '聚合时钟'];

function updateStatus() {
  const raw = runtime.UTF8ToString(runtime._demo_state());
  if (raw === lastState) return;
  lastState = raw;
  const state = JSON.parse(raw);
  document.querySelector('.virtual-device').dataset.simState = raw;
  status.textContent = `${state.scene === 0 ? pageNames[state.page] : sceneNames[state.scene]} · ${state.offline ? '离线状态（模拟）' : '已配网（模拟）'}`;
  if (state.scene === 0) document.getElementById('simScene').value = String(state.page);
}

function tick(now) {
  frame = undefined;
  if (!runtime || !running() || failed) return;
  if (now - lastFrame >= 32) {
    try {
      runtime._demo_tick(Date.now() / 1000, now);
      updateStatus();
    } catch {
      failed = true; status.textContent = '模拟器运行异常，请刷新重试；静态预览仍可查看。';
      controls.forEach(control => { control.disabled = true; });
      return;
    }
    lastFrame = now;
  }
  frame = requestAnimationFrame(tick);
}

async function load() {
  if (loading) return loading;
  loading = (async () => {
    // Load the trusted build artifact through the parent service worker, then
    // keep all form code isolated in the opaque-origin sandbox, also offline.
    fetch('./simulator/portal.html').then(response => {
      if (!response.ok) throw new Error('Portal artifact unavailable');
      return response.text();
    }).then(html => { document.getElementById('portalSimulator').srcdoc = html; })
      .catch(() => { document.querySelector('.virtual-portal .sim-section-title span').textContent = '虚拟配网加载失败，请联网刷新重试'; });
    try {
      const { default: create } = await import('./simulator/weather-clock.js');
      const title = document.title;
      runtime = await create({ canvas, print: () => {}, printErr: text => console.warn(text), onAbort: () => { failed = true; } });
      document.title = title;
      const info = JSON.parse(runtime.UTF8ToString(runtime._demo_build_info()));
      build.textContent = `基于固件 ${info.firmwareVersion} · 源码 ${info.sourceCommit.slice(0, 8)}${info.sourceDirty ? '（本地未提交构建）' : ''} · 内容 ${info.sourceDigest.slice(0, 12)} · 构建于 ${new Date(info.builtAt).toLocaleString()}`;
      build.title = `源码 ${info.sourceCommit}\n内容 ${info.sourceDigest}\n${info.engine}`;
      controls.forEach(control => { control.disabled = false; });
      if (running()) frame = requestAnimationFrame(tick);
    } catch {
      failed = true;
      status.textContent = '交互模拟器暂不可用，可展开静态预览。';
      build.textContent = '未加载到模拟器构建信息';
      document.querySelector('.screen-snapshots').open = true;
    }
  })();
  return loading;
}

function refreshVisibility() {
  cancelHeldKeys();
  if (!running()) { cancelAnimationFrame(frame); frame = undefined; return; }
  if (!runtime) { load(); return; }
  if (!frame && !failed) frame = requestAnimationFrame(tick);
}

function press(key, long) {
  if (!runtime || failed || !running()) return;
  runtime._demo_key(key, long ? 1 : 0); updateStatus();
}
function cancelKey(key) {
  const held = heldKeys.get(key);
  if (held) clearTimeout(held.timer);
  heldKeys.delete(key);
}
function cancelHeldKeys() { for (const key of heldKeys.keys()) cancelKey(key); }
function begin(key) {
  if (!runtime || failed || !running() || heldKeys.has(key)) return;
  const held = { start: performance.now(), fired: false, timer: undefined };
  held.timer = setTimeout(() => {
    if (heldKeys.get(key) !== held) return;
    held.fired = true;
    press(key, true);
  }, 1200);
  heldKeys.set(key, held);
}
function end(key) {
  const held = heldKeys.get(key);
  if (!held) return;
  cancelKey(key);
  if (!held.fired) press(key, performance.now() - held.start >= 1200);
}
for (const [id, key] of [['simBoot', 0], ['simKey', 1]]) {
  const button = document.getElementById(id);
  button.addEventListener('pointerdown', event => { if (button.disabled || event.button !== 0 || !event.isPrimary) return; begin(key); button.setPointerCapture(event.pointerId); });
  button.addEventListener('pointerup', () => end(key));
  button.addEventListener('pointercancel', () => cancelKey(key));
  button.addEventListener('lostpointercapture', () => cancelKey(key));
  button.addEventListener('contextmenu', event => event.preventDefault());
  button.addEventListener('click', event => { if (event.detail === 0) press(key, false); });
}
const device = document.querySelector('.virtual-device');
device.addEventListener('keydown', event => {
  if (event.target.matches('input,select,textarea')) return;
  const key = event.code === 'KeyB' ? 0 : event.code === 'KeyK' ? 1 : -1;
  if (key >= 0) { event.preventDefault(); begin(key); }
});
device.addEventListener('keyup', event => { if (event.code === 'KeyB') end(0); if (event.code === 'KeyK') end(1); });
window.addEventListener('blur', cancelHeldKeys);
device.addEventListener('focusout', event => { if (!device.contains(event.relatedTarget)) cancelHeldKeys(); });
document.getElementById('simReset').addEventListener('click', () => { runtime._demo_reset(); document.getElementById('simWeather').value = '20'; updateStatus(); });
for (const id of ['simScene', 'simWeather']) document.getElementById(id).addEventListener('change', event => { runtime._demo_scene(Number(event.target.value)); updateStatus(); });
document.getElementById('simDialogue').addEventListener('click', () => { runtime._demo_scene(30); updateStatus(); });
document.getElementById('simPomodoro').addEventListener('click', () => { runtime._demo_scene(31); updateStatus(); });
window.addEventListener('host-tab-change', refreshVisibility);
document.addEventListener('visibilitychange', refreshVisibility);
refreshVisibility();
