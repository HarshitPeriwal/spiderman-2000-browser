#!/usr/bin/env bash
# Rebuilds the headless Node build and runs it against a game folder (inside WSL/Linux).
#   run_node.sh "<game folder>" [seconds]      log in $SPIDEY_BUILD/run.log
# RC_* variables pass through (RC_LOG=1, RC_LOG_IMPORTS=1, RC_LOG_D3D=1|2, RC_WATCHDOG=secs, ...).
# Saves and settings go to $SPIDEY_BUILD/userdata, never into the game folder.
set -euo pipefail
GAME=${1:?usage: run_node.sh "<game folder>" [seconds]}
SECS=${2:-90}
HERE=$(cd "$(dirname "$0")" && pwd)
B=${SPIDEY_BUILD:-$HOME/spidey-build}
source "${EMSDK_ENV:-$HOME/tools/emsdk/emsdk_env.sh}" >/dev/null 2>&1
python3 "$HERE/build.py" node > "$B/build.log" 2>&1 || { tail -40 "$B/build.log"; exit 1; }
grep -v "^  compiled" "$B/build.log" || true
mkdir -p "$B/userdata"
set +e
RC_USERDATA="$B/userdata" timeout "$SECS" node "$B/node/recomp.js" "$GAME" > "$B/run.log" 2>&1
echo "exit $? ($(wc -l < "$B/run.log") log lines, $B/run.log)"
grep -n -E "guest trap|unimplemented|\[stub\]|not implemented|Aborted" "$B/run.log" | head -30
