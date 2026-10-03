# Packs every room's SAT collision (OBJ from JADERLINK's RE4-SAT-EAT-TOOL) into RECraft_rooms.bin
import glob, os, struct
rooms = {}
for d in sorted(glob.glob('src/St*/r*/')):
    rid = int(os.path.basename(d.rstrip('/'))[1:], 16); tris = []
    for f in sorted(glob.glob(d + '*_[0-9].obj')):
        V = []
        for l in open(f):
            if l.startswith('v '): V.append(tuple(map(float, l.split()[1:4])))
            elif l.startswith('f '):
                idx = [int(p.split('/')[0]) - 1 for p in l.split()[1:]]
                for k in range(1, len(idx) - 1): tris.append((V[idx[0]], V[idx[k]], V[idx[k + 1]]))
    rooms[rid] = tris
with open('RECraft_rooms.bin', 'wb') as f:
    ids = sorted(rooms); f.write(b'R4CR' + struct.pack('<II', 1, len(ids)))
    off = 12 + len(ids) * 12
    for r in ids: f.write(struct.pack('<III', r, len(rooms[r]), off)); off += len(rooms[r]) * 36
    for r in ids:
        for t in rooms[r]: f.write(struct.pack('<9f', *t[0], *t[1], *t[2]))
