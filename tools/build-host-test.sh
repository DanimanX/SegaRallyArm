#!/usr/bin/env bash
# Native x86_64 host build of SegaRallyArm to test the exact platform/arm64/
# input engine, 4-core thread affinity, and GLES/GL2 rendering on desktop Linux.
#
# Usage:
#   tools/build-host-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BDIR="$PWD/build-host"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$BDIR/obj"

SDL_CFLAGS=$(pkg-config --cflags sdl2 2>/dev/null || echo "-I/usr/include/SDL2")
SDL_LIBS=$(pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")

COMMON_DEFS="-D_GNU_SOURCE -DGL_GLEXT_PROTOTYPES=1 -DHOST_COMPAT_HAVE_CLOCK_GETTIME \
  -DI960_HOST_HAVE_SDL -DSDL_MAIN_HANDLED -DI960_HOST_HAVE_GL"

INC_FLAGS="-I$PWD/src -I$PWD/include -I$PWD/lib/model2/include -I$PWD/lib/model2/host \
  -I$PWD/lib/model2/geo/include -I$PWD/lib/model2/hw/include -I$PWD/lib/model2/tgp/include \
  -I$PWD/lib/model2/snd/include -I$PWD/lib/host_compat -I$PWD/platform/arm64 $SDL_CFLAGS"

C_FLAGS="-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-attributes \
  $COMMON_DEFS $INC_FLAGS"

echo "[1/2] Compiling native x86_64 SegaRallyArm sources..."

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
    opt="-O2"
    case "$src" in
        *game_start_race_obj_car_frame.c) opt="-O1" ;; # Workaround for GCC 16 ICE
    esac
    exec gcc '"$C_FLAGS"' $opt -c "$src" -o "$out"
' _ {} "$BDIR/obj" < "$BDIR/sources.txt"

echo "[2/2] Linking $BDIR/segarally.x86_64..."
gcc -O2 -rdynamic -o "$BDIR/segarally.x86_64" "$BDIR/obj"/*.o $SDL_LIBS -lGL -lm -lpthread -ldl

echo "SUCCESS! Built native binary:"
ls -lh "$BDIR/segarally.x86_64"
