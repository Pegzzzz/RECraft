#!/usr/bin/env python3
"""RECraft: Minecraft icons for the items the merchant shows as pictures instead of models (attache cases,
treasure maps), made from YOUR OWN Minecraft jar into YOUR OWN copy of the shop's image pack.

The shop (ss_shop) shows these items with a texture from the language's shop pack (ImagePackHD/3c00000N.pack:
3c000001 English, 3c000002 French): the table in ss_shop.cpp itemTexNo picks texture (index + 15).

usage: build_shop_icons.py <minecraft-client.jar> <3c00000N.pack> <out dir>
Needs mono and JADERLINK's RE4_UHD_PACK_TOOL.exe (env PACK_TOOL), Pillow.
"""
import io, os, shutil, subprocess, sys, zipfile
from PIL import Image

PACK_TOOL = os.environ.get('PACK_TOOL', 'RE4_UHD_PACK_TOOL.exe')
# texture index in the pack -> Minecraft item (or a recipe for one)
ICONS = {86: 'filled_map', 88: 'leather_chestplate', 89: 'iron_chestplate', 87: 'diamond_chestplate'}


def mono(*args, cwd):
    cmd = list(args) if os.name == 'nt' else ['mono', *args]
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def item(jar, name):
    img = Image.open(io.BytesIO(jar.read('assets/minecraft/textures/item/%s.png' % name))).convert('RGBA')
    img = img.crop((0, 0, 16, 16)) if img.size != (16, 16) else img
    if name == 'leather_chestplate':   # undyed leather: Minecraft tints it brown and lays the overlay on top
        r, g, b, a = img.split()
        tint = Image.merge('RGBA', (r.point(lambda v: v * 160 // 255), g.point(lambda v: v * 101 // 255),
                                    b.point(lambda v: v * 64 // 255), a))
        over = Image.open(io.BytesIO(jar.read('assets/minecraft/textures/item/leather_chestplate_overlay.png'))).convert('RGBA')
        img = Image.alpha_composite(tint, over.crop((0, 0, 16, 16)))
    return img


def icon(img16, size):
    """Like Minecraft's inventory: each pixel a crisp square, with a soft shadow so it reads on the shop panel."""
    s = (size - 8) // 16
    big = img16.resize((16 * s, 16 * s), Image.NEAREST)
    out = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    a = big.split()[3].point(lambda v: 110 if v else 0)
    shadow = Image.new('RGBA', big.size, (0, 0, 0, 0)); shadow.putalpha(a)
    o = (size - big.size[0]) // 2
    out.alpha_composite(shadow, (o + 4, o + 4))
    out.alpha_composite(big, (o, o))
    return out


def main():
    jar_path, pack_path, out_dir = sys.argv[1:4]
    jar = zipfile.ZipFile(jar_path)
    work = os.path.join(out_dir, 'work_icons'); shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
    shutil.copy(pack_path, os.path.join(work, 'p.pack'))
    mono(os.path.abspath(PACK_TOOL), '-bat', 'p.pack', cwd=work)
    sub = [d for d in os.listdir(work) if os.path.isdir(os.path.join(work, d))][0]
    for idx, name in ICONS.items():
        f = os.path.join(work, sub, '%04d.tga' % idx)
        old = Image.open(f)
        icon(item(jar, name), old.size[0]).save(f, 'TGA')
        print('icon %d -> %s' % (idx, name))
    mono(os.path.abspath(PACK_TOOL), '-bat', 'p.pack.idxpack', cwd=work)
    shutil.copy(os.path.join(work, 'p.pack'), os.path.join(out_dir, os.path.basename(pack_path)))
    print('wrote', os.path.join(out_dir, os.path.basename(pack_path)))


if __name__ == '__main__':
    main()
