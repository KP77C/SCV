#!/bin/bash
# build_vitagl.sh — builds vitaGL (with vitaShaRK and math-neon) for the Vita
# OpenGL path into build-vita/vgl/prefix. Pinned to the revisions this port
# was tested with. Nothing is installed into the vitasdk itself.
#
# SceShaccCgExt (which needs taiHEN stubs) is left out: vitaShaRK only uses
# it to switch on GLSL extensions, and Deus Ex draws with fixed-function GL.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export VITASDK="${VITASDK:-/usr/local/vitasdk}"; export PATH="$VITASDK/bin:$PATH"
W="$ROOT/build-vita/vgl"; P="$W/prefix"
mkdir -p "$W" "$P/lib" "$P/include"
fetch() {   # repo dir commit
    [ -d "$W/$2/.git" ] || git clone -q "https://github.com/$1" "$W/$2"
    git -C "$W/$2" fetch -q --depth 1 origin "$3" 2>/dev/null || true
    git -C "$W/$2" checkout -q "$3"
}
fetch Rinnegatamante/math-neon math-neon 0faab814782c071ff4015527f1ca955ab1ccc470
fetch Rinnegatamante/vitaShaRK vitaShaRK df24065e65098b2d1ac533760109ad4367573f28
fetch Rinnegatamante/vitaGL vitaGL 21d8ee2b3f0d03d2b537b898df5cac62f02e2d29

cat > "$P/include/shacccg_ext.h" <<'EOF'
/* Deus Ex Vita: SceShaccCgExt left out (it needs taiHEN). Its extensions
   only matter for GLSL translation; the game uses fixed-function GL. */
#pragma once
static inline int sceShaccCgExtEnableExtensions(void) { return 0; }
static inline int sceShaccCgExtDisableExtensions(void) { return 0; }
EOF
( cd "$W/math-neon" && make -s -j8 >/dev/null && cp libmathneon.a "$P/lib/" && cp source/math_neon.h "$P/include/" )
( cd "$W/vitaShaRK" && make -s -j8 CFLAGS="-g -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize -I$P/include" >/dev/null \
    && cp libvitashark.a "$P/lib/" && cp source/vitashark.h "$P/include/" )
( cd "$W/vitaGL" && make -s clean >/dev/null 2>&1;  C_INCLUDE_PATH="$P/include" CPLUS_INCLUDE_PATH="$P/include" make -s -j8 NO_SPLASHSCREEN=1 >/dev/null \
    && cp libvitaGL.a "$P/lib/" && cp source/vitaGL.h "$P/include/" )
arm-vita-eabi-nm --defined-only "$P/lib/libvitaGL.a" 2>/dev/null | awk '$2=="T"{print $3}' | grep -E '^(gl|vgl)' | sort -u > "$P/vgl_syms.txt"
echo "built $P"
