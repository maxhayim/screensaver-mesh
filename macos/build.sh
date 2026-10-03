#!/bin/sh
# Builds build/Mesh.saver (universal: Apple silicon + Intel) with the
# command-line tools. No Xcode project, no paid signing: the bundle is
# ad-hoc signed, which is free.
set -eu
cd "$(dirname "$0")/.."

VERSION=$(cat VERSION)
MIN_MACOS=11.0
OUT=build/Mesh.saver
rm -rf build/obj "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"

for ARCH in arm64 x86_64; do
  OBJ=build/obj/$ARCH
  mkdir -p "$OBJ"
  clang -std=c99 -O2 -Wall -Wextra -target "$ARCH-apple-macos$MIN_MACOS" -c core/mesh.c -o "$OBJ/mesh.o"
  swiftc -O -target "$ARCH-apple-macos$MIN_MACOS" -module-name MeshSaver -parse-as-library \
    -I core -emit-library -o "$OBJ/Mesh" \
    macos/*.swift "$OBJ/mesh.o" \
    -framework ScreenSaver -framework AppKit
done

lipo -create build/obj/arm64/Mesh build/obj/x86_64/Mesh -output "$OUT/Contents/MacOS/Mesh"
sed "s/__VERSION__/$VERSION/g" macos/Info.plist > "$OUT/Contents/Info.plist"
cp macos/Resources/* "$OUT/Contents/Resources/"
# Dropbox and Finder attach extended attributes that codesign rejects.
xattr -cr "$OUT"
codesign --force --sign - "$OUT"

echo "Built $OUT ($VERSION)"
