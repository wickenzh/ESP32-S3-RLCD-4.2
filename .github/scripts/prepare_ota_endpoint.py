#!/usr/bin/env python3
"""为 GitHub Actions 从私有环境变量生成临时 OTA 端点头文件。"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path


ENDPOINT_ENVIRONMENTS = {
    "WEATHER_CLOCK_OTA_MANIFEST_URL": "OTA_MANIFEST_URL",
    "WEATHER_CLOCK_OTA_BACKUP_MANIFEST_URL": "OTA_BACKUP_MANIFEST_URL",
}
PLACEHOLDER_HOST = "example.invalid"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def load_endpoints() -> dict[str, str]:
    endpoints: dict[str, str] = {}
    for macro_name, environment_name in ENDPOINT_ENVIRONMENTS.items():
        url = os.environ.get(environment_name, "").strip()
        if not url.startswith("https://") or PLACEHOLDER_HOST in url:
            raise ValueError(
                f"GitHub Actions secret for {macro_name} is missing or invalid"
            )
        endpoints[macro_name] = url
    return endpoints


def render_header(endpoints: dict[str, str]) -> str:
    definitions = "\n".join(
        f"#define {name} {json.dumps(url)}" for name, url in endpoints.items()
    )
    return (
        "// GitHub Actions temporary OTA endpoint configuration; never commit.\n"
        "#pragma once\n"
        f"{definitions}\n"
    )


def main() -> int:
    args = parse_args()
    try:
        content = render_header(load_endpoints())
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
