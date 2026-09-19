// Four-language completeness, interpolation, reactive messages and protocol isolation.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { staticMessages } from '../locales/static.js';
import { dynamicMessages } from '../locales/dynamic.js';
import { tr, setLanguage, getLanguage, LocalizedError } from '../i18n.js';
import { makeQuickConfigLink } from '../quick-config.js';

const rows = [...staticMessages, ...dynamicMessages];
const keys = new Set();
const placeholders = text => [...new Set(text.match(/\{\d+\}/g) || [])].sort();
for (const row of rows) {
  assert.equal(row.length, 4, row[0]);
  assert(!keys.has(row[0]), `Duplicate message: ${row[0]}`);
  keys.add(row[0]);
  row.forEach(text => {
    assert.equal(typeof text, 'string');
    assert(text.length, row[0]);
    assert.deepEqual(placeholders(text), placeholders(row[0]), row[0]);
  });
}
const html = await readFile(new URL('../index.html', import.meta.url), 'utf8');
for (const match of html.matchAll(/>([^<>]+)</g)) {
  const text = match[1].trim();
  if (/\p{Script=Han}/u.test(text) && !['简体中文','繁體中文','日本語'].includes(text)) assert(keys.has(text), `Missing static message: ${text}`);
}
for (const match of html.matchAll(/(?:title|placeholder|aria-label|alt)="([^"]+)"/g)) {
  if (/\p{Script=Han}/u.test(match[1])) assert(keys.has(match[1]), `Missing attribute: ${match[1]}`);
}
assert(html.indexOf('id="serialSupport"') < html.indexOf('id="languageSelect"'));
assert(html.indexOf('id="languageSelect"') < html.indexOf('id="sourceRepoLink"'));
for (const file of ['app.js', 'quick-config.js', 'simulator-ui.js']) {
  const source = await readFile(new URL(`../${file}`, import.meta.url), 'utf8');
  for (const match of source.matchAll(/\btr\(\s*("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')/g)) {
    const message = vm.runInNewContext(match[1], {}, {timeout:100});
    if (/\p{Script=Han}/u.test(message)) assert(keys.has(message), `Missing UI message in ${file}: ${message}`);
  }
}
const input = {ssid:'使用条件：&+Demo', pass:'DEMO+#123', api_key:'DEMO_KEY', api_host:'abc.re.qweatherapi.com', weather_city:'杭州'};
let expected;
for (const language of ['zh-CN','zh-TW','ja','en']) {
  setLanguage(language, {persist:false});
  assert.equal(getLanguage(), language);
  const link = makeQuickConfigLink(input);
  expected ??= link.url;
  assert.equal(link.url, expected, 'Language must not change configuration bytes');
  assert.equal(tr`已选择 ${7} 个文件`, tr('已选择 {0} 个文件', 7));
  assert(tr('已选择 {0} 个文件', '<unsafe>&').includes('<unsafe>&'));
  assert.throws(() => makeQuickConfigLink({...input, pass:'short'}), LocalizedError);
  if (language === 'en') {
    assert.equal(tr('资源制作'), 'Create assets');
    assert(!/\p{Script=Han}/u.test(link.warnings.join('')));
  }
}
setLanguage('zh-CN', {persist:false});
let error;
try { makeQuickConfigLink({...input, pass:'short'}); } catch (caught) { error = caught; }
setLanguage('en', {persist:false});
assert(error.message.includes('Primary Wi-Fi password'));
assert(error.message.includes('8 bytes'));
assert.equal(setLanguage('unsupported', {persist:false}), false);
assert.equal(getLanguage(), 'en');
const portal = await readFile(new URL('../portal-demo.js', import.meta.url), 'utf8');
assert.doesNotMatch(portal, /host-language|from ['"].*i18n|setLanguage/);
setLanguage('zh-CN', {persist:false});
console.log(`${keys.size} complete four-language messages; static coverage, placeholders and byte isolation passed.`);
