#!/usr/bin/env python3
"""RECraft: the Del Lago harpoons become Minecraft spears.

The boat's archive Em/pl0f.udas holds the harpoon model (file 3, a .BIN) next to the boat's texture list (file 2,
a .TPL, shared by the boat and the harpoon); the textures sit in ImagePack(HD)/0100000f.pack.yz2. This adds your
own Minecraft jar's spear texture to that pack, builds a spear in Minecraft's held-item style (each pixel a small
block) the harpoon's size and direction, points the harpoon at it, and repacks the archive.

usage: build_harpoon.py <minecraft-client.jar> <pl0f.udas> <ImagePackHD 0100000f.pack.yz2> <ImagePack 0100000f.pack.yz2> <out>
Needs mono, Pillow and JADERLINK's tools (env BIN_TOOL, PACK_TOOL, UDAS_TOOL). Files already decompressed (re4lfs).
"""
import io, math, os, shutil, sys, zipfile
from PIL import Image, ImageOps
from build_item_models import BIN_TOOL, PACK_TOOL, MATERIAL, UUBIN, mono, dds_dxt1

UDAS_TOOL = os.environ.get('UDAS_TOOL', 'JADERLINK_DATUDAS_TOOL.exe')
SPEAR = 'iron_spear_in_hand'
LENGTH = (-12.6368752, 6.871875)   # the harpoon's extent along z in the tool's units (x100 = mm): butt .. tip


def spear_obj(img):
    """The 32x32 held-spear sprite (tip top-left) as pixel blocks in the y-z plane, tip toward +z."""
    N = img.size[0]
    px = img.load()
    solid = lambda x, y: 0 <= x < N and 0 <= y < N and px[x, y][3] >= 128
    # along the diagonal: a = how far from the tip, b = across
    r2 = math.sqrt(0.5)
    A = lambda x, y: (x + y) * r2
    B = lambda x, y: (x - y) * r2
    cells = [(x, y) for y in range(N) for x in range(N) if solid(x, y)]
    a0 = min(A(x, y) for x, y in cells); a1 = max(A(x + 1, y + 1) for x, y in cells)
    s = (LENGTH[1] - LENGTH[0]) / (a1 - a0)
    t = s * 0.5
    V, VT, F = [], [], []

    def P(x, y, side):          # sprite point -> model (x: thickness, y: across, z: along, tip at +z)
        V.append((side, B(x, y) * s, LENGTH[1] - (A(x, y) - a0) * s))
        return len(V)

    def quad(pts, u, v, n):
        VT.append((u, v)); ti = len(VT)
        F.append(([P(*p) for p in pts], ti, n))

    for x, y in cells:
        u, v = (x + 0.5) / N, 1 - (y + 0.5) / N
        quad([(x, y + 1, t), (x + 1, y + 1, t), (x + 1, y, t), (x, y, t)], u, v, 0)
        quad([(x, y + 1, -t), (x, y, -t), (x + 1, y, -t), (x + 1, y + 1, -t)], u, v, 1)
        if not solid(x - 1, y): quad([(x, y + 1, -t), (x, y + 1, t), (x, y, t), (x, y, -t)], u, v, 2)
        if not solid(x + 1, y): quad([(x + 1, y + 1, t), (x + 1, y + 1, -t), (x + 1, y, -t), (x + 1, y, t)], u, v, 3)
        if not solid(x, y - 1): quad([(x, y, t), (x + 1, y, t), (x + 1, y, -t), (x, y, -t)], u, v, 4)
        if not solid(x, y + 1): quad([(x, y + 1, -t), (x + 1, y + 1, -t), (x + 1, y + 1, t), (x, y + 1, t)], u, v, 5)
    d = lambda dx, dy: (0.0, (dx - dy) * r2, -(dx + dy) * r2)   # sprite direction -> model direction
    normals = [(1, 0, 0), (-1, 0, 0), d(-1, 0), d(1, 0), d(0, -1), d(0, 1)]
    out = ['mtllib item.mtl'] + ['v %.6f %.6f %.6f' % v for v in V] + ['vt %.6f %.6f' % q for q in VT]
    out += ['vn %.6f %.6f %.6f' % n for n in normals] + ['g MATERIAL_000', 'usemtl MATERIAL_000']
    for ids, ti, n in F:
        a, b, c, e = ids
        out.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (a, ti, n + 1, b, ti, n + 1, c, ti, n + 1))
        out.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (a, ti, n + 1, c, ti, n + 1, e, ti, n + 1))
    return '\n'.join(out) + '\n'


def add_texture(pack_path, work, dds):
    """Appends `dds` to a .pack.yz2 and returns its texture id."""
    os.makedirs(work, exist_ok=True)
    name = os.path.basename(pack_path)              # 0100000f.pack.yz2
    shutil.copy(pack_path, os.path.join(work, name))
    mono(os.path.abspath(PACK_TOOL), '-bat', name, cwd=work)
    tex_dir = os.path.join(work, name.split('.')[0])
    ids = sorted(int(f[:4]) for f in os.listdir(tex_dir) if f[:4].isdigit())
    tid = ids[-1] + 1
    open(os.path.join(tex_dir, '%04d.dds' % tid), 'wb').write(dds)
    mono(os.path.abspath(PACK_TOOL), '-bat', name + '.idxpack', cwd=work)
    return tid, os.path.join(work, name)


def main():
    jar_path, udas, pack_hd, pack_sd, out = sys.argv[1:6]
    jar = zipfile.ZipFile(jar_path)
    work = os.path.join(out, 'work'); shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
    img = Image.open(io.BytesIO(jar.read('assets/minecraft/textures/item/%s.png' % SPEAR))).convert('RGBA')
    dds = dds_dxt1(img)
    tid, hd = add_texture(pack_hd, os.path.join(work, 'hd'), dds)
    tid2, sd = add_texture(pack_sd, os.path.join(work, 'sd'), dds)
    assert tid == tid2, (tid, tid2)

    # the archive
    ud = os.path.join(work, 'udas'); os.makedirs(ud)
    shutil.copy(udas, os.path.join(ud, 'pl0f.udas'))
    mono(os.path.abspath(UDAS_TOOL), '-bat', '-idx', 'pl0f.udas', cwd=ud)
    files = os.path.join(ud, 'pl0f')
    m = os.path.join(work, 'model'); os.makedirs(m)
    shutil.copy(os.path.join(files, 'pl0f_003.BIN'), os.path.join(m, 'o.BIN'))
    shutil.copy(os.path.join(files, 'pl0f_002.TPL'), os.path.join(m, 'o.TPL'))
    mono(os.path.abspath(BIN_TOOL), 'o.BIN', 'o.TPL', cwd=m)
    tpl = open(os.path.join(m, 'o.idxuhdtpl')).read()
    n = tpl.count('TPL_0')
    tpl = tpl.rstrip() + '\n\n\nTPL_%03d\nPackID:0100000F\nTextureID:%04d\nPixelFormatType:0E\nWidth:32\nHeight:32\n' % (n, tid)
    open(os.path.join(m, 'item.idxuhdtpl'), 'w').write(tpl)
    open(os.path.join(m, 'item.obj'), 'w').write(spear_obj(img))
    open(os.path.join(m, 'item.idxmaterial'), 'w').write(MATERIAL.replace('diffuse_map:0', 'diffuse_map:%d' % n))
    open(os.path.join(m, 'item.idxuubin'), 'w').write(UUBIN)
    mono(os.path.abspath(BIN_TOOL), 'item.obj', 'item.idxmaterial', cwd=m)
    mono(os.path.abspath(BIN_TOOL), 'item.idxuhdtpl', cwd=m)
    shutil.copy(os.path.join(m, 'item.BIN'), os.path.join(files, 'pl0f_003.BIN'))
    shutil.copy(os.path.join(m, 'item.TPL'), os.path.join(files, 'pl0f_002.TPL'))
    os.remove(os.path.join(ud, 'pl0f.udas'))
    mono(os.path.abspath(UDAS_TOOL), '-bat', 'pl0f.idx', cwd=ud)
    for sub, src in (('Em', os.path.join(ud, 'pl0f.udas')), ('ImagePackHD', hd), ('ImagePack', sd)):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
        shutil.copy(src, os.path.join(out, sub, os.path.basename(src)))
    print('spear texture %04d (TPL entry %d); wrote' % (tid, n), out)


if __name__ == '__main__':
    main()
