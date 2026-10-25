#!/usr/bin/env python3
"""Quanta only: explicitly bump release version. No builds, flashes or deployments."""
import argparse, json, re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"
VERSION = ROOT / "scripts" / "firmware_version.json"
PATTERN = re.compile(r'(?m)^#define FW_VERSION "(\d+)\.(\d+)\.(\d+)"$')

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--part", choices=["major", "minor", "patch"], default="patch")
    p.add_argument("--apply", action="store_true", help="Actually update both files; otherwise preview")
    a = p.parse_args()
    code = MAIN.read_text(encoding="utf-8")
    matches = list(PATTERN.finditer(code))
    if len(matches) != 1:
        raise SystemExit("ERROR: expected exactly one FW_VERSION in Quanta src/main.cpp")
    current = matches[0].group(0).split('"')[1]
    saved = json.loads(VERSION.read_text(encoding="utf-8"))["version"]
    if current != saved:
        raise SystemExit(f"ERROR: main.cpp={current}, firmware_version.json={saved}; resolve mismatch first")
    v = list(map(int,current.split('.')))
    if a.part == 'major': v = [v[0]+1,0,0]
    elif a.part == 'minor': v = [v[0],v[1]+1,0]
    else: v[2] += 1
    new = '.'.join(map(str,v))
    print(f"Quanta release: {current} -> {new} ({'APPLY' if a.apply else 'PREVIEW ONLY'})")
    if not a.apply: return
    newcode = code[:matches[0].start()] + f'#define FW_VERSION "{new}"' + code[matches[0].end():]
    MAIN.write_text(newcode, encoding="utf-8")
    VERSION.write_text(json.dumps({"version":new}, indent=2)+"\n",encoding="utf-8")
    print("Updated Quanta main.cpp and firmware_version.json. No firmware built or flashed.")
if __name__ == "__main__": main()
