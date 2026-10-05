#!/bin/sh
# Cross-compiles build/Mesh.scr with mingw-w64 (macOS: brew install mingw-w64;
# Debian/Ubuntu: apt install gcc-mingw-w64-x86-64). Unsigned, like every
# release of this project.
set -eu
cd "$(dirname "$0")/.."

CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
VERSION=$(cat VERSION)
COMMA=$(echo "$VERSION" | tr . ,),0

mkdir -p build/obj/windows
"$WINDRES" -I windows \
  -DMESH_VERSION_COMMA="$COMMA" -DMESH_VERSION_STRING="\\\"$VERSION\\\"" \
  windows/mesh.rc -O coff -o build/obj/windows/mesh.res
"$CC" -std=gnu99 -O2 -Wall -Wextra -mwindows -Icore -Iwindows \
  -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
  core/mesh.c core/json.c core/meshmonitor.c windows/mesh_win.c build/obj/windows/mesh.res \
  -o build/Mesh.scr -static -lgdiplus -lwinhttp -lsecur32 -lcomdlg32 -lcomctl32 -lshell32 -lm

echo "Built build/Mesh.scr ($VERSION)"
