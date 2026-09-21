// 集中维护设备配网页与电脑调试 Demo 共用的视觉资源和表单结构。
#pragma once

namespace wifi_portal_ui {

inline constexpr char kCommonCss[] = R"PORTAL(
:root{
  color-scheme:light;
  --ink:#1d1d1f;
  --ink-soft:#3a3a3c;
  --muted:#6e6e73;
  --line:#d2d2d7;
  --line-strong:#c7c7cc;
  --paper:#ffffff;
  --surface:#f5f5f7;
  --surface-soft:#f2f2f7;
  --success:#248a3d;
  --success-bg:#f0f9f2;
  --danger:#d70015;
  --danger-bg:#fff2f3;
  --focus:#0071e3;
}
*{box-sizing:border-box}
html{background:var(--surface)}
body{
  margin:0;
  background:var(--surface);
  color:var(--ink);
  font-family:-apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC","Segoe UI","Microsoft YaHei",sans-serif;
  letter-spacing:0;
  -webkit-font-smoothing:antialiased;
}
button,input{font:inherit}
button{-webkit-tap-highlight-color:transparent}
.portal-shell{width:min(100%,540px);margin:0 auto;padding:15px 16px 38px}
.portal-header{
  display:flex;
  flex-direction:row;
  align-items:center;
  justify-content:space-between;
  gap:10px;
  margin:0 2px 13px;
  text-align:left;
}
.brand-lockup{display:flex;flex:1 1 auto;flex-direction:row;align-items:center;gap:10px;min-width:0}
.brand-mark{
  flex:0 0 auto;
  width:40px;
  height:40px;
  border:1px solid #b8b8bd;
  border-radius:8px;
  display:grid;
  place-items:center;
  background:var(--paper);
  box-shadow:0 3px 12px rgba(0,0,0,.08);
  font-size:17px;
  font-weight:800;
}
.brand-copy{min-width:0}
.brand-copy h1{margin:0;font-size:20px;line-height:1.15;font-weight:750}
.brand-copy p{margin:3px 0 0;color:var(--muted);font-size:11px;line-height:1.3;white-space:nowrap}
.ap-meta{display:flex;flex:0 1 150px;min-width:0;flex-direction:column;align-items:flex-end;justify-content:center;gap:2px;margin:0;color:var(--muted);text-align:right}
.ap-meta span{
  display:inline;
  color:var(--muted);
  font-size:11px;
  font-weight:600;
}
.ap-meta strong{display:block;max-width:150px;overflow:hidden;text-overflow:ellipsis;color:var(--ink-soft);font-size:12px;font-weight:650;white-space:nowrap}
.portal-panel{
  border:1px solid var(--line);
  border-radius:8px;
  background:var(--paper);
  box-shadow:0 2px 10px rgba(0,0,0,.035);
}
.portal-panel-body{padding:16px}
.portal-form-shell{display:block}
.feedback{
  margin:0 0 12px;
  padding:13px 15px;
  border:1px solid rgba(215,0,21,.28);
  border-radius:8px;
  background:var(--danger-bg);
  color:#8d0614;
  font-size:14px;
  line-height:1.5;
}
.feedback.pending{border-color:rgba(0,113,227,.28);background:#eff7ff;color:#075aab}
.feedback.success{border-color:rgba(36,138,61,.28);background:var(--success-bg);color:#1c6b30}
.feedback strong{display:block;margin-bottom:3px;font-size:16px}
.form-section{
  padding:14px;
  border:1px solid var(--line);
  border-radius:8px;
  background:var(--paper);
  box-shadow:0 2px 10px rgba(0,0,0,.035);
}
.form-section+.form-section{margin-top:10px}
.section-heading{display:flex;align-items:center;gap:10px;margin-bottom:11px}
.section-heading>div{display:flex;min-width:0;align-items:baseline;gap:8px}
.section-index{
  flex:0 0 auto;
  width:27px;
  height:27px;
  border-radius:50%;
  display:grid;
  place-items:center;
  background:#007aff;
  color:#fff;
  font-size:10px;
  font-weight:800;
}
.form-section.section-weather .section-index{background:#34a853}
.form-section.section-offline .section-index{background:#f28b00}
.section-heading h2{flex:0 0 auto;margin:0;font-size:17px;line-height:1.2;font-weight:750}
.section-heading p{min-width:0;margin:0;color:var(--muted);font-size:11px;line-height:1.2;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.field{margin-top:10px}
.field:first-of-type{margin-top:0}
label{display:block;margin:0 0 5px;color:var(--ink-soft);font-size:13px;font-weight:650}
label em{color:var(--muted);font-size:12px;font-style:normal;font-weight:500}
input{
  width:100%;
  height:44px;
  border:1px solid transparent;
  border-radius:8px;
  padding:0 13px;
  outline:none;
  background:var(--surface-soft);
  color:var(--ink);
  font-size:16px;
}
input::placeholder{color:#8a959f}
input:disabled{color:var(--muted);opacity:.6;cursor:not-allowed}
input:focus{border-color:var(--focus);box-shadow:0 0 0 3px rgba(0,113,227,.14);background:#fff}
.hint{margin:5px 1px 0;color:var(--muted);font-size:11px;line-height:1.4}
.actions{margin-top:12px}
.submit{
  display:block;
  width:100%;
  height:48px;
  border:1px solid var(--focus);
  border-radius:8px;
  background:var(--focus);
  color:#fff;
  font-size:16px;
  font-weight:700;
  cursor:pointer;
  box-shadow:0 2px 8px rgba(0,113,227,.18);
}
.submit:active{transform:translateY(1px)}
.submit:disabled{cursor:wait;opacity:.72}
.save-status{
  display:none;
  margin:11px 0 0;
  padding:10px 12px;
  border:1px solid var(--line);
  border-radius:8px;
  background:var(--surface-soft);
  color:var(--ink-soft);
  font-size:13px;
  line-height:1.45;
}
.save-status.show{display:flex;align-items:center;gap:9px}
.activity-dot{
  width:8px;
  height:8px;
  border-radius:50%;
  background:var(--focus);
  box-shadow:0 0 0 4px rgba(0,113,227,.12);
  animation:portal-pulse 1.1s ease-in-out infinite;
}
@keyframes portal-pulse{50%{opacity:.36;transform:scale(.72)}}
.wifi-section{margin-top:12px}
.wifi-section .portal-panel-body{padding:15px 16px 16px}
.section-title{display:flex;align-items:center;justify-content:space-between;margin:0 2px 10px;font-size:13px;font-weight:800;color:var(--ink-soft)}
.section-title a{color:var(--ink);text-decoration:none}
.wifi-list{display:grid;grid-template-columns:1fr;gap:8px}
.wifi{
  width:100%;
  min-width:0;
  min-height:42px;
  border:1px solid var(--line);
  border-radius:8px;
  padding:10px 12px;
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:8px;
  background:#fff;
  color:var(--ink);
  text-align:left;
  cursor:pointer;
}
.wifi span{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:14px}
.wifi b{flex:0 0 auto;color:var(--muted);font-size:11px;white-space:nowrap}
.wifi:focus,.wifi:hover{border-color:var(--focus);background:#f7fbff}
.muted{grid-column:1/-1;padding:12px;border:1px dashed var(--line-strong);border-radius:8px;color:var(--muted);background:var(--surface-soft);font-size:13px}
.result-shell{width:min(100%,480px);margin:0 auto;padding:28px 14px}
.result-panel{padding:19px}
.result-state{
  display:inline-grid;
  min-width:74px;
  height:38px;
  padding:0 12px;
  place-items:center;
  border:1px solid var(--line);
  border-radius:8px;
  background:var(--surface-soft);
  font-size:14px;
  font-weight:900;
}
.result-panel h1{margin:16px 0 7px;font-size:23px}
.result-panel>p{margin:0 0 15px;color:var(--ink-soft);font-size:14px;line-height:1.55}
.note{margin:0 0 14px;padding:10px 11px;border:1px solid var(--line);border-radius:6px;background:var(--surface-soft);font-size:13px;line-height:1.5}
.meta{padding-top:13px;border-top:1px solid #e1e6eb;color:var(--muted);font-size:12px;line-height:1.65}
.primary-link{display:block;height:48px;margin-top:17px;border-radius:8px;background:var(--focus);color:#fff;font-weight:700;line-height:48px;text-align:center;text-decoration:none}
@media(max-width:430px){
  .portal-shell{padding:12px 12px 30px}
  .portal-header{margin-bottom:12px}
  .brand-mark{width:38px;height:38px;font-size:16px}
  .brand-copy h1{font-size:19px}
  .brand-copy p{font-size:10px}
  .ap-meta{flex-basis:118px}
  .ap-meta strong{max-width:118px;font-size:11px}
  .form-section{padding:16px 14px 17px}
  .wifi-section .portal-panel-body{padding:14px}
}
@media(prefers-reduced-motion:reduce){
  *{scroll-behavior:auto!important}
  .activity-dot{animation:none}
}
)PORTAL";

inline constexpr char kCommonScript[] = R"PORTAL(
function pick(ssid){
  var field=document.querySelector("[name=ssid]");
  if(!field){return;}
  field.value=ssid;
  var password=document.querySelector("[name=pass]");
  if(password){password.focus();}
}
function beginSave(form){
  var provider=document.getElementById("weather-provider");
  if(provider){provider.value=document.getElementById("open-meteo").checked?"open_meteo":"qweather";}
  var button=form.querySelector(".submit");
  var status=document.getElementById("save-status");
  if(button){
    button.disabled=true;
    button.textContent="正在保存，请稍候…";
  }
  if(status){status.classList.add("show");}
  setTimeout(function(){form.submit();},80);
  return false;
}
function selectWeatherProvider(){
  var toggle=document.getElementById('open-meteo');
  var selected=toggle && toggle.checked;
  var provider=document.getElementById('weather-provider');
  if(provider){provider.value=selected?'open_meteo':'qweather';}
  ['weather-key','weather-host'].forEach(function(id){var field=document.getElementById(id);if(field){field.disabled=selected;}});
}
document.addEventListener('DOMContentLoaded',selectWeatherProvider);
)PORTAL";

inline constexpr char kFormHtml[] = R"PORTAL(
<form method='post' action='/save' accept-charset='UTF-8' onsubmit='return beginSave(this)'>
  <div class='form-section section-network'>
    <div class='section-heading'>
      <span class='section-index'>01</span>
      <div><h2>联网配置</h2><p>连接并验证后自动进入工作状态</p></div>
    </div>
    <div class='field'>
      <label for='wifi-ssid'>主 Wi-Fi 名称（SSID）</label>
      <input id='wifi-ssid' name='ssid' placeholder='请选择或输入 Wi-Fi 名称' value='%s' autocomplete='off'>
    </div>
    <div class='field'>
      <label for='wifi-pass'>主 Wi-Fi 密码</label>
      <input id='wifi-pass' name='pass' placeholder='请输入 Wi-Fi 密码' type='password' autocomplete='current-password'>
      <p class='hint'>SSID 未改变时，密码留空可继续使用原密码。</p>
    </div>
    <div class='field'>
      <label for='backup-wifi-ssid'>备用 Wi-Fi 名称 <em>选填</em></label>
      <input id='backup-wifi-ssid' name='backup_ssid' placeholder='主 Wi-Fi 不可用时自动尝试' value='%s' autocomplete='off'>
    </div>
    <div class='field'>
      <label for='backup-wifi-pass'>备用 Wi-Fi 密码 <em>选填</em></label>
      <input id='backup-wifi-pass' name='backup_pass' placeholder='请输入备用 Wi-Fi 密码' type='password' autocomplete='off'>
      <p class='hint'>可不配置；备用连接成功后会自动成为首选 Wi-Fi。</p>
    </div>
  </div>
  <div class='form-section section-weather'>
    <div class='section-heading'>
      <span class='section-index'>02</span>
      <div><h2>天气服务</h2><p>用于天气、预报与空气质量</p></div>
    </div>
    <div class='field'>
      <label style='display:flex;align-items:center;gap:8px'><input id='open-meteo' type='checkbox' onchange='selectWeatherProvider()' style='width:18px;height:18px;padding:0;margin:0'> 使用 Open-Meteo</label>
      <input id='weather-provider' type='hidden' name='weather_provider' value='qweather'>
      <p class='hint'>开启后无需和风密钥，不提供天气预警，空气质量使用 US AQI。公共服务仅限非商业使用。</p>
      <p class='hint'>数据：<a href='https://open-meteo.com/' target='_blank' rel='noopener'>Open-Meteo</a> / <a href='https://atmosphere.copernicus.eu/' target='_blank' rel='noopener'>CAMS</a>；城市：<a href='https://www.geonames.org/' target='_blank' rel='noopener'>GeoNames</a></p>
    </div>
    <div class='field' data-qweather>
      <label for='weather-key'>和风天气 API 密钥</label>
      <input id='weather-key' name='api_key' placeholder='请输入和风天气 API Key' value='' autocomplete='off'>
    </div>
    <div class='field' data-qweather>
      <label for='weather-host'>和风天气 API Host</label>
      <input id='weather-host' name='api_host' placeholder='例如：abc123.re.qweatherapi.com' value='' autocomplete='off' aria-describedby='api-host-hint'>
      <p id='api-host-hint' class='hint'>请在和风天气控制台“设置 → API Host”中查看；只填写域名，不含 https:// 和路径。已有 Host 时可留空。</p>
    </div>
    <div class='field'>
      <label for='weather-city'>天气城市 <em>选填</em></label>
      <input id='weather-city' name='weather_city' placeholder='例如：杭州；留空则根据公网 IP 自动定位' value='%s' autocomplete='off'>
    </div>
  </div>
  <div class='form-section section-offline'>
    <div class='section-heading'>
      <span class='section-index'>03</span>
      <div><h2>离线使用</h2><p>不连接 Wi-Fi 时设置本地时间</p></div>
    </div>
    <div class='field'>
      <label for='manual_time'>离线日期和时间 <em>选填</em></label>
      <input id='manual_time' name='manual_time' type='datetime-local' placeholder='连接 Wi-Fi 时可留空' aria-describedby='manual-time-hint'>
      <p id='manual-time-hint' class='hint'>正常连接 Wi-Fi 时留空；仅离线使用时填写。</p>
    </div>
  </div>
  <div class='actions'>
    <button class='submit' type='submit'>保存并连接</button>
  </div>
  <p id='save-status' class='save-status' role='status' aria-live='polite'><span class='activity-dot' aria-hidden='true'></span><span>正在保存设置并连接，请稍候…</span></p>
</form>
)PORTAL";

} // namespace wifi_portal_ui
