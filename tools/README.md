# Room collision (RECraft_rooms.bin)

**On Windows, `build_rooms.ps1` does all of this for you** (see [INSTALL.md](../INSTALL.md), step 3). By hand:

RECraft streams each RE4 room's collision to Minecraft. That data comes from **your own copy** of RE4 and
is not distributed here. To build `RECraft_rooms.bin`:

1. Get JADERLINK's tools: RE4 LFS decompressor (`re4lfs`), DATUDAS tool, and RE4-SAT-EAT-TOOL.
2. For every room file `BIO4\St*\r*.udas.lfs`: decompress it (`re4lfs`), unpack the `.udas` (DATUDAS
   tool), and convert each `.SAT` inside to OBJ (`RE4_SAT_EAT_EXTRACT <file>.SAT UHD`).
   Keep the layout `src/StX/rXXX/*_0.obj` (one folder per room, named after the room).
3. Run `python build_rooms.py` in the folder containing `src/`. It writes `RECraft_rooms.bin`.
4. Put it next to `winmm.dll` in `Resident Evil 4\Bin32`.

Format: `"R4CR"`, version 1, room count, then `{room, triangle count, offset}` entries and the
triangles (9 floats each, the tool's units: x100 = RE4 units).

# Minecraft item models in the attache case and the merchant (optional)

RECraft can show the weapons it maps as Minecraft items (swords, axes, bow, enchanted book, arrows) in
RE4's attache case and shop. The models are generated from **your own** RE4 files and **your own**
Minecraft jar; nothing of either is distributed here.

1. Decompress (`re4lfs`) `BIO4\SS\<lang>\ss_pzzl.dat.lfs` and the language's sub-screen image pack
   `BIO4\ImagePackHD\3b00000N.pack.lfs` (English `3b000001`, French `3b000002`; the pack id is in the
   weapon `.TPL`s).
2. Run, with mono, Pillow and JADERLINK's RE4_UHD_BIN_TPL_TOOL / RE4_UHD_PACK_TOOL:

       BIN_TOOL=…/JADERLINK_RE4_UHD_BIN_TPL_TOOL.exe PACK_TOOL=…/RE4_UHD_PACK_TOOL.exe \
       python3 build_item_models.py <minecraft-26.x-client.jar> ss_pzzl.dat 3b000001.pack 3B000001 out

3. Back up the two `.lfs` files, then put `out/ss_pzzl.dat` in `BIO4\SS\<lang>\` and `out/3b000001.pack` in
   `BIO4\ImagePackHD\` (RE4 reads uncompressed files when the `.lfs` is gone). Restore the `.lfs` files to undo.

Each item pixel becomes a small block (like Minecraft draws held items), sized to the RE4 weapon's slot.
Item names and descriptions are changed by the DLL at run time (`dll/names.cpp`), no files needed.

The three grenades share one model in the case, so each becomes a flat Minecraft item card (TNT shown as
its little block icon, the fire charge, the ender pearl) cut out with an opacity map.

# Minecraft items lying in the world (optional)

Every room carries its own copies of its item models; RECraft's DLL (`dll/items.cpp`) swaps them, when a room
loads, for the models in `BIO4\SS\cmn\itmXX.bin/.tpl` (XX = item id in hex) with textures in
`ImagePack(HD)\220000XX.pack.yz2`. `build_world_items.py` makes those from your Minecraft jar: every kind of ammo
becomes an arrow, herbs a steak, the hand grenade a TNT block, the incendiary a fire charge, the flash grenade an
ender pearl, handguns swords, shotguns axes, rifles a bow and the TMP an enchanted book.

1. `python3 build_world_items.py <minecraft-26.x-client.jar> out`
2. Rename the game's `SS\cmn\itmXX.bin.lfs`, `itmXX.tpl` and `220000XX.pack.yz2.lfs` files for the same ids out
   of the way, then copy `out/SS` and `out/ImagePack*` into `BIO4`. Remove them (and rename the originals back)
   to undo.
