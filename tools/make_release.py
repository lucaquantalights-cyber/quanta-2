# Makes the fleet update folder from the last PlatformIO build.
#   1. Raise FW_VERSION in src/main.cpp (controllers only install a HIGHER version).
#   2. Build in PlatformIO.
#   3. Run:  python tools/make_release.py ["What's new text"]
#   4. Upload the release/quanta folder's two files to your server's quanta/ folder.
import hashlib, json, os, re, shutil, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
version = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', open(os.path.join(root, "src", "main.cpp"), encoding="utf-8").read()).group(1)
binpath = os.path.join(root, ".pio", "build", "esp32dev", "firmware.bin")
if not os.path.exists(binpath):
    sys.exit("No build found at .pio/build/esp32dev/firmware.bin - build the project in PlatformIO first.")

data = open(binpath, "rb").read()
if data[0] != 0xE9:
    sys.exit("firmware.bin doesn't look like an ESP32 app image.")
out = os.path.join(root, "release", "quanta")
os.makedirs(out, exist_ok=True)
shutil.copyfile(binpath, os.path.join(out, "firmware.bin"))
manifest = {
    "product": "quanta-altair",
    "version": version,
    "firmware": "firmware.bin",
    "md5": hashlib.md5(data).hexdigest(),
    "size": len(data),
    "notes": sys.argv[1] if len(sys.argv) > 1 else "",
}
with open(os.path.join(out, "firmware.json"), "w") as f:
    json.dump(manifest, f, indent=2)
print(f"release/quanta/firmware.bin   {len(data):,} bytes")
print(f"release/quanta/firmware.json  version {version}, md5 {manifest['md5']}")
print("Upload both files to the quanta/ folder on your update server.")
