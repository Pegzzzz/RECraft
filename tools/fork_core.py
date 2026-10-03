#!/usr/bin/env python3
"""One-off: rename the SkyCraft sources copied into minecraft-mod/src/dev/recraft/core to RECraft names.
(SkyCraft by chasmlol, MIT - see THIRD-PARTY-NOTICES.md.) Kept for reference; run once on a fresh copy."""
import os, re, sys

ROOT = sys.argv[1]   # minecraft-mod
CLASSES = [("SkyCraftClient", "RecraftCoreClient"), ("SkyCraft", "RecraftCore"), ("SkyLink", "CoreLink"),
           ("SkyClient", "CoreClient"), ("SkyCollision", "CoreCollision"), ("SkyCollider", "CoreCollider"),
           ("SkyrimActorEntity", "Re4ActorEntity"), ("SkyCombat", "CoreCombat"), ("SkyNet", "CoreNet"),
           ("SkyClip", "CoreClip"), ("SkyTri", "CoreTri"), ("SkyRay", "CoreRay"), ("SkyDigBlast", "CoreDigBlast"),
           ("SkyDigClient", "CoreDigClient"), ("SkyDig", "CoreDig"), ("SkyWater", "CoreWater"), ("SkyAtlas", "CoreAtlas")]
FILE_RENAMES = dict(CLASSES)

def words(s):
    # prose (strings, comments): the product names
    s = s.replace("Local\\\\SkyCraft_v1", "Local\\\\RECraft_v1")
    s = re.sub(r"SkyCraft", "RECraft", s)
    s = re.sub(r"skycraft", "recraft", s)
    s = re.sub(r"SKYCRAFT", "RECRAFT", s)
    s = re.sub(r"Skyrim's", "RE4's", s)
    s = re.sub(r"Skyrim", "RE4", s)
    s = re.sub(r"skyrim", "re4", s)
    s = re.sub(r"SKYRIM", "RE4", s)
    s = s.replace("SKSE", "RE4 DLL").replace("Dovahkiin", "Pegz")
    return s

def code(s):
    for a, b in CLASSES:
        s = re.sub(r"\b%s\b" % a, b, s)
    s = s.replace("dev.skycraft", "dev.recraft.core")
    s = s.replace("Skyrim", "Re4").replace("skyrim", "re4").replace("SKYRIM", "RE4")
    s = s.replace("SKYCRAFT", "RECRAFT")
    return s

def transform(src):
    out, i, n = [], 0, len(src)
    buf = []
    def flush():
        if buf: out.append(code("".join(buf))); buf.clear()
    while i < n:
        c = src[i]
        if src.startswith('"""', i):
            j = src.index('"""', i + 3) + 3; flush(); out.append(words(src[i:j])); i = j
        elif c == '"':
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            flush(); out.append(words(src[i:j + 1])); i = j + 1
        elif c == "'":
            j = i + 1
            while src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            flush(); out.append(src[i:j + 1]); i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i); j = n if j < 0 else j
            flush(); out.append(words(src[i:j])); i = j
        elif src.startswith("/*", i):
            j = src.index("*/", i) + 2; flush(); out.append(words(src[i:j])); i = j
        else:
            buf.append(c); i += 1
    flush()
    return "".join(out)

core = os.path.join(ROOT, "src/dev/recraft/core")
for dp, _, fs in os.walk(core):
    for f in fs:
        p = os.path.join(dp, f)
        if not f.endswith(".java"): continue
        s = transform(open(p, encoding="utf-8").read())
        base = f[:-5]
        np_ = os.path.join(dp, FILE_RENAMES.get(base, base) + ".java")
        open(np_, "w", encoding="utf-8").write(s)
        if np_ != p: os.remove(p)
print("ok")
