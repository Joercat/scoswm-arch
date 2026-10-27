#!/bin/sh
# Integration test: boot scoswm on a real X server, manage a real X11
# client (tests/testclient.c), and verify adoption through the window
# tree with tests/xprobe.c.  Uses Xvfb when available; if there is no
# X server at all it reports SKIP and exits 0 (unit tests already ran).
set -u
WM=${1:-./scoswm}
TMP=$(mktemp -d)
WMPID=; CPID=; XPID=
cleanup() { [ -n "$WMPID" ] && kill $WMPID 2>/dev/null; [ -n "$CPID" ] && kill $CPID 2>/dev/null; [ -n "$XPID" ] && kill $XPID 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

cc ${CPPFLAGS:-} ${CFLAGS:--O2} -o "$TMP/xprobe" tests/xprobe.c ${LDFLAGS:-} -lX11 || { echo "SKIP: cannot build probe (libX11 dev files?)"; exit 0; }
cc ${CPPFLAGS:-} ${CFLAGS:--O2} -o "$TMP/testclient" tests/testclient.c ${LDFLAGS:-} -lX11 || { echo "SKIP: cannot build client"; exit 0; }

if [ -z "${DISPLAY:-}" ]; then
    if command -v Xvfb >/dev/null 2>&1; then
        Xvfb :77 -screen 0 1280x800x24 >"$TMP/xvfb.log" 2>&1 &
        XPID=$!
        DISPLAY=:77; export DISPLAY
        sleep 1
    else
        echo "SKIP: no Xvfb and DISPLAY unset - run under Xvfb/Xorg to enable"
        exit 0
    fi
fi

"$WM" >"$TMP/wm.log" 2>&1 &
WMPID=$!
sleep 1
if ! kill -0 $WMPID 2>/dev/null; then
    echo "FAIL: scoswm exited at startup:"; cat "$TMP/wm.log"; exit 1
fi

"$TMP/testclient" "Integration Window" &
CPID=$!
sleep 1

OUT=$("$TMP/xprobe")
echo "$OUT"
echo "$OUT" | grep -q "Integration Window" || { echo "FAIL: client not adopted/framed"; exit 1; }
echo "$OUT" | grep -q "active=0x" || { echo "FAIL: no active window set"; exit 1; }
kill $CPID 2>/dev/null; CPID=
sleep 1
echo "integration: scoswm managed a real X11 client OK"
