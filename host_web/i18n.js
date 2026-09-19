// Local UI messages only. Never translate user data or raw device output.
import { staticMessages } from './locales/static.js';
import { dynamicMessages } from './locales/dynamic.js';

export const languages = ['zh-CN', 'zh-TW', 'ja', 'en'];
const rows = [...staticMessages, ...dynamicMessages];
const catalog = new Map(rows.map(row => [row[0], row]));
const preferenceKey = 'weather-clock-studio:language';
let language = 'zh-CN';
try {
  const saved = typeof window === 'undefined' ? null : window.localStorage.getItem(preferenceKey);
  if (languages.includes(saved)) language = saved;
} catch { /* Storage may be blocked; language selection still works. */ }
export const getLanguage = () => language;
export function tr(source, ...values) {
  const template = Array.isArray(source)
    ? source.reduce((text, part, index) => text + (index ? `{${index - 1}}` : '') + part, '')
    : String(source);
  const row = catalog.get(template);
  const translated = row?.[languages.indexOf(language)] ?? template;
  return translated.replace(/\{(\d+)\}/g, (match, index) => index < values.length ? String(values[index]) : match);
}
export class LocalizedError extends Error {
  constructor(render) {
    super();
    Object.defineProperty(this, 'message', { configurable: true, get: render });
  }
}
const bindings = new WeakMap();
function bind(element, property, render) {
  const current = bindings.get(element) || new Map();
  current.set(property, render);
  bindings.set(element, current);
  if (property === 'textContent') element.textContent = render();
  else element.setAttribute(property, render());
}
export function setText(element, render) { bind(element, 'textContent', render); }
export function setAttr(element, attribute, render) { bind(element, attribute, render); }
const staticNodes = new WeakMap();
export function localizeStatic(root) {
  const document = root.ownerDocument || root;
  const walker = document.createTreeWalker(root, 4);
  let node;
  while ((node = walker.nextNode())) {
    if (node.parentElement?.closest('script,style,textarea,[data-user-content],[translate="no"]')) continue;
    const source = node.nodeValue.trim();
    if (catalog.has(source)) staticNodes.set(node, { source, prefix: node.nodeValue.match(/^\s*/)[0], suffix: node.nodeValue.match(/\s*$/)[0] });
  }
  for (const element of root.querySelectorAll('*')) {
    for (const attr of ['title', 'placeholder', 'aria-label', 'alt']) {
      const value = element.getAttribute(attr);
      if (value && catalog.has(value)) setAttr(element, attr, () => tr(value));
    }
  }
  renderLanguage(root);
}
function renderLanguage(root) {
  for (const element of root.querySelectorAll('*')) {
    if (!element.isConnected) continue;
    for (const [property, render] of bindings.get(element) || []) {
      if (property === 'textContent') element.textContent = render();
      else element.setAttribute(property, render());
    }
  }
  const document = root.ownerDocument || root;
  const walker = document.createTreeWalker(root, 4);
  let node;
  while ((node = walker.nextNode())) {
    const saved = staticNodes.get(node);
    if (saved) node.nodeValue = saved.prefix + tr(saved.source) + saved.suffix;
  }
}
export function setLanguage(next, { persist = true } = {}) {
  if (!languages.includes(next)) return false;
  language = next;
  if (persist && typeof window !== 'undefined') { try { window.localStorage.setItem(preferenceKey, next); } catch {} }
  if (typeof document !== 'undefined') {
    document.documentElement.lang = next;
    const select = document.getElementById('languageSelect');
    if (select) select.value = next;
    renderLanguage(document);
    window.dispatchEvent(new CustomEvent('host-language-change', { detail: next }));
  }
  return true;
}

if (typeof document !== 'undefined') {
  document.documentElement.lang = language;
  localizeStatic(document);
  const select = document.getElementById('languageSelect');
  if (select) {
    select.value = language;
    select.addEventListener('change', () => setLanguage(select.value));
  }
  for (const input of document.querySelectorAll('input[type="file"]')) {
    const chooser = document.createElement('button');
    chooser.type = 'button';
    chooser.className = 'secondary-button file-choose';
    chooser.dataset.fileInput = input.id;
    chooser.setAttribute('aria-controls', input.id);
    setText(chooser, () => tr('选择文件'));
    const name = document.createElement('span');
    name.className = 'file-selection';
    name.dataset.userContent = '';
    function showFiles() {
      const files = [...(input.files || [])];
      setText(name, () => files.length === 0 ? tr('未选择文件') : files.length === 1 ? files[0].name : tr('已选择 {0} 个文件', files.length));
      name.title = files.map(file => file.name).join('\n');
    }
    chooser.addEventListener('click', event => { event.preventDefault(); if (!input.disabled) input.click(); });
    input.addEventListener('change', showFiles);
    input.classList.add('localized-file-input');
    input.tabIndex = -1;
    const row = document.createElement('div');
    row.className = 'file-choice-row';
    row.append(chooser, name);
    input.after(row);
    const syncDisabled = () => { chooser.disabled = input.disabled; };
    new MutationObserver(syncDisabled).observe(input, { attributes: true, attributeFilter: ['disabled'] });
    syncDisabled(); showFiles();
  }
}
