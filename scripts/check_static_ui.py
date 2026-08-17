#!/usr/bin/env python3
from __future__ import annotations
import re, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
STATIC=ROOT/"data"/"static"
REQUIRED_SCREENS={
"account-select.html","index.html","procedures.html","analysis.html","analysis-results.html","testing.html","protocols.html","protocol-detail.html","test-run.html","user-protocols.html","user-protocol-editor.html","external.html","quick-start.html","free-run.html","profiles.html","profile-editor.html","profile-run.html","heart-rate.html","heart-rate-run.html","biofeedback.html","biofeedback-run.html","lidar-mode.html","virtual-reality.html","vr-walk.html","patients.html","patient-search.html","patient-edit.html","patient-detail.html","patient-stats.html","patient-history.html","history.html","history-detail.html","statistics.html","settings.html","handrails.html","acceleration.html","calibration.html","heart-rate-sensor.html","units.html","data-transfer.html","date-time.html","screen-settings.html","passwords.html","external-devices.html","software-update.html","service.html"}
GENERATED_VENDOR_ASSETS={"vendor/bootstrap/bootstrap.min.css","vendor/bootstrap/bootstrap.bundle.min.js"}
REQUIRED_LOCAL_ASSETS={"app.js","style.css","charts.js","charts.css"}
REF_RE=re.compile(r"(?:href|src)\s*=\s*[\"']([^\"']+)[\"']",re.I)
def local_target(value):
    value=value.strip()
    if not value or value.startswith(("#","data:","mailto:","tel:","javascript:")): return None
    if value.startswith(("http://","https://","//")): raise ValueError(f"external runtime dependency: {value}")
    value=value.split("?",1)[0].split("#",1)[0]
    return value.lstrip("/") or None
def main():
    errors=[]
    if not STATIC.is_dir(): print(f"ERROR: static directory not found: {STATIC}"); return 1
    present={p.name for p in STATIC.glob("*.html")}
    missing=sorted(REQUIRED_SCREENS-present)
    if missing: errors.append("missing required screens: "+", ".join(missing))
    for asset in sorted(GENERATED_VENDOR_ASSETS|REQUIRED_LOCAL_ASSETS):
        if not (STATIC/asset).is_file(): errors.append(f"required offline asset missing: {asset}")
    for page in sorted(STATIC.glob("*.html")):
        text=page.read_text(encoding="utf-8")
        if 'href="style.css"' not in text and "href='style.css'" not in text: errors.append(f"{page.name}: style.css is not linked")
        if 'src="app.js"' not in text and "src='app.js'" not in text: errors.append(f"{page.name}: app.js is not linked")
        for ref in REF_RE.findall(text):
            try: target=local_target(ref)
            except ValueError as exc: errors.append(f"{page.name}: {exc}"); continue
            if target and not target.startswith("api/") and not (STATIC/target).exists(): errors.append(f"{page.name}: broken local reference: {ref}")
    style=(STATIC/"style.css").read_text(encoding="utf-8")
    if 'vendor/bootstrap/bootstrap.min.css' not in style: errors.append("style.css: local Bootstrap CSS import is missing")
    if re.search(r"@import\s+url\([\"']?https?://",style,re.I): errors.append("style.css: external CSS import found")
    app=(STATIC/"app.js").read_text(encoding="utf-8")
    if "vendor/bootstrap/bootstrap.bundle.min.js" not in app: errors.append("app.js: local Bootstrap bundle loader is missing")
    for name in ("statistics.html","patient-stats.html"):
        text=(STATIC/name).read_text(encoding="utf-8") if (STATIC/name).exists() else ""
        if 'src="charts.js"' not in text: errors.append(f"{name}: charts.js is not linked")
        if 'href="charts.css"' not in text: errors.append(f"{name}: charts.css is not linked")
    if errors:
        print("Static UI audit FAILED")
        for error in errors: print(" -",error)
        return 1
    print(f"Static UI audit OK: {len(present)} HTML screens; Remotion workflow, Bootstrap and chart assets are local.")
    return 0
if __name__=="__main__": sys.exit(main())
