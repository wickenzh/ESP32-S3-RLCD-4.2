#!/usr/bin/env python3
"""构建浏览器LVGL/SDL产物及独立虚拟配网页，写入实际构建来源信息。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from datetime import datetime, timezone

root = Path(__file__).resolve().parents[2]
firmware = root / "RLCD_CLOCK"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, default=root / "host_web/simulator")
parser.add_argument("--build", type=Path, default=root / "build/web-simulator")
parser.add_argument("--lvgl", type=Path, default=firmware / "managed_components/lvgl__lvgl")
args = parser.parse_args()
output, build = args.output.resolve(), args.build.resolve()
if output == root or output == firmware or root.is_relative_to(output):
    raise SystemExit("Output must not contain source repository")
version = re.search(r'PROJECT_VER\s+"([^"]+)"', (firmware / "CMakeLists.txt").read_text()).group(1)
lvgl_header = (args.lvgl / "lvgl.h").read_text()
for field, value in (("MAJOR", "8"), ("MINOR", "4"), ("PATCH", "0")):
    if not re.search(r"#define\s+LVGL_VERSION_" + field + r"\s+" + value + r"\b", lvgl_header):
        raise SystemExit("The web simulator currently requires locked LVGL 8.4.0")
revision = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
inputs = []
for directory in (firmware / "simulator", firmware / "main/ui", firmware / "main/assets", firmware / "main/core"):
    inputs.extend(p for p in directory.rglob("*") if p.suffix in (".h", ".c", ".cpp", ".txt") and "build" not in p.relative_to(directory).parts)
inputs.extend([firmware / "main/network/qweather_icons.cpp", firmware / "main/network/qweather_icons.h", firmware / "main/network/wifi_portal_ui_assets.h", Path(__file__), root / "host_web/portal-demo.js", firmware / "dependencies.lock", firmware / "CMakeLists.txt"])
digest = hashlib.sha256()
for p in sorted(set(inputs)):
    digest.update(str(p.relative_to(root)).encode()); digest.update(p.read_bytes())
dirty = bool(subprocess.check_output(["git", "-C", str(root), "status", "--porcelain", "--", "RLCD_CLOCK/simulator", "RLCD_CLOCK/main", "RLCD_CLOCK/CMakeLists.txt", "RLCD_CLOCK/dependencies.lock", "host_web/scripts/build_web_simulator.py", "host_web/portal-demo.js"], text=True).strip())
info = {"firmwareVersion": version, "sourceCommit": revision, "sourceDirty": dirty, "sourceDigest": digest.hexdigest(), "builtAt": datetime.now(timezone.utc).isoformat(), "engine": "LVGL 8.4.0 + SDL2 / WebAssembly (Emscripten 4.0.23)"}
build.mkdir(parents=True, exist_ok=True)
(build / "web_build_info.h").write_text('#pragma once\n#define WEB_FIRMWARE_VERSION '+json.dumps(version)+'\n#define WEB_BUILD_INFO R"BUILD('+json.dumps(info, ensure_ascii=True)+')BUILD"\n')
emcmake = shutil.which("emcmake")
if not emcmake: raise SystemExit("Activate Emscripten 4.0.23 before building")
emcc_version = subprocess.check_output(["emcc", "--version"], text=True).splitlines()[0]
if "4.0.23" not in emcc_version: raise SystemExit("Expected Emscripten 4.0.23")
subprocess.run([emcmake,"cmake","-S",str(firmware / "simulator"),"-B",str(build),"-DCMAKE_BUILD_TYPE=Release",f"-DWEATHER_CLOCK_LVGL_DIR={args.lvgl.resolve()}"], check=True)
subprocess.run(["cmake","--build",str(build),"--target","weather_clock_sdl","-j","4"], check=True)
output.mkdir(parents=True, exist_ok=True)
for name in ("weather-clock.js", "weather-clock.wasm"):
    shutil.copyfile(build / name, output / name)

header = (firmware / "main/network/wifi_portal_ui_assets.h").read_text()
def literal(name):
    return re.search(r'\b'+name+r'\[\]\s*=\s*R"PORTAL\((.*?)\)PORTAL";', header, re.S).group(1)
form = literal("kFormHtml")
assert form.count("%s") == 3
for value in ("Demo-WiFi", "Demo-Backup", ""):
    form = form.replace("%s", value, 1)
form = form.replace("action='/save'", "action='#'").replace(" onsubmit='return beginSave(this)'", "")
form = form.replace("例如：杭州", "例如：城市").replace("autocomplete='current-password'", "autocomplete='off'")
script = (root / "host_web/portal-demo.js").read_text()
demo_css = "h1{font-size:20px;margin:0 0 16px} select,#demo-networks button,#demo-reset{font:inherit;min-height:36px;border:1px solid #c5c9d0;border-radius:6px;padding:6px 10px;background:#fff;color:#1d1d1f} #demo-networks{display:flex;gap:8px;margin:12px 0} #demo-reset{margin-top:12px} #demo-result{line-height:1.5;color:#30343a}"
csp = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; form-action 'none'; connect-src 'none'"
portal = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="Content-Security-Policy" content="'+csp+'"><title>虚拟配网</title><style>'+literal("kCommonCss")+demo_css+'</style><body><main class="portal-shell"><h1>虚拟配网</h1><label>模拟结果 <select id="demo-outcome"><option value="success">连接成功</option><option value="password">密码错误</option><option value="timeout">连接超时</option></select></label><div id="demo-networks"><button type="button" data-ssid="Demo-WiFi">Demo-WiFi</button><button type="button" data-ssid="Demo-Guest">Demo-Guest</button></div>'+form+'<button id="demo-reset" type="button">重置表单</button><p id="demo-result" role="status"></p></main><script>'+script+'</script></body></html>'
(output / "portal.html").write_text(portal)
info["artifacts"] = {name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in ("weather-clock.js", "weather-clock.wasm", "portal.html")}
(output / "build-info.json").write_text(json.dumps(info, indent=2)+"\n")
print("Browser simulator ready:", output)
