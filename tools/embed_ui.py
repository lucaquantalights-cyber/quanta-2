# Embeds ui/index.html into lib/QuantaWebUI/src/web_ui.h so the controller can serve its phone page.
# PlatformIO runs this automatically before every build (see extra_scripts in platformio.ini).
import os
try:
    Import("env")                                  # running inside PlatformIO
    root = env.subst("$PROJECT_DIR")
except NameError:                                  # running by hand: python3 tools/embed_ui.py
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "ui", "index.html")
dst = os.path.join(root, "lib", "QuantaWebUI", "src", "web_ui.h")
body = open(src, encoding="utf-8").read()
html = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">'
        '<meta name="theme-color" content="#081017"></head><body>' + body + '</body></html>')
assert ')QCUI"' not in html, "delimiter collision"
out = ("// GENERATED from ui/index.html by tools/embed_ui.py - edit that file instead.\n"
       "#pragma once\n#include <Arduino.h>\n"
       'const char INDEX_HTML[] PROGMEM = R"QCUI(' + html + ')QCUI";\n')
if not os.path.exists(dst) or open(dst, encoding="utf-8").read() != out:
    open(dst, "w", encoding="utf-8").write(out)
    print("embed_ui: updated lib/QuantaWebUI/src/web_ui.h")
