# Building RECraft's Minecraft mod

Minecraft 26.x is no longer obfuscated, so the mod compiles straight against the game's jars; no
Loom/remapping is needed.

Classpath: `minecraft-26.3-client.jar` and the other jars in your Prism `libraries` folder, plus
the jars nested in `fabric-api-*.jar` (`META-INF/jars/`). Java 25:

```
javac --release 25 -cp "<all those jars>" -d out src/dev/recraft/*.java src/dev/recraft/mixin/*.java
```

Then zip `fabric.mod.json`, `recraft.mixins.json` and `out/dev/recraft/**/*.class` (keeping the `dev/recraft/` path) into
`RECraft-<version>.jar`. SkyCraft is reached through reflection, so it isn't needed to compile.
