# Building RECraft's Minecraft mod

Minecraft 26.x is no longer obfuscated, so the mod compiles straight against the game's jars; no
Loom/remapping is needed.

Put these jars in one folder (`CP`): `minecraft-26.3-client.jar` and the other jars in your Prism `libraries` folder,
the jars nested in `fabric-api-*.jar` (`META-INF/jars/`) and `mixinextras-fabric-*.jar` from fabric-loader's
`META-INF/jars/`. Then, with JDK 25:

```
cd minecraft-mod
CP=<that folder> JAVAC=<jdk-25>/bin/javac OUT=build sh build.sh
```

It writes `build/RECraft-<version>.jar` (classes, `res/`, `fabric.mod.json`, both mixin configs, the licence and
third-party notices).

Layout: `src/dev/recraft/` is RECraft's own game logic (health, merchant gear, blocks stopping enemies, sweeps,
throwables...); `src/dev/recraft/core/` is the link to RE4 (shared memory, collision, camera/HUD export, enemies,
digging), a modified copy of [SkyCraft](https://github.com/chasmlol/SkyCraft)'s Fabric mod by chasmlol (MIT, see
`THIRD-PARTY-NOTICES.md`). `tools/fork_core.py` is the one-off script that renamed that copy.
