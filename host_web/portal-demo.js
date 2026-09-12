// 独立虚拟配网表单：仅内存校验与假结果，不发送或存储用户字段。
(() => {
  const form = document.querySelector('form');
  const result = document.getElementById('demo-result');
  const button = form.querySelector('.submit');
  const pending = document.getElementById('save-status');
  let timer;
  const bytes = value => new TextEncoder().encode(value).length;
  const reset = () => {
    clearTimeout(timer); form.reset(); result.textContent = '';
    pending.classList.remove('show'); button.disabled = false;
    button.textContent = '保存并连接';
  };
  document.getElementById('demo-networks').addEventListener('click', event => {
    if (event.target.dataset.ssid) form.elements.ssid.value = event.target.dataset.ssid;
  });
  document.getElementById('demo-reset').addEventListener('click', reset);
  form.addEventListener('submit', event => {
    event.preventDefault();
    if (button.disabled) return;
    const fields = new FormData(form);
    const value = name => String(fields.get(name) || '').trim();
    const ssid = value('ssid');
    let error = '';
    if (!ssid && !value('manual_time')) error = '请填写 Wi-Fi 名称，或选择离线日期和时间。';
    for (const key of ['ssid', 'backup_ssid']) if (bytes(value(key)) > 32) error = 'Wi-Fi 名称不能超过32字节。';
    for (const key of ['pass', 'backup_pass']) {
      const password = String(fields.get(key) || '');
      if (password && (bytes(password) < 8 || bytes(password) > 63)) error = 'Wi-Fi 密码长度应为8至63字节。';
    }
    if (bytes(value('weather_city')) > 31 || /[&=?#%/\\<>"'`]/.test(value('weather_city'))) error = '天气城市格式不正确。';
    if (value('api_host') && !/^[a-zA-Z0-9.-]+$/.test(value('api_host'))) error = 'API Host 只填写域名，不包含协议和路径。';
    if (error) { result.textContent = error; return; }
    const outcome = document.getElementById('demo-outcome').value;
    button.disabled = true; pending.classList.add('show'); result.textContent = '';
    timer = setTimeout(() => {
      pending.classList.remove('show'); button.disabled = false;
      result.textContent = !ssid ? '离线设置完成（模拟），未修改电脑时间。' : outcome === 'success' ? '连接成功（模拟）。配置仅在本页演示，未连接真实网络。' : outcome === 'password' ? '连接失败（模拟）：Wi-Fi 密码错误，请重试。' : '连接超时（模拟），请重试。';
      for (const name of ['pass', 'backup_pass', 'api_key']) form.elements[name].value = '';
    }, 1200);
  });
})();
