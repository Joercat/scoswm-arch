#!/bin/sh
# Optional headless graphical regressions using sidorares/node-x11's pure-JS
# X server. No Xvfb/Xorg is needed. Set SCOSWM_NODE_X11 to a node-x11 checkout,
# or install an x11 module that provides lib/xserver, then run make check.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
command -v node >/dev/null 2>&1 || { echo "SKIP: node is not installed for headless graphical tests"; exit 0; }
if [ -n "${SCOSWM_NODE_X11:-}" ]; then
    [ -f "$SCOSWM_NODE_X11/lib/xserver/index.js" ] || {
        echo "FAIL: SCOSWM_NODE_X11 does not provide lib/xserver"; exit 1;
    }
elif ! node -e "require('x11/lib/xserver')" >/dev/null 2>&1; then
    echo "SKIP: node-x11 xserver unavailable (set SCOSWM_NODE_X11 to enable graphical tests)"
    exit 0
fi
cd "$root"
exec node tests/test_wm_graphical.js
