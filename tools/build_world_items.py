#!/usr/bin/env python3
"""RECraft: Minecraft models for the items lying in RE4's world (enemy and crate drops, boxes on tables).

RE4 draws those with the common item models BIO4/SS/cmn/itmXX.bin/.tpl (XX = item id in hex), each with
its own texture pack ImagePack(HD)/220000XX.pack.yz2. This rebuilds them from YOUR OWN Minecraft jar:
  * ammo of every kind -> an arrow, herbs -> a steak, standing up like a dropped Minecraft item
    (one pixel thick; RECraft's DLL turns them slowly, like Minecraft's dropped items)
  * hand grenade -> a TNT block, incendiary grenade -> a fire charge, flash grenade -> an ender pearl

Rooms carry their own copies of the item models (the room's ITM block); RECraft's DLL swaps those for these
files when a room loads (dll/items.cpp), so every item in the world shows the Minecraft model.
Needs: python3 + Pillow, mono, JADERLINK's RE4_UHD_BIN_TPL_TOOL.exe and RE4_UHD_PACK_TOOL.exe.

usage: build_world_items.py <minecraft-client.jar> <out dir> [item ids in hex...]
writes <out>/SS/cmn/itmXX.bin + .tpl, <out>/ImagePackHD/220000XX.pack.yz2 and <out>/ImagePack/220000XX.pack.yz2
"""
import os, shutil, sys, zipfile
from build_item_models import (BIN_TOOL, PACK_TOOL, MATERIAL, UUBIN, mono, item_image, block_image, dds_dxt1)
from PIL import Image

# item id -> (Minecraft item, pixel size in model units (1 = 10 cm))
ARROW, STEAK = ('arrow', 0.17), ('cooked_beef', 0.15)
WORLD = {
    # ammo of every kind -> arrows
    0x00: ARROW, 0x04: ARROW, 0x07: ARROW, 0x18: ARROW, 0x1A: ARROW, 0x20: ARROW, 0x6A: ARROW,
    # herbs (single and mixed) -> steak
    0x06: STEAK, 0x19: STEAK, 0x1C: STEAK, 0x12: STEAK, 0x13: STEAK, 0x14: STEAK, 0x15: STEAK, 0x16: STEAK, 0xA8: STEAK,
    # grenades
    0x01: ('tnt', 2.2), 0x02: ('fire_charge', 0.13), 0x0E: ('ender_pearl', 0.13),
    # handguns -> swords (RECraft's tiers)
    0x23: ('wooden_sword', 0.32), 0x21: ('stone_sword', 0.32), 0x40: ('stone_sword', 0.32),
    0x25: ('iron_sword', 0.32), 0x26: ('iron_sword', 0.32), 0x27: ('diamond_sword', 0.32),
    0x03: ('netherite_sword', 0.32), 0x29: ('netherite_sword', 0.32), 0x2A: ('netherite_sword', 0.32), 0x37: ('netherite_sword', 0.32),
    # shotguns -> axes
    0x2C: ('stone_axe', 0.32), 0x94: ('iron_axe', 0.32), 0x2D: ('diamond_axe', 0.32), 0x34: ('netherite_axe', 0.32),
    # rifles -> bow, TMP -> enchanted book
    0x2E: ('bow', 0.32), 0x6B: ('bow', 0.32), 0x2F: ('bow', 0.32), 0x6C: ('bow', 0.32), 0x51: ('bow', 0.32),
    0x30: ('enchanted_book', 0.2), 0x32: ('enchanted_book', 0.2), 0x3E: ('enchanted_book', 0.2),
    # pesetas -> emerald
    0x78: ('emerald', 0.13), 0x79: ('emerald', 0.13),
}


class Obj:
    def __init__(self):
        self.V, self.VT, self.VN, self.F = [], [], [], []

    def quad(self, pts, uv, n):
        """pts: 4 corners (counter-clockwise seen from the normal); uv: one (u, v) or 4 of them."""
        self.VN.append(n)
        ni = len(self.VN)
        uvs = uv if isinstance(uv[0], tuple) else [uv] * 4
        ids = []
        for p, t in zip(pts, uvs):
            self.V.append(p)
            self.VT.append(t)
            ids.append(len(self.V))
        a, b, c, d = ids
        self.F.append((a, b, c, ni))
        self.F.append((a, c, d, ni))

    def text(self):
        out = ['mtllib item.mtl']
        out += ['v %.6f %.6f %.6f' % v for v in self.V]
        out += ['vt %.6f %.6f' % t for t in self.VT]
        out += ['vn %.6f %.6f %.6f' % n for n in self.VN]
        out += ['g MATERIAL_000', 'usemtl MATERIAL_000']
        out += ['f %d/%d/%d %d/%d/%d %d/%d/%d' % (a, a, n, b, b, n, c, c, n) for a, b, c, n in self.F]
        return '\n'.join(out) + '\n'


def card_voxels(o, img, p, axis):
    """One-pixel-thick card of the item's opaque pixels, standing on the ground, facing +Z (axis 0) or +X (1)."""
    px = img.load()
    solid = lambda x, y: 0 <= x < 16 and 0 <= y < 16 and px[x, y][3] >= 128
    xs = [x for y in range(16) for x in range(16) if solid(x, y)]
    ys = [y for y in range(16) for x in range(16) if solid(x, y)]
    cx = (min(xs) + max(xs) + 1) / 2
    base = max(ys) + 1                       # the lowest opaque row sits 2 cm above the ground
    t = p / 2

    def P(x, y, z):                          # card space (pixels, y down) -> model units
        X, Y, Z = (x - cx) * p, (base - y) * p + 0.2, z
        return (X, Y, Z) if axis == 0 else (Z, Y, -X)

    def N(n):
        return n if axis == 0 else (n[2], n[1], -n[0])

    e = 0.08 / 16                            # keep samples inside each pixel's texels

    def runs(cells):                         # consecutive integers -> [start, end)
        out, start, prev = [], None, None
        for c in cells:
            if start is None:
                start = prev = c
            elif c == prev + 1:
                prev = c
            else:
                out.append((start, prev + 1))
                start = prev = c
        if start is not None:
            out.append((start, prev + 1))
        return out

    U = lambda x: x / 16
    V = lambda y: 1 - y / 16                 # OBJ v is bottom-up (the BIN tool flips it)
    for y in range(16):                      # faces and top/bottom edges: one quad per run of pixels in a row
        for xa, xb in runs([x for x in range(16) if solid(x, y)]):
            uv = [(U(xa) + e, V(y + 1) + e), (U(xb) - e, V(y + 1) + e), (U(xb) - e, V(y) - e), (U(xa) + e, V(y) - e)]
            o.quad([P(xa, y + 1, t), P(xb, y + 1, t), P(xb, y, t), P(xa, y, t)], uv, N((0, 0, 1)))
            o.quad([P(xb, y + 1, -t), P(xa, y + 1, -t), P(xa, y, -t), P(xb, y, -t)], [uv[1], uv[0], uv[3], uv[2]], N((0, 0, -1)))
        vm = V(y + 0.5)
        for xa, xb in runs([x for x in range(16) if solid(x, y) and not solid(x, y - 1)]):
            o.quad([P(xa, y, t), P(xb, y, t), P(xb, y, -t), P(xa, y, -t)], [(U(xa) + e, vm), (U(xb) - e, vm), (U(xb) - e, vm), (U(xa) + e, vm)], N((0, 1, 0)))
        for xa, xb in runs([x for x in range(16) if solid(x, y) and not solid(x, y + 1)]):
            o.quad([P(xa, y + 1, -t), P(xb, y + 1, -t), P(xb, y + 1, t), P(xa, y + 1, t)], [(U(xa) + e, vm), (U(xb) - e, vm), (U(xb) - e, vm), (U(xa) + e, vm)], N((0, -1, 0)))
    for x in range(16):                      # left/right edges: one quad per run in a column
        um = U(x + 0.5)
        for ya, yb in runs([y for y in range(16) if solid(x, y) and not solid(x - 1, y)]):
            o.quad([P(x, yb, -t), P(x, yb, t), P(x, ya, t), P(x, ya, -t)], [(um, V(yb) + e), (um, V(yb) + e), (um, V(ya) - e), (um, V(ya) - e)], N((-1, 0, 0)))
        for ya, yb in runs([y for y in range(16) if solid(x, y) and not solid(x + 1, y)]):
            o.quad([P(x + 1, yb, t), P(x + 1, yb, -t), P(x + 1, ya, -t), P(x + 1, ya, t)], [(um, V(yb) + e), (um, V(yb) + e), (um, V(ya) - e), (um, V(ya) - e)], N((1, 0, 0)))


def tnt_atlas(jar):
    """32x32: side | top / bottom | (unused)."""
    a = Image.new('RGBA', (32, 32), (0, 0, 0, 255))
    a.paste(block_image(jar, 'tnt_side'), (0, 0))
    a.paste(block_image(jar, 'tnt_top'), (16, 0))
    a.paste(block_image(jar, 'tnt_bottom'), (0, 16))
    return a


def tnt_cube(o, s):
    h = s / 2

    def tile(tx, ty):                        # the tile's corners in OBJ uv (v bottom-up): bl, br, tr, tl
        u0, u1 = tx * 16 / 32, (tx + 1) * 16 / 32
        v0, v1 = 1 - (ty + 1) * 16 / 32, 1 - ty * 16 / 32
        return [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]

    side, top, bottom = tile(0, 0), tile(1, 0), tile(0, 1)
    o.quad([(-h, 0, h), (h, 0, h), (h, s, h), (-h, s, h)], side, (0, 0, 1))
    o.quad([(h, 0, -h), (-h, 0, -h), (-h, s, -h), (h, s, -h)], side, (0, 0, -1))
    o.quad([(h, 0, h), (h, 0, -h), (h, s, -h), (h, s, h)], side, (1, 0, 0))
    o.quad([(-h, 0, -h), (-h, 0, h), (-h, s, h), (-h, s, -h)], side, (-1, 0, 0))
    o.quad([(-h, s, h), (h, s, h), (h, s, -h), (-h, s, -h)], top, (0, 1, 0))
    o.quad([(-h, 0, -h), (h, 0, -h), (h, 0, h), (-h, 0, h)], bottom, (0, -1, 0))


def main():
    jar_path, out = sys.argv[1:3]
    jar = zipfile.ZipFile(jar_path)
    work = os.path.join(out, 'work')
    shutil.rmtree(work, ignore_errors=True)
    for sub in ('SS/cmn', 'ImagePackHD', 'ImagePack'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    only = {int(x, 16) for x in sys.argv[3:]}   # optional: just these ids (hex)
    for rid, (name, p) in sorted(WORLD.items()):
        if only and rid not in only:
            continue
        pack = '220000%02X' % rid
        d = os.path.join(work, 'itm%02x' % rid)
        os.makedirs(d)
        o = Obj()
        if name == 'tnt':
            img = tnt_atlas(jar)
            tnt_cube(o, p)
        else:
            img = item_image(jar, name)
            card_voxels(o, img, p, 0)
        open(os.path.join(d, 'item.obj'), 'w').write(o.text())
        open(os.path.join(d, 'item.idxmaterial'), 'w').write(MATERIAL)
        # the model's root bone at its middle, like RE4's own item models (pesetas: 0.84 of a 2.0 tall pouch):
        # the "Take it?" / "You got" view centres the item on that bone (at the bottom, the top went off screen)
        ys = [v[1] for v in o.V]
        mid = (min(ys) + max(ys)) / 2 if ys else 0.0
        open(os.path.join(d, 'item.idxuubin'), 'w').write(UUBIN.replace('BoneLine:   0   -1   0.0  0.0  0.0', 'BoneLine:   0   -1   0.0  0.0  %.6f' % mid))
        open(os.path.join(d, 'item.idxuhdtpl'), 'w').write(
            'TPL_000\nPackID:%s\nTextureID:0000\nPixelFormatType:0E\nWidth:%d\nHeight:%d\n' % (pack, img.size[0], img.size[1]))
        mono(os.path.abspath(BIN_TOOL), 'item.obj', 'item.idxmaterial', cwd=d)
        mono(os.path.abspath(BIN_TOOL), 'item.idxuhdtpl', cwd=d)
        shutil.copy(os.path.join(d, 'item.BIN'), os.path.join(out, 'SS/cmn/itm%02x.bin' % rid))
        shutil.copy(os.path.join(d, 'item.TPL'), os.path.join(out, 'SS/cmn/itm%02x.tpl' % rid))
        # the texture pack: one texture
        pd = os.path.join(d, 'pack')
        os.makedirs(os.path.join(pd, pack.lower()))
        open(os.path.join(pd, pack.lower(), '0000.dds'), 'wb').write(dds_dxt1(img))
        open(os.path.join(pd, '%s.pack.yz2.idxpack' % pack.lower()), 'w').write('# RE4 PACK TOOL\nMAGIC:%s\n' % pack.lower())
        mono(os.path.abspath(PACK_TOOL), '-bat', '%s.pack.yz2.idxpack' % pack.lower(), cwd=pd)
        for sub in ('ImagePackHD', 'ImagePack'):
            shutil.copy(os.path.join(pd, '%s.pack.yz2' % pack.lower()), os.path.join(out, sub))
        print('itm%02x -> %s (%d bytes)' % (rid, name, os.path.getsize(os.path.join(d, 'item.BIN'))))


if __name__ == '__main__':
    main()
