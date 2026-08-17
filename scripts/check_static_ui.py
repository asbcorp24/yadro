#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATIC = ROOT / "data" / "static"

REQUIRED_SCREENS = {
    "index.html",
    "testing.html",
    "protocols.html",
    "protocol-detail.html",
    "test-run.html",
    "user-protocols.html",
    "user-protocol-editor.html",
    "external.html",
    "free-run.html",
    "profiles.html",
    "profile-editor.html",
    "profile-run.html",
    "heart-rate.html",
    "heart-rate-run.html",
    "patients.html",
    "patient-search.html",
    "patient-edit.html",
    "patient-detail.html",
    "patient-stats.html",
    "patient-history.html",
    "history.html",
    "history-detail.html",
    "settings.html",
    "acceleration.html",
    "calibration.html",
    "heart-rate-sensor.html",
    "units.html",
    "data-transfer.html",
    "screen-settings.html",
    "passwords.html",
    "service.html",
}

GENERATED_VENDOR_ASSETS = {
    "vendor/bootstrap/bootstrap.min.css",
    "vendor/bootstrap/bootstrap.bundle.min.js",
}

REF_RE = re.compile(r"(?:href|src)\s*=\s*[\"']([^\"']+)[\"']", re.I)


def local_target(value: str) -> str | None:
    value = value.strip()
    if not value or value.startswith(("#", "data:", "mailto:", "tel:", "javascript:")):
        return None
    if value.startswith(("http://", "https://", "//")):
        raise ValueError(f"external runtime dependency: {value}")
    value = value.split("?", 1)[0].split("#", 1)[0]
    if not value:
        return None
    return value.lstrip("/")


def main() -> int:
    errors: list[str] = []
    if not STATIC.is_dir():
        print(f"ERROR: static directory not found: {STATIC}")
        return 1

    present = {p.name for p in STATIC.glob("*.html")}
    missing = sorted(REQUIRED_SCREENS - present)
    if missing:
        errors.append("missing required screens: " + ", ".join(missing))

    for asset in sorted(GENERATED_VENDOR_ASSETS):
        if not (STATIC / asset).is_file():
            errors.append(f"offline Bootstrap asset missing after CMake configure: {asset}")

    for page in sorted(STATIC.glob("*.html")):
        text = page.read_text(encoding="utf-8")
        if 'href="style.css"' not in text and "href='style.css'" not in text:
            errors.append(f"{page.name}: style.css is not linked")
        if 'src="app.js"' not in text and "src='app.js'" not in text:
            errors.append(f"{page.name}: app.js is not linked")

        for ref in REF_RE.findall(text):
            try:
                target = local_target(ref)
            except ValueError as exc:
                errors.append(f"{page.name}: {exc}")
                continue
            if not target:
                continue
            # API routes and browser-created query links are not static files.
            if target.startswith("api/"):
                continue
            target_path = STATIC / target
            if not target_path.exists():
                errors.append(f"{page.name}: broken local reference: {ref}")

    style = (STATIC / "style.css").read_text(encoding="utf-8")
    if 'vendor/bootstrap/bootstrap.min.css' not in style:
        errors.append("style.css: local Bootstrap CSS import is missing")
    if re.search(r"@import\s+url\([\"']?https?://", style, re.I):
        errors.append("style.css: external CSS import found")

    app = (STATIC / "app.js").read_text(encoding="utf-8")
    if "vendor/bootstrap/bootstrap.bundle.min.js" not in app:
        errors.append("app.js: local Bootstrap bundle loader is missing")

    if errors:
        print("Static UI audit FAILED")
        for error in errors:
            print(" -", error)
        return 1

    print(f"Static UI audit OK: {len(present)} HTML screens; Bootstrap assets are local; no external HTML runtime references.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
