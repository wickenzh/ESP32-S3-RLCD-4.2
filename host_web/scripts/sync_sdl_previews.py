#!/usr/bin/env python3
"""从SDL权威清单复制八个主页面到上位机，或核对副本是否同步。"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import sys

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("sdl_preview_catalog", root / "scripts/sdl_preview_catalog.py")
catalog = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = catalog
spec.loader.exec_module(catalog)
modes = ("main", "weather_board", "gallery", "flip_clock", "history", "calendar", "xiaozhi", "aggregate_clock")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
cases = {case.mode: case for case in catalog.PREVIEW_CASES}
target = root / "host_web/assets/screens"
if not args.check:
    target.mkdir(parents=True, exist_ok=True)
for mode in modes:
    name = cases[mode].stem + ".png"
    source = root / "assets/previews" / name
    destination = target / name
    if args.check:
        if not destination.is_file() or source.read_bytes() != destination.read_bytes():
            raise SystemExit(f"SDL preview out of sync: {name}")
    else:
        shutil.copyfile(source, destination)
print("Eight SDL main-page previews are synchronized")
