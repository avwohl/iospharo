#!/usr/bin/env python3
"""List every public function of the SDL2 headers.

Usage: sdl2-list-functions.py <SDL2 include dir> <out.json>
Writes {name: [header, return type, parameter text]}.  The VM audit
(docs/sdl2-stubs.md) and scripts/gen-sdl2-device-stubs.py read this list.
"""
import glob
import json
import re
import sys

funcs = {}
for path in sorted(glob.glob(sys.argv[1] + "/SDL*.h")):
    text = open(path, errors="ignore").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"^\s*#.*$", "", text, flags=re.M)
    for statement in text.split(";"):
        m = re.search(r"extern\s+(?:DECLSPEC\s+)?(.*?)\s*SDLCALL\s+(\w+)\s*\((.*)\)", statement.replace("\n", " "), flags=re.S)
        if m:
            header = path.rsplit("/", 1)[1]
            funcs.setdefault(m.group(2), [header, re.sub(r"\s+", " ", m.group(1)).strip(), re.sub(r"\s+", " ", m.group(3)).strip()])
json.dump(funcs, open(sys.argv[2], "w"), indent=0, sort_keys=True)
print(len(funcs), "functions")
