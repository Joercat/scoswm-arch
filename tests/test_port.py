#!/usr/bin/env python3
"""Smoke tests for the SCos Linux port, driving the headless backend.

    python3 tests/test_port.py ./scoswm-headless

Every test runs the real WM code (compositor, apps, VFS) with the
headless backend: presented frames are accumulated into PPM
screenshots, and synthetic input (--click/--type) is injected through
the same event queues the SDL backend feeds.

These are smoke tests, not pixel-perfect golden images: they assert
that the desktop and each app actually paint, that terminal input
executes, and that the snapshot persistence round-trips.
"""
import concurrent.futures
import subprocess
import sys
import tempfile
import os

BIN = sys.argv[1] if len(sys.argv) > 1 else "./scoswm-headless"
FAILURES = []


def run(args, data_dir, timeout=120):
    cmd = [BIN, "--data-dir", data_dir] + args
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise AssertionError(f"{' '.join(cmd)} exited {r.returncode}\n{r.stderr[-2000:]}")
    return r


def read_ppm(path):
    data = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while data[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j+1].isspace():
            j += 1
        parts.append(data[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def colors(pix):
    return set(zip(pix[0::3], pix[1::3], pix[2::3]))


def check(name, fn):
    try:
        fn()
        print(f"ok   {name}")
    except Exception as e:  # noqa: BLE001 - report and continue
        FAILURES.append(name)
        print(f"FAIL {name}: {e}")


def desktop_shot(tmp):
    shot = os.path.join(tmp, "desk.ppm")
    run(["--frames", "12", "--screenshot", shot], os.path.join(tmp, "d1"))
    return read_ppm(shot)


def test_desktop(tmp):
    w, h, pix = desktop_shot(tmp)
    assert (w, h) == (1024, 768), f"unexpected size {w}x{h}"
    cs = colors(pix)
    assert len(cs) > 10, f"desktop looks blank ({len(cs)} colours)"
    assert (0x39, 0xFF, 0x14) in cs, "Matrix theme accent colour missing"


def test_terminal_executes(tmp):
    shot = os.path.join(tmp, "term.ppm")
    d = os.path.join(tmp, "d2")
    run(["--frames", "25", "--screenshot", shot,
         "--open", "terminal", "--type", "version"], d)
    w, h, pix = read_ppm(shot)
    _, _, desk = desktop_shot(tmp)
    assert pix != desk, "terminal window did not change the frame"
    snap = open(os.path.join(d, "vfs.snapshot"), "rb").read()
    assert snap.startswith(b"SCOSLNX1"), "snapshot header missing"
    assert b"system/settings.json" in snap, "snapshot lacks settings file"


def test_theme_persists(tmp):
    d = os.path.join(tmp, "d3")
    run(["--frames", "25", "--open", "terminal", "--type", "theme blue-sky"], d)
    snap = open(os.path.join(d, "vfs.snapshot"), "rb").read()
    assert b"blue-sky" in snap, "theme choice was not persisted"
    shot = os.path.join(tmp, "blue.ppm")
    run(["--frames", "12", "--screenshot", shot], d)
    _, _, pix = read_ppm(shot)
    assert (0x00, 0xAA, 0xFF) in colors(pix), "Blue Sky theme not restored"


def test_launcher_click(tmp):
    shot = os.path.join(tmp, "launch.ppm")
    run(["--frames", "15", "--screenshot", shot, "--click", "25,748"],
        os.path.join(tmp, "d4"))
    _, _, pix = read_ppm(shot)
    _, _, desk = desktop_shot(tmp)
    assert pix != desk, "launcher click had no effect"


def test_app_opens(tmp):
    apps = ["files", "notepad", "calendar", "settings", "about",
            "browser", "blackjack", "solitaire", "sysmon"]
    _, _, desk = desktop_shot(tmp)

    def one(app):
        shot = os.path.join(tmp, f"app-{app}.ppm")
        run(["--frames", "10", "--screenshot", shot, "--open", app],
            os.path.join(tmp, f"d-{app}"), timeout=180)
        _, _, pix = read_ppm(shot)
        assert pix != desk, f"{app} window did not paint"
        assert len(colors(pix)) > 10, f"{app} window looks blank"

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        list(ex.map(one, apps))


def main():
    if not os.path.isfile(BIN):
        print(f"binary not found: {BIN} (build it with: make scoswm-headless)")
        return 2
    with tempfile.TemporaryDirectory(prefix="scoswm-test-") as tmp:
        check("desktop paints", lambda: test_desktop(tmp))
        check("terminal executes a command", lambda: test_terminal_executes(tmp))
        check("theme persists via snapshot", lambda: test_theme_persists(tmp))
        check("launcher opens on click", lambda: test_launcher_click(tmp))
        check("every built-in app opens", lambda: test_app_opens(tmp))
    if FAILURES:
        print(f"\n{len(FAILURES)} test(s) failed: {', '.join(FAILURES)}")
        return 1
    print("\nall tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
