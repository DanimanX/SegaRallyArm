#!/bin/bash
# PortMaster launcher for SegaRallyArm (Practice Mode Quick-Start)
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/Sega Rally.sh" --practice "$@"
