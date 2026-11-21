#!/bin/bash
# build.sh — OpenDE (open-source Deus Ex engine, github.com/jarmitage99/OpenDE,
# MIT licence) for the PS Vita.
#
#   ./build.sh            -> build-vita/opendevita.vpk (+ build-vita/opendevita.elf with symbols)
#
# The first run clones OpenDE at the tested revision into ./OpenDE and applies
# patches/opende-vita.patch. Work on the port happens in ./OpenDE; afterwards
# ./make_patch.sh writes the patch back. Needs the vitasdk (VITASDK) and builds
# vitaGL with ../dxvita/tools/build_vitagl.sh. The game files are NOT part of
# this: they come from the owner's own copy in ux0:data/DeusEx/.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
UPSTREAM=https://github.com/jarmitage99/OpenDE.git
REVISION=72e78a65ceff277e7f2e03979874533adf6fa905
SRC="$ROOT/OpenDE"
export VITASDK="${VITASDK:-/usr/local/vitasdk}"; export PATH="$VITASDK/bin:$PATH"

if [ ! -d "$SRC/.git" ]; then
    git clone -q "$UPSTREAM" "$SRC"
    git -C "$SRC" checkout -q -b vita "$REVISION"
    git -C "$SRC" apply --index "$ROOT/patches/opende-vita.patch"
fi

VGL="$ROOT/../dxvita/build-vita/vgl/prefix"
[ -f "$VGL/lib/libvitaGL.a" ] || "$ROOT/../dxvita/tools/build_vitagl.sh"

OUT="$ROOT/build-vita"; mkdir -p "$OUT/sce_sys"
cmake -S "$SRC" -B "$OUT/cmake" -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DSKIP_QA=ON -DSKIP_UT=ON -DVITAGL_PREFIX="$VGL" > "$OUT/cmake.log"
cmake --build "$OUT/cmake" -j"$(nproc)" --target deusex
cp "$OUT/cmake/bin/deusex" "$OUT/opendevita.elf"
vita-elf-create "$OUT/opendevita.elf" "$OUT/opendevita.velf"
vita-make-fself -c "$OUT/opendevita.velf" "$OUT/eboot.bin"
# ATTRIBUTE2=12: the extended user memory budget.
vita-mksfoex -s TITLE_ID=ODEVITA01 -s STITLE="OpenDE" -d ATTRIBUTE2=12 "Deus Ex (OpenDE)" "$OUT/sce_sys/param.sfo"
( cd "$OUT" && vita-pack-vpk -s sce_sys/param.sfo -b eboot.bin opendevita.vpk )
echo "built $OUT/opendevita.vpk"
