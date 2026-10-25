#!/usr/bin/env python3
"""Quanta firmware build only; never bumps versions, flashes or deploys."""
import argparse, json, re, shutil, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--device", help="Optional manufacturing label, e.g. quantacontroller1")
    p.add_argument("--build", action="store_true", help="Run PlatformIO (otherwise preview only)")
    a = p.parse_args()
    if a.device and not re.fullmatch(r"quantacontroller[1-9][0-9]*", a.device):
        raise SystemExit("ERROR: expected quantacontroller1, quantacontroller2, etc.")
    ini = ROOT / "platformio.ini"
    maincpp = ROOT / "src/main.cpp"
    if not ini.exists() or not maincpp.exists():
        raise SystemExit("ERROR: put these scripts in Quanta PlatformIO project, alongside platformio.ini and src/main.cpp")
    m = re.findall(r'^\s*#define\s+FW_VERSION\s+"(P-[0-9]+\.[0-9]+\.[0-9]+)"', maincpp.read_text(), re.M)
    if len(m) != 1: raise SystemExit("ERROR: FW_VERSION missing or duplicated")
    version = json.loads((ROOT/'scripts/firmware_version.json').read_text())['version']
    if version != m[0]: raise SystemExit("ERROR: version file and main.cpp differ")
    config = ini.read_text()
    if 'board_build.flash_size = 8MB' not in config or 'board_build.partitions = quanta_8mb.csv' not in config:
        raise SystemExit("ERROR: expected Quanta 8MB platformio.ini; check board and partition configuration")
    if not (ROOT/'quanta_8mb.csv').exists(): raise SystemExit("ERROR: quanta_8mb.csv missing")
    print(f"Quanta firmware {version}; controller {a.device or 'generic'}")
    print("No version change. No flash or Firebase deployment.")
    if not a.build:
        print("Preview only. Use --build to compile.")
        return
    pio = shutil.which('pio') or shutil.which('platformio') or str(Path.home() / '.platformio' / 'penv' / 'Scripts' / 'platformio.exe')
    if not pio: raise SystemExit("ERROR: PlatformIO CLI not found on PATH")
    subprocess.run([pio, 'run'], cwd=ROOT, check=True)
    print("Build finished; see .pio/build/esp32dev/firmware.bin")
if __name__ == '__main__': main()
