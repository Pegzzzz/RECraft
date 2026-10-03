# Installing RECraft

About 20-30 minutes the first time. You need your own copies of both games: **Resident Evil 4 (2005) Ultimate HD
Edition on Steam** and **Minecraft: Java Edition** (a Microsoft account that owns it). RECraft ships no game files;
step 3 builds the one RE4-derived file it needs from your own install.

> Back up your RE4 saves first (`Documents\My Games\...` or Steam Cloud). RECraft doesn't touch them, but it's early software.

## 1. Resident Evil 4

1. Install **[re4_tweaks](https://github.com/nipkownix/re4_tweaks)** (copy its files into `Resident Evil 4\Bin32\`,
   next to `bio4.exe`). Leave its frame-rate fixes on.
2. From the [latest RECraft release](../../releases), copy **`winmm.dll`** into the same `Bin32` folder.

That's the whole RE4 side. To remove RECraft later, delete `winmm.dll` (and `RECraft.ini`, `RECraft.log`,
`RECraft_rooms.bin`) from `Bin32`.

## 2. Minecraft (Prism Launcher)

1. Install **[Prism Launcher](https://prismlauncher.org/)** and sign in with your Microsoft account.
2. **Add Instance** → Minecraft **26.3**, mod loader **Fabric** (0.19.5 or newer). Name it e.g. `RECraft`.
3. Put these three files in the instance's `mods` folder (*Edit → Mods → View Folder*):
   - **Fabric API** for 26.3 ([Modrinth](https://modrinth.com/mod/fabric-api)) - tested with `0.161.0+26.3`
   - **SkyCraft**'s Fabric mod from [SkyCraft's releases](https://github.com/chasmlol/SkyCraft/releases) - version **0.1.2**
     (the `skycraft-fabric-…jar` / `skycraft-…jar` inside the release)
   - **`RECraft-1.11.0.jar`** from the [RECraft release](../../releases)
4. *Edit → Settings → Java*: tick **Java arguments** and enter
   `--enable-native-access=ALL-UNNAMED -Dskycraft.startHidden=true`
   (Prism downloads Java 25 by itself; 4 GB of memory is plenty.)
5. Start the instance once on its own, let it reach the title screen, then close it. That's the setup done.

## 3. Room collision (`RECraft_rooms.bin`)

Minecraft has to know where RE4's floors and walls are. This file is made from your game's room files.
Without it every room is a flat floor (you'll walk through walls and over stairs).

1. Install **[Python 3](https://www.python.org/downloads/)** (tick *Add python.exe to PATH*).
2. Download the RECraft source (*Code → Download ZIP* on this page) and unzip it.
3. Make a folder `tools\bin` in it and put these in it:
   - `re4lfs.exe` and `xcompress64.dll` from [emoose's re4-research](https://github.com/emoose/re4-research)
   - `JADERLINK_DATUDAS_TOOL.exe` (JADERLINK's RE4 DATUDAS tool) and `RE4_SAT_EAT_EXTRACT.exe`
     ([JADERLINK's RE4-SAT-EAT-TOOL](https://github.com/JADERLINK/RE4-SAT-EAT-TOOL)) - both from [JADERLINK's GitHub](https://github.com/JADERLINK)
4. Open PowerShell in the `tools` folder and run:
   ```
   powershell -ExecutionPolicy Bypass -File .\build_rooms.ps1
   ```
   If RE4 isn't in the default Steam folder, add `-Game "D:\SteamLibrary\steamapps\common\Resident Evil 4"`.
   It works on copies, takes a few minutes, and puts `RECraft_rooms.bin` (about 6 MB) in `Bin32` for you.

## 4. Play

1. Start **Resident Evil 4** from Steam and load a save or start a new game.
2. Start the **RECraft instance in Prism**. Minecraft hides its window, opens its own world, and takes over
   Leon within a few seconds (you'll see Minecraft's hotbar and hearts in RE4).
3. Play. Quit RE4 normally; Minecraft saves and closes by itself.

**Controls:** Minecraft's own keys for everything (move, jump, sneak, attack, use, inventory, hotbar), plus:

| Key | |
|---|---|
| **F** | RE4's action button: doors, ladders, items, the merchant |
| **Esc** | RE4: skip a cutscene, pause menu |
| **O** | Minecraft's pause / options menu |
| **F6** | hand Leon back to RE4's own controls (or back to Minecraft) |

Your Minecraft gear comes from Leon's attache case: buy and tune up weapons at RE4's merchant (see the README).

## Optional: Minecraft-looking items

RE4's items on the floor, in the attache case and at the merchant can look like Minecraft items (swords, bow,
arrows, steak, emeralds...). These are generated from **your own** Minecraft jar - see [tools/README.md](tools/README.md).

## Settings and troubleshooting

- `Bin32\RECraft.ini` (made on first start): difficulty, shared health, what shows in cutscenes, and performance
  switches (`OverlayHalfRes`, `HalfRateVsync`, `LowLatency`, `MinecraftLowPriority`). Save and restart RE4 after editing.
- `Bin32\RECraft.log`: what the RE4 side did (it says *ready*, *Minecraft CONNECTED*, *puppet ON* when all is well).
- Nothing happens when Minecraft starts: check its `logs\latest.log` for `SkyCraft: linked` and `[RECraft] Minecraft
  side ready`; make sure all three mods are in `mods` and the Java arguments are set.
- Stuck somewhere: press **F6** to give Leon back to RE4's own controls, move, then F6 again.
