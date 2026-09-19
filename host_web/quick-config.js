import { tr, setText, setAttr, LocalizedError, getLanguage } from './i18n.js';
// 本地生成现有配网 GET 链接；不联网、不保存凭据、不自动导航。
const fields = {
  ssid: ['主 Wi-Fi 名称', 32], pass: ['主 Wi-Fi 密码', 64],
  backup_ssid: ['备用 Wi-Fi 名称', 32], backup_pass: ['备用 Wi-Fi 密码', 64],
  api_key: ['API Key', 95], api_host: ['API Host', 127],
  weather_city: ['天气城市', 31], manual_time: ['离线时间', 31],
};
const asciiTrim = value => value.replace(/^[\t\n\v\f\r ]+|[\t\n\v\f\r ]+$/g, '');

export function makeQuickConfigLink(input) {
  const values = Object.fromEntries(Object.keys(fields).map(key => [key, String(input[key] ?? '')]));
  for (const key of ['api_key', 'api_host', 'weather_city', 'manual_time']) values[key] = asciiTrim(values[key]);
  const offline = !asciiTrim(values.ssid);
  const warnings = [];
  let selected;
  if (offline) {
    const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(values.manual_time);
    if (!match) throw new LocalizedError(() => tr('联网时填写主 Wi-Fi 名称；离线使用时填写有效日期和时间。'));
    const [year, month, day, hour, minute, second] = match.slice(1).map(value => Number(value || 0));
    const date = new Date(Date.UTC(year, month - 1, day, hour, minute, second));
    if (year < 2024 || year > 2035 || date.getUTCFullYear() !== year || date.getUTCMonth() !== month - 1 ||
        date.getUTCDate() !== day || date.getUTCHours() !== hour || date.getUTCMinutes() !== minute || date.getUTCSeconds() !== second) {
      throw new LocalizedError(() => tr('离线时间必须是 2024–2035 年之间的有效日期和时间。'));
    }
    selected = { manual_time: values.manual_time };
    warnings.push(tr('此链接仅设置离线固定时间，不保存 Wi-Fi 或天气字段；使用前核对时间。'));
  } else {
    for (const key of ['pass', 'backup_pass']) {
      if (values[key] && new TextEncoder().encode(values[key]).length < 8) {
        throw new LocalizedError(() => tr`${tr(fields[key][0])}至少需要 8 字节（英文、数字或符号通常为 8 位）；请补全密码，或留空以沿用已有密码 / 连接开放网络。`);
      }
    }
    if (Boolean(values.api_key) !== Boolean(values.api_host)) {
      throw new LocalizedError(() => tr('API Key 和 API Host 必须同时填写，或同时留空以沿用设备已有配置。'));
    }
    if (values.backup_ssid && values.backup_ssid === values.ssid) throw new LocalizedError(() => tr('主 Wi-Fi 和备用 Wi-Fi 名称不能相同。'));
    if (values.backup_pass && !values.backup_ssid) throw new LocalizedError(() => tr('填写备用密码时必须同时填写备用 Wi-Fi 名称。'));
    const host = values.api_host.toLowerCase();
    if (host && (!host.endsWith('.qweatherapi.com') || !host.split('.').every(label =>
      /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label)))) {
      throw new LocalizedError(() => tr('API Host 必须是有效的专属 qweatherapi.com 子域名，不含协议或路径。'));
    }
    values.api_host = host;
    if (/[&=?#%/\\<>"'\x00-\x1f]/.test(values.weather_city)) throw new LocalizedError(() => tr('天气城市包含设备不支持的特殊字符。'));
    selected = Object.fromEntries(Object.entries(values).filter(([key]) => key !== 'manual_time'));
    if (!values.api_key) warnings.push(tr('API Key 和 API Host 均留空，将沿用设备已有天气配置；首次配置请同时填写。'));
    if (!values.pass) warnings.push(tr('Wi-Fi 密码留空，仅适用于开放网络或同名网络已有密码。'));
    if (!values.weather_city) warnings.push(tr('天气城市留空，设备将恢复 IP 自动定位。'));
    if (!values.backup_ssid) warnings.push(tr('备用 Wi-Fi 留空，设备将取消备用网络。'));
  }
  const params = new URLSearchParams();
  for (const [key, value] of Object.entries(selected)) {
    const [label, limit] = fields[key];
    if (/[\x00-\x1f\x7f]/.test(value)) throw new LocalizedError(() => tr`${tr(label)}不能包含控制字符。`);
    if (new TextEncoder().encode(value).length > limit) throw new LocalizedError(() => tr`${tr(label)}超过设备的 ${limit} 字节限制。`);
    const encoded = new URLSearchParams({ v: value }).toString().slice(2);
    if (encoded.length >= 160) throw new LocalizedError(() => tr`${tr(label)}编码后过长，设备会截断该字段，请缩短内容。`);
    if (value) params.set(key, value);
  }
  const url = new URL('http://192.168.4.1/save');
  url.search = params.toString();
  const uriBytes = url.pathname.length + url.search.length;
  if (uriBytes > 512) throw new LocalizedError(() => tr('配置链接超过固件 512 字节请求地址限制，请减少选填内容或改用设备配网页填写。'));
  return { url: url.href, uriBytes, warnings, offline };
}

if (typeof document !== 'undefined') {
  const form = document.getElementById('quickConfigForm');
  const result = document.getElementById('quickResult');
  const status = document.getElementById('quickStatus');
  const copy = document.getElementById('quickCopy');
  const passwordButtons = [...form.querySelectorAll('[data-password]')];
  function showPassword(button, visible) {
    document.getElementById(button.dataset.password).type = visible ? 'text' : 'password';
    button.setAttribute('aria-pressed', String(visible));
    const label = () => tr(visible ? '隐藏{0}' : '显示{0}', tr(button.dataset.label));
    setAttr(button, 'aria-label', label);
    setAttr(button, 'title', label);
  }
  passwordButtons.forEach(button => button.addEventListener('click', () => {
    showPassword(button, document.getElementById(button.dataset.password).type === 'password');
  }));
  function invalidate(message = '信息已修改，请重新生成链接。') {
    result.value = ''; copy.disabled = true; setText(status, () => tr(message));
  }
  form.addEventListener('input', event => { if (event.target !== result) invalidate(); });
  form.addEventListener('submit', event => {
    event.preventDefault();
    invalidate();
    try {
      const input = Object.fromEntries(new FormData(form));
      const link = makeQuickConfigLink(input);
      result.value = link.url; copy.disabled = false;
      setText(status, () => tr`已生成（请求地址 ${link.uriBytes}/512 字节）。先连接设备配网热点，再在浏览器访问；访问即提交配置。${makeQuickConfigLink(input).warnings.join(' ')}`);
    } catch (error) { setText(status, () => error.message); }
  });
  copy.addEventListener('click', async () => {
    const link = result.value;
    if (!link) return;
    try {
      await navigator.clipboard.writeText(link);
      if (result.value === link) setText(status, () => tr('已复制含明文凭据的链接，请妥善保管。先连接设备配网热点，再在浏览器访问；访问即提交配置。'));
    } catch {
      if (result.value === link) { result.focus(); result.select(); setText(status, () => tr('无法自动复制，链接已选中，请手动复制并妥善保管。')); }
    }
  });
  form.addEventListener('reset', () => {
    invalidate('已清空本页信息；此前复制的链接仍可能保留在剪贴板或历史中。');
    passwordButtons.forEach(button => showPassword(button, true));
  });
  window.addEventListener('pagehide', () => form.reset());
  window.addEventListener('pageshow', event => { if (event.persisted) form.reset(); });
  document.getElementById('quickGenerate').disabled = false;
}
