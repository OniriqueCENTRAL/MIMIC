#!/usr/bin/env python3
"""Génère ui/mimic_ui.html (page autonome, sans CDN) à partir de l'interface et de three.js r128.
Usage: python tools/make_ui.py <mimic-3d.html> <three.min.js> <sortie.html>"""
import sys
src, three, out = sys.argv[1:4]
html = open(src, encoding="utf-8").read()
js = open(three, encoding="utf-8").read()
assert "</script" not in js.lower()
tag = '<script src="https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js"></script>'
assert tag in html, "balise three.js introuvable"
html = html.replace(tag, "<script>" + js + "</script>")
if "charset" not in html[:400].lower():
    html = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' + html
open(out, "w", encoding="utf-8").write(html)
print("OK", out, len(html), "octets")
