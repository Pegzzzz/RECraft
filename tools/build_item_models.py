#!/usr/bin/env python3
"""RECraft: Minecraft item models for RE4's attache case and merchant.

Builds, from YOUR OWN game files and YOUR OWN Minecraft jar (nothing of either is shipped with RECraft):
  * SS/<lang>/ss_pzzl.dat   - the case/shop piece models of the weapons RECraft maps, replaced by
                              Minecraft-style item models (each pixel of the 16x16 item texture becomes a
                              small block, like Minecraft draws held items)
  * ImagePackHD/<pack>.pack - the textures for them, appended to the language's sub-screen image pack

Needs: python3 + Pillow, mono, JADERLINK's RE4_UHD_BIN_TPL_TOOL.exe and RE4_UHD_PACK_TOOL.exe, and the files
already decompressed from .lfs (re4lfs.exe).

usage: build_item_models.py <minecraft-client.jar> <ss_pzzl.dat> <pack file> <pack id hex> <out dir>
"""
import io, math, os, struct, subprocess, sys, zipfile, shutil
from PIL import Image

BIN_TOOL = os.environ.get('BIN_TOOL', 'JADERLINK_RE4_UHD_BIN_TPL_TOOL.exe')
PACK_TOOL = os.environ.get('PACK_TOOL', 'RE4_UHD_PACK_TOOL.exe')

# RE4 item id -> (Minecraft item texture, rotate 45 degrees so the item lies along the case slot)
TARGETS = {
    35: ('wooden_sword', True), 33: ('stone_sword', True), 37: ('iron_sword', True), 39: ('diamond_sword', True),
    3: ('netherite_sword', True), 41: ('netherite_sword', True), 42: ('netherite_sword', True), 55: ('netherite_sword', True),
    44: ('stone_axe', True), 148: ('iron_axe', True), 45: ('diamond_axe', True), 52: ('netherite_axe', True),
    46: ('bow', True), 47: ('bow', True),
    48: ('enchanted_book', False), 62: ('enchanted_book', False),
    0: ('arrow', True),   # the shared ammo model (every ammo type uses model 0 with its own texture)
    6: ('cooked_beef', False),    # herbs (green/red/yellow share model 6) -> steak
    18: ('cooked_beef', False),   # mixed herbs (share model 18)
}
# items drawn with a shared model and their own texture entry: (model owner, ids whose texture follows it)
# the three grenades share model 1 in the case, each with its own textures: a flat card per item (cut out
# with an opacity map) so each can show its own Minecraft item
CARDS = {1: 'tnt', 2: 'fire_charge', 14: 'ender_pearl'}
SHARED_TEXTURES = [(0, [4, 7, 24, 26, 32, 106]),             # ammo -> arrow
                   (6, [25, 28]), (18, [19, 20, 21, 22, 168])]   # herbs -> steak

MATERIAL = """UseMaterial:MATERIAL_000
material_flag:00
diffuse_map:0
bump_map:255
opacity_map:255
generic_specular_map:255
intensity_specular_r:0
intensity_specular_g:0
intensity_specular_b:0
unk_08:0
unk_09:0
specular_scale:00
unk_11:0
custom_specular_map:255
"""
UUBIN = """UseAlternativeNormals:True
UseWeightMap:True
EnableAdjacentBoneTag:True
EnableBonepairTag:False
UseVertexColor:False
ObjFileUseBone:0

BoneLine:   0   -1   0.0  0.0  0.0
"""


def mono(*args, cwd):
    # the JADERLINK tools are .NET programs: Windows runs them directly, Linux / macOS through mono
    cmd = list(args) if os.name == 'nt' else ['mono', *args]
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_dat(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    offs = list(struct.unpack_from('<%dI' % n, d, 16)) + [len(d)]
    exts = [d[16 + 4 * n + 4 * i:16 + 4 * n + 4 * i + 4] for i in range(n)]
    entries = [d[offs[i]:offs[i + 1]] for i in range(n)]
    return exts, entries


def write_dat(path, exts, entries):
    n = len(entries)
    head = 16 + 8 * n
    pos = (head + 31) & ~31
    offs, body = [], bytearray()
    for e in entries:
        offs.append(pos + len(body))
        body += e
        body += b'\0' * ((-len(body)) % 32)
    out = struct.pack('<I12x', n) + struct.pack('<%dI' % n, *offs) + b''.join(exts)
    out += b'\0' * (pos - len(out))
    open(path, 'wb').write(out + body)


def item_image(jar, name):
    img = Image.open(io.BytesIO(jar.read('assets/minecraft/textures/item/%s.png' % name))).convert('RGBA')
    return img.crop((0, 0, 16, 16)) if img.size != (16, 16) else img


def dds_dxt1(img):
    """NxN pixel art (16 or 32) -> 128x128 DXT1 (each pixel a solid square of 4x4 blocks) + a 64 mip."""
    N = img.size[0]
    px = img.load()
    # transparent pixels take a neighbour's colour so filtering never darkens the edges
    rgb = [[px[x, y] for x in range(N)] for y in range(N)]
    solid = [(x, y) for y in range(N) for x in range(N) if rgb[y][x][3] >= 128]
    for y in range(N):
        for x in range(N):
            if rgb[y][x][3] < 128 and solid:
                sx, sy = min(solid, key=lambda p: (p[0] - x) ** 2 + (p[1] - y) ** 2)
                rgb[y][x] = rgb[sy][sx]

    def c565(c):
        return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)

    def level(size):
        out = bytearray()
        for by in range(size // 4):
            for bx in range(size // 4):
                c = c565(rgb[(by * 4 * N) // size][(bx * 4 * N) // size])
                # c0 > c1 picks 4-colour mode; equal colours with index 0 are exact in either mode
                out += struct.pack('<HHI', c, c, 0)
        return bytes(out)

    data = level(128) + level(64)
    hdr = struct.pack('<4sIIIIIII', b'DDS ', 124, 0x000A1007, 128, 128, 128 * 128 // 2, 0, 2)
    hdr += b'\0' * 44
    hdr += struct.pack('<II4sIIIII', 32, 0x4, b'DXT1', 0, 0, 0, 0, 0)
    hdr += struct.pack('<IIIII', 0x401008, 0, 0, 0, 0)
    assert len(hdr) == 128
    return hdr + data


def tga_mask(img):
    """The image's alpha as RE4's opacity map: a 128x128 32-bit TGA, grey in every channel."""
    a = img.split()[3].point(lambda v: 255 if v >= 128 else 0).resize((128, 128), Image.NEAREST)
    m = Image.merge('RGBA', (a, a, a, a))
    buf = io.BytesIO()
    m.save(buf, 'TGA')
    return buf.getvalue()


def block_image(jar, name):
    img = Image.open(io.BytesIO(jar.read('assets/minecraft/textures/block/%s.png' % name))).convert('RGBA')
    return img.crop((0, 0, 16, 16)) if img.size != (16, 16) else img


def tnt_icon(jar):
    """Minecraft's inventory look for a block: a little isometric TNT cube, 32x32."""
    top, side = block_image(jar, 'tnt_top'), block_image(jar, 'tnt_side')
    S = 8                                          # draw at 256x256, then shrink without smoothing
    big = Image.new('RGBA', (32 * S, 32 * S), (0, 0, 0, 0))
    from PIL import ImageDraw
    d = ImageDraw.Draw(big)
    c, h = 15.0, 7.5                               # half-width, quarter-height of the 32 px cube
    O = (16.0, 1.0)                                # top corner

    def pt(x, y, z):                               # cube coords 0..1 -> icon pixels
        return ((O[0] + (x - z) * c) * S, (O[1] + (x + z) * h + (1 - y) * 2 * h * 1.06) * S)

    def face(tex, corner, du, dv, shade):
        t = tex.load()
        for v in range(16):
            for u in range(16):
                r, g, b, a = t[u, v]
                if a < 128:
                    continue
                q = []
                for (a_, b_) in ((u, v), (u + 1, v), (u + 1, v + 1), (u, v + 1)):
                    q.append(pt(*[corner[k] + du[k] * a_ / 16 + dv[k] * b_ / 16 for k in range(3)]))
                d.polygon(q, fill=(int(r * shade), int(g * shade), int(b * shade), 255))

    face(top, (0, 1, 0), (1, 0, 0), (0, 0, 1), 1.0)
    face(side, (0, 1, 1), (1, 0, 0), (0, -1, 0), 0.8)     # front-left (z = 1)
    face(side, (1, 1, 1), (0, 0, -1), (0, -1, 0), 0.6)    # front-right (x = 1)
    return big.resize((32, 32), Image.NEAREST)


def flat_obj(lo, hi, flip=False):
    """A square card, both sides, fitted to the original model's box (for models several items share)."""
    W, H = (hi[0] - lo[0]) * 0.95, (hi[1] - lo[1]) * 0.95
    s = min(W, H) / 2
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    z = 0.002
    out = ['mtllib item.mtl']
    for zz in (z, -z):
        for (x, y) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            out.append('v %.6f %.6f %.6f' % (cx + x * s, cy + y * s, zz))
    out += ['vt 0 0', 'vt 1 0', 'vt 1 1', 'vt 0 1', 'vn 0 0 1', 'vn 0 0 -1', 'g MATERIAL_000', 'usemtl MATERIAL_000']
    out += ['f 1/1/1 2/2/1 3/3/1', 'f 1/1/1 3/3/1 4/4/1', 'f 5/1/2 8/4/2 7/3/2', 'f 5/1/2 7/3/2 6/2/2']
    return '\n'.join(out) + '\n'


CARD_MATERIAL = MATERIAL.replace('material_flag:00', 'material_flag:04').replace('opacity_map:255', 'opacity_map:1')


def bounds_of(obj_text):
    vs = [list(map(float, l.split()[1:4])) for l in obj_text.splitlines() if l.startswith('v ')]
    lo = [min(v[k] for v in vs) for k in range(3)]
    hi = [max(v[k] for v in vs) for k in range(3)]
    return lo, hi


def item_obj(img16, rotate, lo, hi):
    px = img16.load()
    solid = lambda x, y: 0 <= x < 16 and 0 <= y < 16 and px[x, y][3] >= 128
    a = -math.pi / 4 if rotate else 0.0
    ca, sa = math.cos(a), math.sin(a)
    rot = lambda x, y: (x * ca - y * sa, x * sa + y * ca)
    # size: fit the opaque pixels' rotated bounding box into the original model's box
    corners = [rot(x + dx - 8, 8 - (y + dy)) for y in range(16) for x in range(16) if solid(x, y) for dx in (0, 1) for dy in (0, 1)]
    bx0, bx1 = min(c[0] for c in corners), max(c[0] for c in corners)
    by0, by1 = min(c[1] for c in corners), max(c[1] for c in corners)
    W, H = (hi[0] - lo[0]) * 0.92, (hi[1] - lo[1]) * 0.92
    s = min(W / (bx1 - bx0), H / (by1 - by0))
    sx = min(W / (bx1 - bx0), s * (1.15 if rotate else 1.0))   # at most a touch longer along the slot
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    mx, my = (bx0 + bx1) / 2, (by0 + by1) / 2
    t = s * 0.5                                 # half a pixel thick each side
    V, VT, F = [], [], []

    def P(x, y, z):
        rx, ry = rot(x, y)
        V.append(((rx - mx) * sx + cx, (ry - my) * s + cy, z))
        return len(V)

    def quad(pts, u, v, n):
        VT.append((u, v))
        ti = len(VT)
        ids = [P(*p) for p in pts]
        F.append((ids, ti, n))

    for y in range(16):
        for x in range(16):
            if not solid(x, y):
                continue
            u, v = (x + 0.5) / 16, 1 - (y + 0.5) / 16   # OBJ v is bottom-up (the BIN tool flips it)
            X0, X1, Y0, Y1 = x - 8, x - 7, 8 - (y + 1), 8 - y
            quad([(X0, Y0, t), (X1, Y0, t), (X1, Y1, t), (X0, Y1, t)], u, v, 0)       # front
            quad([(X0, Y0, -t), (X0, Y1, -t), (X1, Y1, -t), (X1, Y0, -t)], u, v, 1)   # back
            if not solid(x - 1, y): quad([(X0, Y0, -t), (X0, Y0, t), (X0, Y1, t), (X0, Y1, -t)], u, v, 2)
            if not solid(x + 1, y): quad([(X1, Y0, t), (X1, Y0, -t), (X1, Y1, -t), (X1, Y1, t)], u, v, 3)
            if not solid(x, y - 1): quad([(X0, Y1, t), (X1, Y1, t), (X1, Y1, -t), (X0, Y1, -t)], u, v, 4)
            if not solid(x, y + 1): quad([(X0, Y0, -t), (X1, Y0, -t), (X1, Y0, t), (X0, Y0, t)], u, v, 5)
    nx, ny = rot(1, 0), rot(0, 1)
    normals = [(0, 0, 1), (0, 0, -1), (-nx[0], -nx[1], 0), (nx[0], nx[1], 0), (ny[0], ny[1], 0), (-ny[0], -ny[1], 0)]
    out = ['mtllib item.mtl']
    out += ['v %.6f %.6f %.6f' % v for v in V]
    out += ['vt %.6f %.6f' % t_ for t_ in VT]
    out += ['vn %.6f %.6f %.6f' % n for n in normals]
    out += ['g MATERIAL_000', 'usemtl MATERIAL_000']
    for ids, ti, n in F:
        a_, b_, c_, d_ = ids
        out.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (a_, ti, n + 1, b_, ti, n + 1, c_, ti, n + 1))
        out.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (a_, ti, n + 1, c_, ti, n + 1, d_, ti, n + 1))
    return '\n'.join(out) + '\n'


def main():
    jar_path, dat_path, pack_path, pack_id, out_dir = sys.argv[1:6]
    work = os.path.join(out_dir, 'work')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    jar = zipfile.ZipFile(jar_path)
    exts, entries = read_dat(dat_path)

    # 1) textures, appended to the image pack
    pack_dir = os.path.join(work, 'pack')
    os.makedirs(pack_dir)
    shutil.copy(pack_path, os.path.join(pack_dir, 'p.pack'))
    mono(os.path.abspath(PACK_TOOL), '-bat', 'p.pack', cwd=pack_dir)
    tex_dir = os.path.join(pack_dir, pack_id.lower())
    existing = sorted(int(f[:4]) for f in os.listdir(tex_dir) if f[:4].isdigit())
    next_id = existing[-1] + 1
    tex_ids = {}
    for name in sorted({t[0] for t in TARGETS.values()}):
        tex_ids[name] = next_id
        open(os.path.join(tex_dir, '%04d.dds' % next_id), 'wb').write(dds_dxt1(item_image(jar, name)))
        next_id += 1
    card_ids = {}
    for rid, name in CARDS.items():
        img = tnt_icon(jar) if name == 'tnt' else item_image(jar, name)
        open(os.path.join(tex_dir, '%04d.dds' % next_id), 'wb').write(dds_dxt1(img))
        open(os.path.join(tex_dir, '%04d.tga' % (next_id + 1)), 'wb').write(tga_mask(img))
        card_ids[rid] = (next_id, next_id + 1, img.size[0])
        next_id += 2
    mono(os.path.abspath(PACK_TOOL), '-bat', 'p.pack.idxpack', cwd=pack_dir)

    # 2) one model + texture reference per weapon
    for rid, (name, rotate) in TARGETS.items():
        d = os.path.join(work, 'm%03d' % rid)
        os.makedirs(d)
        open(os.path.join(d, 'o.BIN'), 'wb').write(entries[rid * 2])
        open(os.path.join(d, 'o.TPL'), 'wb').write(entries[rid * 2 + 1])
        mono(os.path.abspath(BIN_TOOL), 'o.BIN', 'o.TPL', cwd=d)
        lo, hi = bounds_of(open(os.path.join(d, 'o.obj')).read())
        open(os.path.join(d, 'item.obj'), 'w').write(item_obj(item_image(jar, name), rotate, lo, hi))
        open(os.path.join(d, 'item.idxmaterial'), 'w').write(MATERIAL)
        open(os.path.join(d, 'item.idxuubin'), 'w').write(UUBIN)
        open(os.path.join(d, 'item.idxuhdtpl'), 'w').write(
            'TPL_000\nPackID:%s\nTextureID:%04d\nPixelFormatType:0E\nWidth:16\nHeight:16\n' % (pack_id.upper(), tex_ids[name]))
        mono(os.path.abspath(BIN_TOOL), 'item.obj', 'item.idxmaterial', cwd=d)
        mono(os.path.abspath(BIN_TOOL), 'item.idxuhdtpl', cwd=d)
        entries[rid * 2] = open(os.path.join(d, 'item.BIN'), 'rb').read()
        entries[rid * 2 + 1] = open(os.path.join(d, 'item.TPL'), 'rb').read()
        print('item %3d -> %s (%d bytes)' % (rid, name, len(entries[rid * 2])))
    # 3) the grenades: model 1 becomes a card, every grenade gets its own two textures
    d = os.path.join(work, 'cards')
    os.makedirs(d)
    open(os.path.join(d, 'o.BIN'), 'wb').write(entries[2])
    open(os.path.join(d, 'o.TPL'), 'wb').write(entries[3])
    mono(os.path.abspath(BIN_TOOL), 'o.BIN', 'o.TPL', cwd=d)
    lo, hi = bounds_of(open(os.path.join(d, 'o.obj')).read())
    open(os.path.join(d, 'item.obj'), 'w').write(flat_obj(lo, hi))
    open(os.path.join(d, 'item.idxmaterial'), 'w').write(CARD_MATERIAL)
    open(os.path.join(d, 'item.idxuubin'), 'w').write(UUBIN)
    mono(os.path.abspath(BIN_TOOL), 'item.obj', 'item.idxmaterial', cwd=d)
    entries[2] = open(os.path.join(d, 'item.BIN'), 'rb').read()
    for rid, (dds_id, tga_id, size) in card_ids.items():
        open(os.path.join(d, 't%d.idxuhdtpl' % rid), 'w').write(
            'TPL_000\nPackID:%s\nTextureID:%04d\nPixelFormatType:0E\nWidth:%d\nHeight:%d\n\n'
            'TPL_001\nPackID:%s\nTextureID:%04d\nPixelFormatType:00\nWidth:%d\nHeight:%d\n'
            % (pack_id.upper(), dds_id, size, size, pack_id.upper(), tga_id, size, size))
        mono(os.path.abspath(BIN_TOOL), 't%d.idxuhdtpl' % rid, cwd=d)
        entries[rid * 2 + 1] = open(os.path.join(d, 't%d.TPL' % rid), 'rb').read()
        print('item %3d -> %s (card)' % (rid, CARDS[rid]))
    for owner, ids in SHARED_TEXTURES:
        for i in ids:
            if entries[i * 2 + 1]:
                entries[i * 2 + 1] = entries[owner * 2 + 1]

    write_dat(os.path.join(out_dir, 'ss_pzzl.dat'), exts, entries)
    shutil.copy(os.path.join(pack_dir, 'p.pack'), os.path.join(out_dir, os.path.basename(pack_path)))
    print('wrote', os.path.join(out_dir, 'ss_pzzl.dat'), 'and', os.path.basename(pack_path))


if __name__ == '__main__':
    main()
