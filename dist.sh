#!/bin/sh
# dist.sh - stage the release package in dist/ZZPlug (+ ZZPlug.info).
# Build first (build.ps1); the .lha is made on an Amiga from this tree, so the
# files get real protection bits. GPL-3.0-or-later, see LICENSE.
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/.*\$VER: ZZPlug \([0-9.]*\).*/\1/p' src/zzplug.c | head -1)
[ -n "$VER" ] || { echo "dist.sh: no version in src/zzplug.c" >&2; exit 1; }
D=dist/ZZPlug

# refuse to package a binary older than its sources (a forgotten build)
for f in out/ZZPlug out/ZZPlugPrefs out/JoyTest out/KeyTest; do
    [ -f "$f" ] || { echo "dist.sh: missing $f - run build.ps1" >&2; exit 1; }
    if [ -n "$(find src -newer "$f" \( -name '*.c' -o -name '*.h' \) | head -1)" ]; then
        echo "dist.sh: $f is older than its sources - run build.ps1" >&2; exit 1
    fi
done

# only the staged drawer: notes and built .lha/.adf in dist/ stay
rm -rf dist/ZZPlug dist/ZZPlug.info
mkdir -p $D/C $D/Prefs $D/Tools
cp out/ZZPlug           $D/C/
cp out/ZZPlugPrefs      $D/Prefs/
cp out/JoyTest out/KeyTest $D/Tools/
sed "s/@VER@/$VER/" dist-src/Install_ZZPlug > $D/Install_ZZPlug
sed "s/@VER@/$VER/" dist-src/README > $D/README
cp LICENSE              $D/

# icons: the committed .info files, copied as they are
I=icons
cp $I/Install_ZZPlug.info $I/README.info $D/
cp $I/ZZPlugPrefs.info  $D/Prefs/
cp $I/ZZPlug.info       dist/ZZPlug.info


echo "ZZPlug $VER:"
find dist -type f | sort
