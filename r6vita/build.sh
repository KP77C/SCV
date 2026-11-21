#!/bin/bash
# build.sh — WarCraft III Vita boot harness.
#   ./build.sh               qemu-arm/Linux host test binary -> build-arm/r6vita_boot
#   TARGET=vita ./build.sh   PS Vita package                 -> build-vita/r6vita.vpk
# Engine (tools/winvita), built first with the WC3 flavour: the three-level
# jump table (WX86_JT3, 43 -> ~6 MiB of host heap at the menu) + counters:
#   cd ../tools/winvita && LIBTAG=_w3 EXTRA="-DWX86_JT3 -DPROF_COUNTERS" ./build.sh
#   cd ../tools/winvita && LIBTAG=_w3 EXTRA="-DWX86_JT3 -DPROF_COUNTERS" TARGET=vita ./build.sh
# then WXTAG=_w3 ./build.sh (host) or WXTAG=_w3 TARGET=vita ./build.sh.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
WX="${WINX86:-$ROOT/../tools/winvita}"
SRCS="$ROOT/src/boot.cpp $ROOT/src/sc_kernel32.cpp $ROOT/src/sc_misc.cpp $ROOT/src/sc_user32.cpp $ROOT/src/r6_d3d.cpp $ROOT/src/r6_shims.cpp $ROOT/src/sc_present.cpp $ROOT/src/sc_present_vita.cpp $ROOT/src/sc_input.cpp $ROOT/src/dx_shims.cpp $ROOT/src/w3_shims.cpp $ROOT/src/w3_gl.cpp $ROOT/src/w3_mp3.cpp $ROOT/src/w3_dshow.cpp $ROOT/src/w3_net.cpp $ROOT/src/w3_trace.cpp"

if [ "${TARGET:-}" = vita ]; then
    export VITASDK="${VITASDK:-/usr/local/vitasdk}"; export PATH="$VITASDK/bin:$PATH"
    OUT="$ROOT/build-vita"; mkdir -p "$OUT/sce_sys"
    WXLIB="$WX/build-vita/libwinx86_vita${WXTAG:-}.a"   # WXTAG=_prof: engine built with PROF_COUNTERS
    [ -f "$WXLIB" ] || { echo "missing $WXLIB"; exit 1; }
    # vitaGL draws the game's OpenGL (tools/build_vitagl.sh builds it into build-vita/vgl).
    VGL="$OUT/vgl/prefix"
    [ -f "$VGL/lib/libvitaGL.a" ] || "$ROOT/tools/build_vitagl.sh"
    arm-vita-eabi-g++ -O2 -g -std=gnu++17 -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -D__vita__ ${WXTAG:+-DPROF_COUNTERS} \
      -I"$WX/src" -I"$WX/src/dynarec86/shim/vita" -I"$VGL/include" -Wall -Wno-unused-parameter $SRCS \
      -Wl,--whole-archive "$WXLIB" -Wl,--no-whole-archive \
      -L"$VGL/lib" -lvitaGL -lvitashark -lmathneon \
      -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceTouch_stub -lSceCommonDialog_stub -lSceAppUtil_stub \
      -lSceAppMgr_stub -lSceAudio_stub -lScePower_stub -lSceSysmodule_stub -lSceKernelDmacMgr_stub -lSceIme_stub \
      -lSceNet_stub -lSceNetCtl_stub -lSceRtc_stub -lpthread -lm -Wl,-q -Wl,--defsym,__sce_headroom=0x4000 \
      -o "$OUT/r6vita.elf"
    # __sce_headroom leaves room after the code for the SCE module tables that
    # vita-elf-create appends (it fails when the code ends just below 64 KiB).
    # -Wl,-q keeps the relocations: the Vita loader places the module wherever
    # it likes, and vita-elf-create turns these into its own relocation table.
    vita-elf-create "$OUT/r6vita.elf" "$OUT/r6vita.velf"
    vita-make-fself "$OUT/r6vita.velf" "$OUT/eboot.bin"
    # ATTRIBUTE2=12: the extended user memory budget (no DMR).
    vita-mksfoex -s TITLE_ID=R6BOVITA1 -s STITLE="R6 Black Ops" -d ATTRIBUTE2=12 "Rainbow Six Black Ops Vita" "$OUT/sce_sys/param.sfo"
    # Optional art (icon0.png, livearea/contents/{bg,startup}.png + template.xml):
    # not in the repository, since it is the owner's local artwork.
    EXTRA_A=()
    for f in icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml; do
        if [ -f "$ROOT/sce_sys/$f" ]; then
            mkdir -p "$OUT/sce_sys/$(dirname "$f")"; cp "$ROOT/sce_sys/$f" "$OUT/sce_sys/$f"
            EXTRA_A+=(-a "sce_sys/$f=sce_sys/$f")
        fi
    done
    ( cd "$OUT" && vita-pack-vpk -s sce_sys/param.sfo -b eboot.bin "${EXTRA_A[@]}" r6vita.vpk )
    echo "built $OUT/r6vita.vpk"
    exit 0
fi
OUT="$ROOT/build-arm"
mkdir -p "$OUT"
WXLIB_HOST="$WX/build-arm/libwinx86${WXTAG:-}.a"
[ -f "$WXLIB_HOST" ] || { echo "missing $WXLIB_HOST (see the engine line in the header)"; exit 1; }
# Host GL: Mesa's OSMesa for armhf (apt: libosmesa6-dev:armhf), so the
# binary is dynamic: run it as  qemu-arm -L / build-arm/r6vita_boot <game-dir>
mkdir -p "$OUT/inc"; for d in GL KHR; do [ -e "$OUT/inc/$d" ] || ln -s "/usr/include/$d" "$OUT/inc/$d"; done
arm-linux-gnueabihf-g++ -O1 -g -std=gnu++17 -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard ${WXTAG:+-DPROF_COUNTERS} \
  -I"$WX/src" -I"$OUT/inc" -Wall -Wno-unused-parameter \
  $SRCS \
  "$WXLIB_HOST" -L/usr/lib/arm-linux-gnueabihf -lOSMesa -lpthread -lm -ldl -no-pie -Wl,-Ttext-segment=0x40000000 \
  -o "$OUT/r6vita_boot"
echo "built $OUT/r6vita_boot"
