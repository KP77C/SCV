#!/bin/bash
# build.sh — Deus Ex Vita boot harness.
#   ./build.sh               qemu-arm/Linux host test binary -> build-arm/dxvita_boot
#   TARGET=vita ./build.sh   PS Vita package                 -> build-vita/dxvita.vpk
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
WX="${WINX86:-$ROOT/../tools/winvita}"
SRCS="$ROOT/src/boot.cpp $ROOT/src/sc_kernel32.cpp $ROOT/src/sc_misc.cpp $ROOT/src/sc_user32.cpp $ROOT/src/sc_ddraw.cpp $ROOT/src/sc_present.cpp $ROOT/src/sc_present_vita.cpp $ROOT/src/sc_input.cpp $ROOT/src/dx_shims.cpp"

if [ "${TARGET:-}" = vita ]; then
    export VITASDK="${VITASDK:-/usr/local/vitasdk}"; export PATH="$VITASDK/bin:$PATH"
    OUT="$ROOT/build-vita"; mkdir -p "$OUT/sce_sys"
    WXLIB="$WX/build-vita/libwinx86_vita${WXTAG:-}.a"   # WXTAG=_prof: engine built with PROF_COUNTERS
    [ -f "$WXLIB" ] || { echo "missing $WXLIB"; exit 1; }
    # OpenGL path: vitaGL (tools/build_vitagl.sh builds it into build-vita/vgl).
    VGL="$OUT/vgl/prefix"
    [ -f "$VGL/lib/libvitaGL.a" ] || "$ROOT/tools/build_vitagl.sh"
    mkdir -p "$OUT/inc"
    python3 "$ROOT/tools/gen/gen_gl.py" "$ROOT/tools/gen/opengldrv_names.txt" "$ROOT/tools/gen/gl.h" "$ROOT/tools/gen/glext.h" \
        --have "$VGL/vgl_syms.txt" > "$OUT/inc/dx_gl_gen.inc"
    arm-vita-eabi-g++ -O2 -g -std=gnu++17 -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -D__vita__ ${WXTAG:+-DPROF_COUNTERS} \
      -I"$WX/src" -I"$WX/src/dynarec86/shim/vita" -I"$OUT/inc" -I"$VGL/include" -Wall -Wno-unused-parameter $SRCS "$ROOT/src/dx_gl.cpp" \
      -Wl,--whole-archive "$WXLIB" -Wl,--no-whole-archive \
      -L"$VGL/lib" -lvitaGL -lvitashark -lmathneon \
      -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceTouch_stub -lSceCommonDialog_stub -lSceAppUtil_stub \
      -lSceAppMgr_stub -lSceAudio_stub -lScePower_stub -lSceSysmodule_stub -lSceKernelDmacMgr_stub -lSceIme_stub \
      -lSceNet_stub -lSceNetCtl_stub -lSceRtc_stub -lpthread -lm -Wl,-q \
      -o "$OUT/dxvita.elf"
    # -Wl,-q keeps the relocations: the Vita loader places the module wherever
    # it likes, and vita-elf-create turns these into its own relocation table.
    vita-elf-create "$OUT/dxvita.elf" "$OUT/dxvita.velf"
    vita-make-fself "$OUT/dxvita.velf" "$OUT/eboot.bin"
    # ATTRIBUTE2=12: the extended user memory budget (no DMR), for K's 304 MiB.
    vita-mksfoex -s TITLE_ID=DXVITA001 -s STITLE="Deus Ex Vita" -d ATTRIBUTE2=12 "Deus Ex Vita" "$OUT/sce_sys/param.sfo"
    # Optional art (icon0.png, livearea/contents/{bg,startup}.png + template.xml):
    # not in the repository, since it is the owner's local artwork.
    EXTRA_A=()
    for f in icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml; do
        if [ -f "$ROOT/sce_sys/$f" ]; then
            mkdir -p "$OUT/sce_sys/$(dirname "$f")"; cp "$ROOT/sce_sys/$f" "$OUT/sce_sys/$f"
            EXTRA_A+=(-a "sce_sys/$f=sce_sys/$f")
        fi
    done
    ( cd "$OUT" && vita-pack-vpk -s sce_sys/param.sfo -b eboot.bin "${EXTRA_A[@]}" dxvita.vpk )
    echo "built $OUT/dxvita.vpk"
    exit 0
fi
OUT="$ROOT/build-arm"
mkdir -p "$OUT/inc"
# Host GL: Mesa's OSMesa for armhf (apt: libosmesa6-dev:armhf from
# ports.ubuntu.com). Only its GL/KHR headers are put on the include path, so
# the cross compiler never sees the build machine's own libc headers.
for d in GL KHR; do [ -e "$OUT/inc/$d" ] || ln -s "/usr/include/$d" "$OUT/inc/$d"; done
# GL shims for what this libOSMesa really exports (the rest become logged stubs).
arm-linux-gnueabihf-nm -D --defined-only /usr/lib/arm-linux-gnueabihf/libOSMesa.so.8 | awk '{print $3}' | grep '^gl' > "$OUT/gl_syms.txt"
python3 "$ROOT/tools/gen/gen_gl.py" "$ROOT/tools/gen/opengldrv_names.txt" "$ROOT/tools/gen/gl.h" "$ROOT/tools/gen/glext.h" \
    --have "$OUT/gl_syms.txt" > "$OUT/inc/dx_gl_gen.inc"
[ -f "$WX/build-arm/libwinx86.a" ] || (cd "$WX" && ./build.sh)
arm-linux-gnueabihf-g++ -O1 -g -std=gnu++17 -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -I"$WX/src" -isystem "$OUT/inc" -Wall -Wno-unused-parameter \
  $SRCS "$ROOT/src/dx_gl.cpp" \
  "$WX/build-arm/libwinx86.a" -L/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,/usr/lib/arm-linux-gnueabihf -lOSMesa \
  -lpthread -lm -ldl -Wl,-Ttext-segment=0x40000000 \
  -o "$OUT/dxvita_boot"
echo "built $OUT/dxvita_boot (dynamic: run with qemu-arm -L / ...)"
