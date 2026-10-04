#!/usr/bin/env python3
"""Build one deterministic, gzip-compressed page with no package manager."""
import base64
import gzip
import json
import pathlib
import re

root = pathlib.Path(__file__).resolve().parents[1]
web = root / "web"
build = root / "build"
build.mkdir(exist_ok=True)


def inline_module(name, expected_imports=()):
    source = (web / name).read_text()
    imports = []

    def remove_import(match):
        imports.append(match.group(2))
        aliases = []
        for binding in match.group(1).split(","):
            binding = binding.strip()
            if not binding:
                continue
            parsed = re.fullmatch(
                r"([A-Za-z_$][\w$]*)(?:\s+as\s+([A-Za-z_$][\w$]*))?", binding
            )
            if not parsed:
                raise SystemExit(f"Unsupported import binding in {name}: {binding}")
            original, alias = parsed.groups()
            if alias and alias != original:
                aliases.append(f"const {alias} = {original};\n")
        return "".join(aliases)

    source = re.sub(
        r"^import\s*\{([^}]*)\}\s*from\s*['\"]([^'\"]+)['\"];\s*",
        remove_import,
        source,
        flags=re.MULTILINE,
    )
    if imports != list(expected_imports):
        raise SystemExit(f"Unexpected imports in {name}: {imports}")
    return re.sub(r"^export (?=(?:const|function)\b)", "", source, flags=re.MULTILINE)


javascript = "\n".join(
    (
        inline_module("image-format.js"),
        inline_module("formats.js", ("./image-format.js",)),
        inline_module("app.js", ("./formats.js",)),
    )
)
style = (web / "style.css").read_text()
for font in sorted((web / "fonts").glob("*.woff2")):
    style = style.replace(
        "fonts/" + font.name,
        "data:font/woff2;base64," + base64.b64encode(font.read_bytes()).decode("ascii"),
    )
for license in sorted((web / "fonts").glob("*-OFL.txt")):
    style += "\n/* " + license.read_text().replace("*/", "* /") + " */\n"
html = (
    (web / "index.html")
    .read_text()
    .replace("/*STYLE*/", style)
    .replace("/*SCRIPT*/", javascript)
)
raw = html.encode()
packed = gzip.compress(raw, compresslevel=9, mtime=0)
(build / "index.html").write_bytes(raw)
(build / "index.html.gz").write_bytes(packed)
(build / "web_asset.h").write_text(
    "#pragma once\n#include <stdint.h>\nstatic const uint8_t web_page_gzip[]={\n"
    + ",".join(str(b) for b in packed)
    + "\n};\n"
)
report = {
    "raw_bytes": len(raw),
    "gzip_bytes": len(packed),
    "saved_percent": round(100 * (1 - len(packed) / len(raw)), 1),
}
(build / "web_sizes.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
