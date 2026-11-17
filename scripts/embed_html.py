#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embed src/web/index.html as a C string, with i18n.json inlined at /*I18N*/."""
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
out_path = Path(sys.argv[2])
i18n_path = Path(sys.argv[3]) if len(sys.argv) > 3 else src.parent / "i18n.json"

html = src.read_text(encoding="utf-8")
data = json.loads(i18n_path.read_text(encoding="utf-8"))
en = data.get("strings", {}).get("en") or {}
for code, table in (data.get("strings") or {}).items():
    missing = set(en) - set(table)
    if missing:
        sys.exit("i18n: %s is missing %s" % (code, sorted(missing)))
if "/*I18N*/" not in html:
    sys.exit("embed_html: /*I18N*/ marker missing in %s" % src)
html = html.replace("/*I18N*/", json.dumps(data, ensure_ascii=False, separators=(",", ":")))

out = ["/* Auto-generated from %s + %s - do not edit. */" % (src.name, i18n_path.name),
       "static const char webui_html[] ="]
for line in html.splitlines():
    s = line.replace("\\", "\\\\").replace('"', '\\"')
    out.append('  "%s\\n"' % s)
out.append(";")
out_path.write_text("\n".join(out) + "\n", encoding="utf-8")
print("wrote", out_path, "bytes", out_path.stat().st_size)
