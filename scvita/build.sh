#!/bin/bash
# build.sh — StarCraft Vita boot harness.
#   ./build.sh               qemu-arm/Linux host test binary -> build-arm/scvita_boot
#   TARGET=vita ./build.sh   PS Vita package                 -> build-vita/scvita.vpk
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
WX="${WINX86:-$ROOT/../tools/winvita}"
SRCS="$ROOT/src/boot.cpp $ROOT/src/sc_kernel32.cpp $ROOT/src/sc_misc.cpp $ROOT/src/sc_user32.cpp $ROOT/src/sc_ddraw.cpp $ROOT/src/sc_present.cpp $ROOT/src/sc_present_vita.cpp $ROOT/src/sc_input.cpp"

if [ "${TARGET:-}" = vita ]; then
    export VITASDK="${VITASDK:-/usr/local/vitasdk}"; export PATH="$VITASDK/bin:$PATH"
    OUT="$ROOT/build-vita"; mkdir -p "$OUT/sce_sys"
    WXLIB="$WX/build-vita/libwinx86_vita${WXTAG:-}.a"   # WXTAG=_prof: engine built with PROF_COUNTERS
    [ -f "$WXLIB" ] || { echo "missing $WXLIB"; exit 1; }
    arm-vita-eabi-g++ -O2 -g -std=gnu++17 -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -D__vita__ ${WXTAG:+-DPROF_COUNTERS} \
      -I"$WX/src" -I"$WX/src/dynarec86/shim/vita" -Wall -Wno-unused-parameter $SRCS \
      -Wl,--whole-archive "$WXLIB" -Wl,--no-whole-archive \
      -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceTouch_stub -lSceCommonDialog_stub -lSceAppUtil_stub \
      -lSceAppMgr_stub -lSceAudio_stub -lScePower_stub -lSceSysmodule_stub -lSceKernelDmacMgr_stub -lSceIme_stub \
      -lSceNet_stub -lSceNetCtl_stub -lSceRtc_stub -lpthread -lm -Wl,-q \
      -o "$OUT/scvita.elf"
    # -Wl,-q keeps the relocations: the Vita loader places the module wherever
    # it likes, and vita-elf-create turns these into its own relocation table.
    vita-elf-create "$OUT/scvita.elf" "$OUT/scvita.velf"
    vita-make-fself "$OUT/scvita.velf" "$OUT/eboot.bin"
    # ATTRIBUTE2=12: the extended user memory budget (no DMR), for K's 304 MiB.
    vita-mksfoex -s TITLE_ID=SCVITA001 -s STITLE="StarCraft Vita" -d ATTRIBUTE2=12 "StarCraft Vita" "$OUT/sce_sys/param.sfo"
    [ -f "$ROOT/sce_sys/icon0.png" ] && cp "$ROOT/sce_sys/icon0.png" "$OUT/sce_sys/icon0.png"
    ( cd "$OUT" && vita-pack-vpk -s sce_sys/param.sfo -b eboot.bin \
        $( [ -f sce_sys/icon0.png ] && echo "-a sce_sys/icon0.png=sce_sys/icon0.png" ) scvita.vpk )
    echo "built $OUT/scvita.vpk"
    exit 0
fi
OUT="$ROOT/build-arm"
mkdir -p "$OUT"
[ -f "$WX/build-arm/libwinx86.a" ] || (cd "$WX" && ./build.sh)
arm-linux-gnueabihf-g++ -O1 -g -std=gnu++17 -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -I"$WX/src" -Wall -Wno-unused-parameter \
  $SRCS \
  "$WX/build-arm/libwinx86.a" -lpthread -lm -ldl -static -Wl,-Ttext-segment=0x40000000 \
  -o "$OUT/scvita_boot"
echo "built $OUT/scvita_boot"
