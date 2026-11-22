#!/usr/bin/env bash
# Cross-compile SegaRallyArm (ROM-free distributable runtime) for AArch64 Linux
# handhelds (Anbernic RG34XX / Allwinner H700, RG35XX Plus/H/SP, RG40XX,
# Powkiddy RGB30, TrimUI Smart Pro) targeting glibc <= 2.28 + OpenGL ES 2.0 +
# ARM64 NEON, and assemble a PortMaster zip package.
#
# Zero copyrighted ROMs or ROM-derived game code ever enter this package.
# Users supply their own legally owned MAME ROM sets (srallyc.zip / srallycb.zip).
#
# Usage:
#   tools/build-arm64.sh                    # default CPU: cortex_a53 (H700)
#   TARGET_CPU=cortex_a55 tools/build-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."

DEPS="$PWD/deps"
BDIR="$PWD/build-arm64"
OUT="$PWD/dist/SegaRallyArm-PortMaster-aarch64"
ZIPOUT="$PWD/dist/SegaRallyArm-PortMaster-aarch64.zip"
TARGET_CPU="${TARGET_CPU:-cortex_a53}"
TARGET_CPU="${TARGET_CPU//-/_}"
TARGET_TRIPLE="aarch64-linux-gnu.2.28"
JOBS="${JOBS:-$(nproc)}"

ZIG_VER="0.17.0"
ZIG_SHA="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
SDL_VER="2.32.10"
SDL_SHA="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"

mkdir -p "$DEPS/dl" "$BDIR/obj" "$BDIR/inc" "$PWD/dist"

fetch_file() {
    local url="$1" name="$2" sha="$3"
    if [ ! -f "$DEPS/dl/$name" ]; then
        curl -fsSL --retry 3 -o "$DEPS/dl/$name.part" "$url"
        mv "$DEPS/dl/$name.part" "$DEPS/dl/$name"
    fi
    echo "$sha  $DEPS/dl/$name" | sha256sum -c --quiet || {
        echo "ERROR: SHA-256 mismatch for $DEPS/dl/$name" >&2
        exit 1
    }
}

# 1. Host Zig cross-compiler
if [ ! -x "$DEPS/zig-linux/zig" ]; then
    echo "[1/5] Fetching Zig $ZIG_VER..."
    fetch_file "https://ziglang.org/download/$ZIG_VER/zig-x86_64-linux-$ZIG_VER.tar.xz" \
        "zig-x86_64-linux-$ZIG_VER.tar.xz" "$ZIG_SHA"
    tar -C "$DEPS" -xJf "$DEPS/dl/zig-x86_64-linux-$ZIG_VER.tar.xz"
    rm -rf "$DEPS/zig-linux"
    mv "$DEPS/zig-x86_64-linux-$ZIG_VER" "$DEPS/zig-linux"
fi
ZIG_BIN="$DEPS/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

# 2. SDL2 headers
if [ ! -d "$DEPS/sdl2-src/SDL2-$SDL_VER/include" ]; then
    echo "[2/5] Fetching SDL2 $SDL_VER headers..."
    fetch_file "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" \
        "SDL2-$SDL_VER.tar.gz" "$SDL_SHA"
    rm -rf "$DEPS/sdl2-src" && mkdir -p "$DEPS/sdl2-src"
    tar -C "$DEPS/sdl2-src" -xzf "$DEPS/dl/SDL2-$SDL_VER.tar.gz"
fi
SDL_INC="$BDIR/inc"
rm -rf "$SDL_INC/SDL2" && mkdir -p "$SDL_INC/SDL2" "$SDL_INC/GLES2" "$SDL_INC/KHR"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/include"/*.h "$SDL_INC/SDL2/"
if [ -d "/usr/include/GLES2" ] && [ -d "/usr/include/KHR" ]; then
    cp /usr/include/GLES2/*.h "$SDL_INC/GLES2/"
    cp /usr/include/KHR/*.h "$SDL_INC/KHR/"
else
    sed 's|/\*#include <KHR/khrplatform.h>\*/|#include <KHR/khrplatform.h>|g; s|/\*#include <GLES2/gl2platform.h>\*/|#include <GLES2/gl2platform.h>|g' "$SDL_INC/SDL2/SDL_opengles2_gl2.h" > "$SDL_INC/GLES2/gl2.h"
    sed 's|/\*#include <KHR/khrplatform.h>\*/|#include <KHR/khrplatform.h>|g' "$SDL_INC/SDL2/SDL_opengles2_gl2platform.h" > "$SDL_INC/GLES2/gl2platform.h"
    cp "$SDL_INC/SDL2/SDL_opengles2_gl2ext.h" "$SDL_INC/GLES2/gl2ext.h"
    cp "$SDL_INC/SDL2/SDL_opengles2_khrplatform.h" "$SDL_INC/KHR/khrplatform.h"
fi
cat > "$SDL_INC/SDL2/SDL_config.h" <<'EOF'
#ifndef SDL_config_h_
#define SDL_config_h_
#include "SDL_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#define SDL_BYTEORDER 1234
#define SIZEOF_VOIDP 8
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define SDL_VIDEO_OPENGL_ES2 1
#define SDL_VIDEO_OPENGL_EGL 1
#endif
EOF

COMMON_DEFS="-D_GNU_SOURCE -DGL_GLEXT_PROTOTYPES=1 -DHOST_COMPAT_HAVE_CLOCK_GETTIME \
  -DI960_HOST_HAVE_SDL -DSDL_MAIN_HANDLED -DI960_HOST_HAVE_GL -DSEGAMOD2_GLES=1"

INC_FLAGS="-I$PWD/src -I$PWD/include -I$PWD/lib/model2/include -I$PWD/lib/model2/host \
  -I$PWD/lib/model2/geo/include -I$PWD/lib/model2/hw/include -I$PWD/lib/model2/tgp/include \
  -I$PWD/lib/model2/snd/include -I$PWD/lib/host_compat -I$PWD/platform/arm64 \
  -I$SDL_INC -I$SDL_INC/SDL2"

C_FLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=gnu99 -O2 -fno-fast-math \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-attributes \
  $COMMON_DEFS $INC_FLAGS"

echo "[3/5] Compiling SegaRallyArm sources for $TARGET_TRIPLE ($TARGET_CPU, GLES 2.0, -j$JOBS)..."

# Discover all C source files
C_SRCS=(
    lib/model2/geo/*.c
    lib/model2/hw/*.c
    lib/model2/tgp/*.c
    lib/model2/snd/*.c
    lib/model2/host/*.c
    src/game/*.c
    src/host/*.c
    src/libc/*.c
    src/boot/*.c
    src/irq/*.c
    src/i960_host_syms.c
    src/i960_host_invoke.c
    src/lift_main.c
    lib/host_compat/model2_gl.c
    platform/arm64/gl_funcs.c
    platform/arm64/controls.c
    platform/arm64/gpu_gles.c
)

python3 - <<PYEOF
import glob, sys
srcs = []
for p in [
    'lib/model2/geo/*.c', 'lib/model2/hw/*.c', 'lib/model2/tgp/*.c',
    'lib/model2/snd/*.c', 'lib/model2/host/*.c', 'src/game/*.c',
    'src/host/*.c', 'src/libc/*.c', 'src/boot/*.c', 'src/irq/*.c',
    'src/i960_host_syms.c', 'src/i960_host_invoke.c', 'src/lift_main.c',
    'lib/host_compat/model2_gl.c', 'platform/arm64/gl_funcs.c',
    'platform/arm64/controls.c', 'platform/arm64/gpu_gles.c'
]:
    srcs.extend(glob.glob(p))
with open('$BDIR/sources.txt', 'w') as f:
    for s in sorted(set(srcs)):
        f.write(s + '\n')
PYEOF

xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    tag=$(echo "$src" | tr "/." "__")
    out="$2/${tag}.o"
    [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
    extra=""
    case "$src" in
        lib/model2/geo/*|lib/model2/tgp/*|lib/model2/snd/*|platform/arm64/gpu_gles.c|src/host/sys24_tile.c|src/host/sys24_gfx.c)
            extra="-O3 -fno-math-errno"
            ;;
    esac
    exec "$3" cc '"$C_FLAGS"' $extra -c "$src" -o "$out"
' _ {} "$BDIR/obj" "$ZIG_BIN" < "$BDIR/sources.txt"

echo "[4/5] Generating SDL2 and GLESv2 link-stubs and linking segarally.aarch64..."

python3 - "$BDIR" "$BDIR/obj"/*.o <<'PYEOF'
import subprocess, sys, os
bdir = sys.argv[1]
objs = sys.argv[2:]
sdl_syms = set()
gles_syms = set()
for o in objs:
    p = subprocess.run(["nm", "-u", o], capture_output=True, text=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U":
            sym = parts[-1]
            if sym.startswith("SDL_"):
                sdl_syms.add(sym)
            elif sym.startswith("gl") and not sym.startswith("gles_"):
                gles_syms.add(sym)

with open(f"{bdir}/sdl2_stub.c", "w") as f:
    f.write("/* Link-time stub for PortMaster system libSDL2-2.0.so.0 */\n")
    for s in sorted(sdl_syms):
        f.write(f"void {s}(void) {{}}\n")
print(f"  Generated SDL2 stub with {len(sdl_syms)} symbols")

with open(f"{bdir}/glesv2_stub.c", "w") as f:
    f.write("/* Link-time stub for PortMaster system libGLESv2.so.2 */\n")
    for s in sorted(gles_syms):
        f.write(f"void {s}(void) {{}}\n")
print(f"  Generated GLESv2 stub with {len(gles_syms)} symbols")
PYEOF

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -shared -fPIC \
    -Wl,-soname,libSDL2-2.0.so.0 "$BDIR/sdl2_stub.c" -o "$BDIR/libSDL2-2.0.so.0"

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -shared -fPIC \
    -Wl,-soname,libGLESv2.so.2 "$BDIR/glesv2_stub.c" -o "$BDIR/libGLESv2.so.2"

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O2 -rdynamic \
    -o "$BDIR/segarally.aarch64" \
    "$BDIR/obj"/*.o \
    -L"$BDIR" -l:libSDL2-2.0.so.0 -l:libGLESv2.so.2 -lm -lpthread -ldl

# Verify binary properties
BIN_INFO=$(file "$BDIR/segarally.aarch64")
echo "  Target binary: $BIN_INFO"
echo "$BIN_INFO" | grep -q "ARM aarch64" || { echo "ERROR: not AArch64"; exit 1; }

MAX_GLIBC=$(nm "$BDIR/segarally.aarch64" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -n1 || echo "unknown")
echo "  Max GLIBC version reference: $MAX_GLIBC (must be <= 2.28 for PortMaster)"

echo "[5/5] Assembling PortMaster ZIP package: $ZIPOUT..."
rm -rf "$OUT" && mkdir -p "$OUT/segarallyarm"
cp "portmaster/Sega Rally.sh" "$OUT/"
cp "portmaster/Sega Rally Practice.sh" "$OUT/"
cp "portmaster/segarallyarm/port.json" "$OUT/segarallyarm/"
cp "portmaster/segarallyarm/gameinfo.xml" "$OUT/segarallyarm/"
cp "portmaster/segarallyarm/gamecontrollerdb.txt" "$OUT/segarallyarm/"
cp "portmaster/segarallyarm/PUT-ROMS-HERE.txt" "$OUT/segarallyarm/"
cp "$BDIR/segarally.aarch64" "$OUT/segarallyarm/"
chmod +x "$OUT"/*.sh "$OUT/segarallyarm/segarally.aarch64"

(cd "$OUT" && zip -r -q "$ZIPOUT" .)
(cd "$(dirname "$ZIPOUT")" && sha256sum "$(basename "$ZIPOUT")" > "$(basename "$ZIPOUT").sha256")

echo "SUCCESS! Created:"
ls -lh "$ZIPOUT" "$ZIPOUT.sha256"
