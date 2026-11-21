#!/bin/bash
# PortMaster launcher for SegaRallyArm (Sega Rally Championship 1995 — Sega Model 2A CRX)
# Targeted at Anbernic RG34XX (Allwinner H700 / Mali-G31) and compatible AArch64
# Linux handhelds running muOS, Knulli, ROCKNIX, ArkOS, AmberELEC or Stock OS.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

GAMEDIR="$(cd "$(dirname "$0")" && pwd)/segarallyarm"
if [ ! -d "$GAMEDIR" ] && [ -n "$directory" ]; then
  GAMEDIR="/$directory/ports/segarallyarm"
fi
cd "$GAMEDIR" || exit 1

exec >> "$GAMEDIR/log.txt" 2>&1

if [ -n "$controlfolder" ] && [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
fi

echo "=== SegaRallyArm (Sega Model 2A CRX ARM64 / OpenGL ES 2.0) ==="
echo "Date: $(date)"
echo "Device: ${DEVICE_NAME:-unknown}  CFW: ${CFW_NAME:-unknown}  Arch: $(uname -m)"

# Use firmware Mali/SDL2 first; append local libs
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$GAMEDIR/libs.aarch64"
if [ -n "$sdl_controllerconfig" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
fi
if [ -z "$SDL_GAMECONTROLLERCONFIG" ]; then
  export SDL_GAMECONTROLLERCONFIG="19000000010000000100000000010000,Anbernic RG34XX Controller,platform:Linux,b:b3,a:b4,dpdown:h0.4,leftx:a0,lefty:a1,lefttrigger:b12,dpleft:h0.8,rightshoulder:b8,leftshoulder:b7,righttrigger:b13,dpright:h0.2,back:b9,start:b10,dpup:h0.1,y:b6,x:b5,guide:b11,"
fi
export SEGAMOD2_GLES=1
export SEGAMOD2_ROOT="$GAMEDIR"
export SEGAMOD2_ROM_DIR="$GAMEDIR/roms/srallyc-b"

# Lock CPU governor to performance while playing if permitted
ORIG_GOV=""
if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null || true
  done
elif [ -n "$ESUDO" ] && command -v "$ESUDO" >/dev/null 2>&1 && [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  $ESUDO sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done' 2>/dev/null || true
fi

pm_platform_helper "$GAMEDIR/segarally.aarch64" >/dev/null 2>&1 || true

./segarally.aarch64 --data-dir "$GAMEDIR" --fullscreen "$@"
RC=$?

# Restore previous CPU governor on exit
if [ -n "$ORIG_GOV" ]; then
  if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      echo "$ORIG_GOV" > "$g" 2>/dev/null || true
    done
  elif [ -n "$ESUDO" ] && command -v "$ESUDO" >/dev/null 2>&1; then
    $ESUDO sh -c "for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo '$ORIG_GOV' > \"\$g\" 2>/dev/null || true; done" 2>/dev/null || true
  fi
fi

pm_finish >/dev/null 2>&1 || true
exit $RC
