# RECraft

**Minecraft, inside Resident Evil 4.** You play Resident Evil 4 (2005, Ultimate HD Edition on Steam)
as a Minecraft player: Minecraft runs hidden next to the game and drives Leon — movement, inventory,
combat, building — while RE4 draws everything. Built on the idea and protocol of
[SkyCraft](https://github.com/chasmlol/SkyCraft) (Minecraft inside Skyrim), with RE4 in Skyrim's place.

> Fan project. Not affiliated with Capcom, Mojang or Microsoft. No game files or assets are included:
> you need your own copies of both games.

## What it does

- **Minecraft is the player.** Minecraft's movement, physics, inventory, hotbar and HUD; Leon's model is
  hidden and RE4's camera sits at the Minecraft player's eye (FOV follows Minecraft's).
- **RE4's world is solid in Minecraft.** Every room's collision is streamed to Minecraft, so you walk,
  jump and build on RE4's floors. Sneaking stops you at ledges.
- **Combat both ways.** Swords, axes, bows hit RE4's Ganados, dogs and crows through RE4's own damage
  system (flinches, knock-backs, deaths, item drops); boxes, barrels and windows break.
- **One health bar.** RE4's and Minecraft's health are linked both ways — herbs fill your hearts,
  Minecraft regeneration heals Leon, dying in either game kills you in both.
- **Sword sweeps.** A fully charged sword swing also hits every RE4 enemy in a wide arc in front of you
  (40% of the hit, up to 85% with Sweeping Edge), like Minecraft's sweep but reaching RE4's crowds.
- **RE4's grenades are Minecraft throwables.** Hand grenades become TNT (it lights when you place it and
  goes off 2.5 s later, hurting everything near it, you included, but breaking no blocks), flash grenades
  become ender pearls (they throw you where you look and stop at RE4's walls), incendiary grenades become
  fire charges (they set the enemy in your sights on fire).
- **The axe is the heavy weapon:** one swing every 20 seconds, 2.5x the damage, and it always knocks down.
- **Your blocks are real.** Blocks you place appear in RE4, are hidden behind its walls, and stop enemies.
  Minecraft's block outline, breaking cracks, dropped items and arrows show up in RE4 too.
- **RE4's scenery reacts.** Shut doors block you, padlocks break, and the things RE4 makes you shoot
  (spinels and other hanging treasure, blue medallions, lamps, boarded passages, bear traps and tripwire bombs) take
  your arrows and swings.
- **RE4 still plays out.** Cutscenes, doors, ladders, vaults and grabs run as RE4 made them, with your
  Minecraft character in Leon's place; Minecraft picks Leon up when they end.
- **RE4's merchant decides your Minecraft gear.** Buy and tune up weapons in RE4's own shop and your
  Minecraft gear follows Leon's attache case:

  | RE4 | Minecraft |
  |---|---|
  | Handgun / Punisher / Red9 / Blacktail / magnums & Matilda | wooden / stone / iron / diamond / netherite sword |
  | (none) / Shotgun / Riot Gun / Striker / Chicago Typewriter | wooden / stone / iron / diamond / netherite axe |
  | Rifle, Rifle (semi-auto) | bow with Power I / II |
  | Attache case S / M / L / XL | no armour / leather / iron / diamond |
  | TMP | Protection on the armour |
  | Firepower / firing speed / reload / capacity | Sharpness, Sweeping Edge, Fire Aspect, Knockback (bow: Power, Punch, Flame, Infinity) |

  In RE4's case and shop these items carry their Minecraft names and descriptions (English or French,
  following the game's language), and with the optional item models (see [tools/](tools/)) they look like
  Minecraft items too.

  Ammo Leon picks up turns into arrows, crates and barrels you break give planks, and herbs become
  steak (one per herb) - food is how you heal, so hunger is part of the fight. Merchant gear doesn't wear
  out (like RE4's weapons); arrows and blocks get used up. Worn armour softens RE4's hits.

## Requirements

| | |
|---|---|
| Resident Evil 4 UHD (Steam, v1.1.0) | with [re4_tweaks](https://github.com/nipkownix/re4_tweaks) installed |
| Minecraft Java 26.3 | via [Prism Launcher](https://prismlauncher.org/), Fabric Loader 0.19.5+, Fabric API |
| [SkyCraft](https://github.com/chasmlol/SkyCraft) 0.1.2 | the Minecraft-side link this builds on |

## Install

Step by step: **[INSTALL.md](INSTALL.md)**. In short: `winmm.dll` from the [release](../../releases) goes in
`Resident Evil 4\Bin32\`; `RECraft-1.12.0.jar` goes in a Prism Minecraft 26.3 Fabric instance with Fabric API and
SkyCraft; `tools\build_rooms.ps1` builds the room collision from your own game files. Start RE4, then Minecraft.

## Controls

| Key | |
|---|---|
| Minecraft's keys | everything (move, jump, sneak, attack, use, inventory, hotbar) |
| **F** | RE4's action button: doors, windows, ladders, items, the merchant |
| **Esc** | RE4: skip a cutscene, pause menu |
| **O** | Minecraft's pause / options menu |
| **F6** | hand Leon to RE4 (or back to Minecraft) by hand |
| `/recraft reset` | (Minecraft chat) empty the inventory (e.g. for a new RE4 game); gear comes back from Leon's case |

## Settings

Difficulty (in `RECraft.ini`): `FlinchCooldownMs` / `FlinchMinDamage` / `FlinchChance` (enemies only flinch from strong
hits, now and then from weaker ones; a small hit marker shows every hit), `AxeDamage` / `AxeCooldownSec`,
`MinEnemyRank` (RE4's adaptive difficulty never drops below it; default 7 = faster, pushier enemies), `EnemyDamage` (RE4's hits x this, default 1.25), `ArrowsPerAmmo` (default 0.5).


`RECraft.ini` (created next to `winmm.dll` on first start): hide Leon, Minecraft HUD and body in cutscenes,
damage scale, shared health, hiding blocks behind walls, door collision. The log is `RECraft.log` in the same folder.

Performance: `OverlayHalfRes=1` draws Minecraft's HUD at half size (4x less copying per frame), `HalfRateVsync=1`
evens out 60 fps on a 120 Hz screen, `LowLatency` keeps one frame queued.

## Building

- **winmm.dll:** `dll/build.sh` with mingw-w64 (i686) and [MinHook](https://github.com/TsudaKageyu/minhook)'s
  sources compiled to objects next to it. The DLL forwards every winmm export to the system copy.
- **Minecraft mod:** see [minecraft-mod/BUILDING.md](minecraft-mod/BUILDING.md).

## How it works (short)

`winmm.dll` is loaded by RE4. It finds RE4's structures by byte patterns (layouts from re4_tweaks' SDK and
the [RE4 GameCube decompilation](https://github.com/emoose/re4)), hooks RE4's camera, Direct3D 9 and
DirectInput, and talks to Minecraft through SkyCraft's shared-memory protocol (`Local\SkyCraft_v1`).
RECraft's Minecraft mod adds health sync, blocks and item rules through a free block of that memory.

## Credits

- [SkyCraft](https://github.com/chasmlol/SkyCraft) by chasmlol (MIT) — the protocol and Minecraft side this builds on.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-Clause).
- [re4_tweaks](https://github.com/nipkownix/re4_tweaks) and the [RE4 decompilation](https://github.com/emoose/re4) — for documenting RE4's internals.
- JADERLINK's RE4 tools — for extracting room collision.

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). RECraft itself is MIT licensed.
