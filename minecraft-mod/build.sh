#!/bin/sh
# Builds RECraft-<version>.jar with a plain JDK 25 (Minecraft 26.x isn't obfuscated: no Loom needed).
# CP = a folder holding the Minecraft 26.3 client jar, every jar of Prism's libraries folder, the jars nested in
# fabric-api-*.jar (META-INF/jars/) and fabric-loader's nested mixinextras-fabric jar.
set -e
CP=${CP:?set CP to the folder of jars}; JAVAC=${JAVAC:-javac}; OUT=${OUT:-build}
VER=$(python3 -c "import json;print(json.load(open('fabric.mod.json'))['version'])")
rm -rf "$OUT/classes" "$OUT/jar"; mkdir -p "$OUT/classes" "$OUT/jar/META-INF"
find src -name '*.java' > "$OUT/srcs.txt"
"$JAVAC" --release 25 -nowarn -proc:none -cp "$(ls "$CP"/*.jar | tr '\n' ':')" -d "$OUT/classes" @"$OUT/srcs.txt"
cp -r "$OUT/classes/dev" "$OUT/jar/"
cp -r res/* "$OUT/jar/"
cp fabric.mod.json recraft.mixins.json "$OUT/jar/"
cp ../LICENSE "$OUT/jar/META-INF/LICENSE"
cp ../THIRD-PARTY-NOTICES.md "$OUT/jar/META-INF/THIRD-PARTY-NOTICES.md"
printf 'Manifest-Version: 1.0\r\n\r\n' > "$OUT/jar/META-INF/MANIFEST.MF"
rm -f "$OUT/RECraft-$VER.jar"
(cd "$OUT/jar" && zip -qr "../RECraft-$VER.jar" META-INF fabric.mod.json *.json dev assets data)
echo "$OUT/RECraft-$VER.jar"
