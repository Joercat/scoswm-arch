'use strict';
// Optional headless WM/terminal regressions. Use tests/run_graphical.sh;
// SCOSWM_NODE_X11 may point at a sidorares/node-x11 checkout with xserver support.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawn } = require('node:child_process');
const moduleRoot = process.env.SCOSWM_NODE_X11;
const x11 = moduleRoot ? require(path.join(moduleRoot, 'lib')) : require('x11');
const { createServer } = moduleRoot ? require(path.join(moduleRoot, 'lib/xserver')) : require('x11/lib/xserver');

const repo = path.resolve(__dirname, '..');
const displayNum = Number(process.env.SCOSWM_TEST_DISPLAY || 93);
const displayName = `127.0.0.1:${displayNum}`;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function waitFor(fn, message, timeout = 6000) {
    const start = Date.now();
    while (Date.now() - start < timeout) {
        const value = fn();
        if (value) return value;
        await delay(40);
    }
    throw new Error(`timeout: ${message}`);
}
function property(server, win, name) {
    const atom = server.atomsByName.get(name);
    const prop = atom && win.properties.get(atom);
    return prop && prop.data.toString('latin1').replace(/\0.*$/, '');
}

(async () => {
    const server = createServer({ width: 800, height: 600 });
    // Minimal SHAPE input-region support for the real libXext calls used by
    // the paint-only right rule. Rendering remains ordinary core X11.
    server.registerExtension('SHAPE', {
        handleRequest(xserver, client, minor, body) {
            if (minor === 0) { // QueryVersion
                const reply = client.startReply(0, 0);
                reply.writeUInt16LE(1, 8);
                reply.writeUInt16LE(1, 10);
                client.send(reply);
            } else if (minor === 1 && body.length >= 12) { // Rectangles
                const op = body.readUInt8(0), destinationKind = body.readUInt8(1);
                const win = xserver.resources.get(body.readUInt32LE(4));
                if (win && destinationKind === 2 && op === 0)
                    win.inputShapeEmpty = body.length === 12; // ShapeSet + empty rectangle list
            }
        }
    });
    const unshapedPointerWindow = server.pointerWindow;
    server.pointerWindow = function () {
        const hidden = [];
        try {
            for (;;) {
                const hit = unshapedPointerWindow.call(this);
                if (!hit || !hit.inputShapeEmpty) return hit;
                hit.mapped = false;
                hidden.push(hit);
            }
        } finally {
            for (const win of hidden) win.mapped = true;
        }
    };
    const directDraws = [];
    const handlerErrors = [];
    const configureLog = [];
    server.on('handlerError', (err, info) => handlerErrors.push({ err, info }));
    for (const opcode of [64, 65, 66, 67, 68, 69, 70, 71, 72, 74, 75, 76, 77]) {
        const original = server.handlers[opcode];
        server.handlers[opcode] = (client, detail, body) => {
            const drawable = server.resources.get(body.readUInt32LE(0));
            if (drawable && drawable.type === 'window' && drawable.id !== 1 && drawable.owner === client)
                directDraws.push({ opcode, wid: drawable.id });
            return original(client, detail, body);
        };
    }
    const originalConfigure = server.handlers[12];
    server.handlers[12] = (client, detail, body) => {
        const wid = body.readUInt32LE(0);
        const mask = body.readUInt16LE(4);
        const result = originalConfigure(client, detail, body);
        const win = server.resources.get(wid);
        configureLog.push({ wid, mask, source: client.resourceBase, width: win && win.width, height: win && win.height });
        return result;
    };
    const copyAreas = [];
    const originalCopyArea = server.handlers[62];
    server.handlers[62] = (client, detail, body) => {
        const result = originalCopyArea(client, detail, body);
        copyAreas.push({ dst: body.readUInt32LE(4), width: body.readUInt16LE(20),
            height: body.readUInt16LE(22), at: Date.now() });
        return result;
    };

    const home = fs.mkdtempSync(path.join(os.tmpdir(), 'scoswm-graphical-'));
    fs.mkdirSync(path.join(home, '.config/scoswm'), { recursive: true });
    fs.writeFileSync(path.join(home, '.config/scoswm/scoswm.conf'),
        `bar_height=28\npins=terminal,files,sysmon,settings\nfiles=${path.join(repo, 'test_file_client')}\n` +
        `volume_mute_cmd=media-action mute\nvolume_down_cmd=media-action down\nvolume_up_cmd=media-action up\n`);
    fs.mkdirSync(path.join(home, '.local/share/applications'), { recursive: true });
    fs.mkdirSync(path.join(home, '.local/share/icons'), { recursive: true });
    fs.copyFileSync(path.join(repo, 'packaging/scos-terminal.desktop'), path.join(home, '.local/share/applications/scos-terminal.desktop'));
    fs.cpSync(path.join(repo, 'packaging/icons/hicolor'), path.join(home, '.local/share/icons/hicolor'), { recursive: true });
    fs.cpSync(path.join(repo, 'packaging/icons/SCos'), path.join(home, '.local/share/icons/SCos'), { recursive: true });
    const fakeBin = path.join(home, 'bin');
    const nmcliLog = path.join(home, 'nmcli-args');
    fs.mkdirSync(fakeBin);
    fs.writeFileSync(path.join(fakeBin, 'nmcli'), `#!/bin/sh
if [ "$1" = "-t" ]; then
    : > "$NMCLI_LOG.started"
    sleep 2
    printf '%s\\n' ':Coffee:99:--:wlp1s0' '*:HomeNet:82:WPA2:wlp1s0' ':HomeNet:65:WPA2:wlp1s0' '*:HomeNet:71:WPA2:wlp2s0' \\
        ':Guest:55:--:wlp1s0' ':Lab:60:WPA2:wlp2s0' ':Five:50:--:wlp1s0' \\
        ':Six:49:--:wlp2s0' ':Seven:48:--:wlp1s0' ':Eight:47:--:wlp2s0'
    : > "$NMCLI_LOG.finished"
elif [ "$1" = "device" ]; then
    printf '%s\\n' "$@" > "$NMCLI_LOG"
    printf 'Connected\\n'
fi
`);
    fs.chmodSync(path.join(fakeBin, 'nmcli'), 0o755);
    fs.writeFileSync(path.join(fakeBin, 'media-action'),
        '#!/bin/sh\nprintf \'%s\\n\' "$*" >> "${NMCLI_LOG}.media"\n');
    fs.chmodSync(path.join(fakeBin, 'media-action'), 0o755);
    const env = { ...process.env, DISPLAY: displayName, HOME: home, XCURSOR_THEME: 'SCos', XCURSOR_SIZE: '24',
        PATH: `${fakeBin}:${process.env.PATH || '/usr/bin:/bin'}`, NMCLI_LOG: nmcliLog };
    delete env.XDG_CONFIG_HOME;
    delete env.XDG_DATA_HOME;
    delete env.XDG_DATA_DIRS;
    // Simulate an existing KDE/Xsession resource database: cursor entries
    // must be replaced in place while unrelated resources remain intact.
    server.root.properties.set(server.atomsByName.get('RESOURCE_MANAGER'), {
        type: server.atomsByName.get('STRING'), format: 8,
        data: Buffer.from('Xft.dpi: 96\nXcursor.theme: Breeze\nXcursor.size: 32\nXresources.keep: yes\n')
    });
    let wm, terminal, longTerminal, filesApp, conn;
    let wmLog = '', terminalLog = '';
    const net = server.listen(displayNum);
    try {
        await new Promise(resolve => net.once('listening', resolve));
        wm = spawn(path.join(repo, 'scoswm'), [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
        wm.stdout.on('data', b => wmLog += b);
        wm.stderr.on('data', b => wmLog += b);
        await delay(500);
        assert.equal(wm.exitCode, null, `WM exited: ${wmLog}`);
        const bar = await waitFor(() => server.root.children.find(w => w.width === 800 && w.height === 28 && w.y === 572 && w.mapped), '28px taskbar');
        const edgeRule = await waitFor(() => server.root.children.find(w =>
            property(server, w, 'WM_NAME') === 'SCosWM desktop right rule' && w.mapped), 'dedicated right-rule window');
        assert.equal(edgeRule.x, 783);
        assert.equal(edgeRule.y, 0);
        assert.equal(edgeRule.width, 2);
        assert.equal(edgeRule.height, 572);
        assert.equal(edgeRule.inputShapeEmpty, true, 'right rule must not intercept application input');
        const main = 0x39ff14;
        const resources = property(server, server.root, 'RESOURCE_MANAGER') || '';
        assert.match(resources, /Xcursor\.theme:\s*SCos/, 'Qt/X resource cursor theme');
        assert.match(resources, /Xcursor\.size:\s*24/, 'Qt/X resource cursor size');
        assert.match(resources, /Xft\.dpi:\s*96/, 'unrelated Xft resource was discarded');
        assert.match(resources, /Xresources\.keep:\s*yes/, 'unrelated custom resource was discarded');
        assert.doesNotMatch(resources, /Breeze/, 'old KDE cursor resource survived');
        assert.equal(resources.split('\n').filter(line => line.startsWith('Xcursor.theme:')).length, 1,
            'duplicate cursor resources after publication');
        const xsAtom = server.atomsByName.get('_XSETTINGS_SETTINGS');
        assert.ok(xsAtom, 'WM did not create the XSETTINGS property atom');
        const xsWindow = await waitFor(() => {
            for (const res of server.resources.values())
                if (res.type === 'window' && res.properties.has(xsAtom)) return res;
            return null;
        }, 'GTK XSETTINGS owner window');
        const xs = Buffer.from(xsWindow.properties.get(xsAtom).data);
        assert.equal(xs[0], 0, 'XSETTINGS byte order');
        assert.equal(xs.readUInt32LE(4), 1, 'XSETTINGS serial');
        assert.equal(xs.readUInt32LE(8), 2, 'XSETTINGS cursor setting count');
        const cursorSettings = {};
        let offset = 12;
        for (let i = 0; i < 2; i++) {
            const type = xs[offset];
            const nameLength = xs.readUInt16LE(offset + 2);
            offset += 4;
            const name = xs.toString('utf8', offset, offset + nameLength);
            offset = (offset + nameLength + 3) & ~3;
            offset += 4; // last-change serial
            if (type === 1) {
                const length = xs.readUInt32LE(offset); offset += 4;
                cursorSettings[name] = xs.toString('utf8', offset, offset + length);
                offset = (offset + length + 3) & ~3;
            } else {
                cursorSettings[name] = xs.readInt32LE(offset); offset += 4;
            }
        }
        assert.deepEqual(cursorSettings, {
            'Gtk/CursorThemeName': 'SCos',
            'Gtk/CursorThemeSize': 24
        });
        server.compose();
        const rootPixel = (x, y) => server.root.raster.data[y * 800 + x] & 0xffffff;
        assert.equal(rootPixel(783, 100), main, 'thin inset right desktop border');
        assert.equal(rootPixel(784, 100), main, 'second thin right desktop border column');
        assert.notEqual(rootPixel(785, 100), main, 'right border became a thick bar again');
        assert.equal(rootPixel(700, 571), main, 'bottom desktop border');

        // "Files" is five 8px glyphs in an 80px cell at x=104: centered x=124.
        let firstLabelX = -1;
        for (let y = 54; y <= 65; y++) {
            for (let x = 104; x < 184; x++) {
                if (rootPixel(x, y) === main) { firstLabelX = x; break; }
            }
            if (firstLabelX >= 0) break;
        }
        assert.ok(firstLabelX >= 123 && firstLabelX <= 125, `desktop label was not centered (first x=${firstLabelX})`);

        // Nothing bright may leak below the y=6..21 control boxes at 28px.
        const barPixel = (x, y) => bar.raster.data[y * bar.width + x] & 0xffffff;
        const countColorPixels = (pixel, x0, y0, w, h, color) => {
            let count = 0;
            for (let y = y0; y < y0 + h; y++)
                for (let x = x0; x < x0 + w; x++) if (pixel(x, y) === color) count++;
            return count;
        };
        const controlRanges = [[8, 42], [48, 158], [168, 323], [594, 628], [634, 668]];
        for (const [x0, x1] of controlRanges) {
            for (let y = 22; y < 28; y++) {
                for (let x = x0; x < x1; x++)
                    assert.notEqual(barPixel(x, y), main, `taskbar glyph escaped box at ${x},${y}`);
            }
        }
        // Every built-in must use its bundled image on both desktop and bar;
        // the dark panel pixels cannot be produced by a letter-glyph fallback.
        for (let i = 0; i < 4; i++) {
            assert.ok(countColorPixels(rootPixel, 44 + i * 88, 24, 24, 24, 0x1a1a1a) > 30,
                `built-in desktop icon ${i} fell back to a glyph`);
            assert.ok(countColorPixels(barPixel, 179 + i * 40, 8, 12, 12, 0x1a1a1a) > 8,
                `built-in taskbar icon ${i} fell back to a glyph`);
        }

        // The numeric month/day and year occupy separate rows between power
        // and the clock, including at the minimum 28px taskbar height.
        assert.ok(countColorPixels(barPixel, 676, 3, 48, 10, main) > 20,
            'numeric month/day row missing');
        assert.ok(countColorPixels(barPixel, 676, 15, 48, 10, main) > 20,
            'numeric year row missing');
        const wifiDotColor = barPixel(611, 18);
        assert.notEqual(wifiDotColor, barPixel(624, 18), 'Wi-Fi dot missing below arcs');
        assert.equal(barPixel(611, 6), wifiDotColor, 'Wi-Fi outer arc is not centred above the dot');
        assert.ok(countColorPixels(barPixel, 600, 7, 22, 8, wifiDotColor) > 0,
            'Wi-Fi arcs are not drawn above the dot');
        // At 28px the power circle is centred at (651,15), radius 6.
        // Its right-hand side must be drawn and its opening remains upward.
        assert.equal(barPixel(657, 15), main, 'power symbol does not open upward');

        // Every taskbar hover/launch state follows the same readable
        // bright-accent + dark-foreground rule as menus and Settings.
        server.injectPointerMove(20, 586);
        await waitFor(() => barPixel(40, 7) === main, 'Start hover accent');
        assert.equal(barPixel(15, 9), 0x001500, 'Start hover glyph contrast');
        server.injectPointerMove(100, 586);
        await waitFor(() => barPixel(155, 7) === main, 'Windows hover accent');
        assert.ok(countColorPixels(barPixel, 56, 7, 94, 20, 0x001500) > 0,
            'Windows hover text contrast');
        server.injectPointerMove(185, 586);
        await waitFor(() => barPixel(199, 7) === main, 'pin hover accent');
        assert.ok(countColorPixels(barPixel, 179, 8, 12, 12, 0x1a1a1a) > 8,
            'pin image disappeared on hover');
        server.injectPointerMove(651, 586);
        await waitFor(() => barPixel(666, 7) === main, 'Power hover accent');
        assert.ok(countColorPixels(barPixel, 640, 8, 22, 16, 0x001500) > 0,
            'Power hover glyph contrast');

        // Launcher selection is a bright accent row with dark text, never
        // one of the old green-on-green combinations.
        server.injectPointerMove(20, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const launcher = await waitFor(() => server.root.children.find(w => w.width === 300 && w.height === 308 && w.mapped), 'launcher');
        const launchPixel = (x, y) => launcher.raster.data[y * launcher.width + x] & 0xffffff;
        await waitFor(() => launchPixel(0, 0) === main, 'launcher backing-image paint');
        server.compose();
        let darkNamePixels = 0;
        for (let y = 53; y < 75; y++)
            for (let x = 44; x < 150; x++) if (launchPixel(x, y) === 0x001500) darkNamePixels++;
        assert.equal(launchPixel(290, 54), main, 'launcher selection was not a bright accent row');
        assert.ok(darkNamePixels > 0, 'selected launcher application name did not use contrasting dark text');
        server.injectPointerMove(20, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => !launcher.mapped, 'launcher close');

        const { X, display, connection } = await new Promise((resolve, reject) => {
            const connection = x11.createClient({ display: displayName }, (err, display) => {
                if (err) reject(err);
                else resolve({ X: display.client, display, connection });
            });
            connection.on('error', reject);
        });
        conn = connection;
        const root = display.screen[0].root;
        const wid = X.AllocID();
        X.CreateWindow(wid, root, 80, 80, 400, 300, 0, 0, 1, 0, {
            backgroundPixel: 0x202020,
            eventMask: x11.eventMask.Exposure | x11.eventMask.StructureNotify
        });
        X.ChangeProperty(0, wid, X.atoms.WM_NAME, X.atoms.STRING, 8, 'Regression Client');
        X.MapWindow(wid);
        const clientWin = await waitFor(() => {
            const w = server.resources.get(wid);
            return w && w.parent && w.parent.id !== root && w.mapped && w.parent.mapped ? w : null;
        }, 'managed test client');
        let frame = clientWin.parent;
        assert.equal(clientWin.width, 400);
        assert.equal(clientWin.height, 300);
        assert.ok(server.root.children.indexOf(frame) < server.root.children.indexOf(edgeRule),
            'frame must stack below the right rule');
        assert.ok(server.root.children.indexOf(edgeRule) < server.root.children.indexOf(bar),
            'right rule must stack below the taskbar');
        assert.equal(server.root.children[server.root.children.indexOf(bar) - 1], edgeRule,
            'right rule should be immediately below dock');
        assert.equal(server.root.children[server.root.children.indexOf(edgeRule) - 1], frame,
            'active frame should be immediately below right rule');

        // Focused close hover must contrast with the focused green titlebar.
        const bx = frame.width - 22;
        const framePixel = (x, y) => frame.raster.data[y * frame.width + x] & 0xffffff;
        await waitFor(() => framePixel(bx + 1, 4) === main, 'focused titlebar draw');
        server.injectPointerMove(frame.absX() + bx + 9, frame.absY() + 10);
        await delay(150);
        assert.equal(framePixel(bx + 1, 4), 0x001500, 'visible close-button hover');

        // A client-originated raise is mediated by the WM and cannot cover the dock.
        X.RaiseWindow(wid);
        await delay(120);
        assert.ok(server.root.children.indexOf(frame) < server.root.children.indexOf(bar), 'client raise covered taskbar');

        // The four permanent pin squares remain, but no running-window box
        // may appear in the empty area between the pins and power control.
        assert.equal(barPixel(168, 6), main, 'running app replaced a pinned shortcut');
        for (let y = 6; y < 22; y++) {
            for (let x = 328; x < 594; x++)
                assert.notEqual(barPixel(x, y), main, `running-window taskbar box at ${x},${y}`);
        }
        assert.equal(barPixel(594, 6), main, 'Wi-Fi button border before power control');
        assert.equal(barPixel(634, 6), main, 'power control sits beside the Wi-Fi button');

        // Minimise from the titlebar, then restore through the Windows picker.
        // The real client must retain 400x300 throughout both animations.
        let mark = configureLog.length;
        const minimiseX = frame.width - 66;
        server.injectPointerMove(frame.absX() + minimiseX + 9, frame.absY() + 10);
        server.injectButton(1, true);
        await waitFor(() => !clientWin.mapped && !frame.mapped, 'minimise on mouse press');
        await delay(100);
        assert.equal(clientWin.width, 400, 'minimise changed client width');
        assert.equal(clientWin.height, 300, 'minimise changed client height');
        assert.equal(configureLog.slice(mark).filter(e => e.wid === wid && (e.mask & 12)).length, 0,
            'minimise sent a real client resize');
        server.injectButton(1, false);
        await delay(60);

        mark = configureLog.length;
        server.injectPointerMove(100, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const picker = await waitFor(() => server.root.children.find(w => w.x === 48 && w.width === 300 && w.height === 30 && w.mapped), 'Windows picker');
        await delay(100);
        assert.equal(picker.mapped, true, 'opening click release dismissed the Windows picker');
        server.injectPointerMove(picker.absX() + 40, picker.absY() + 12);
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => clientWin.mapped && frame.mapped, 'restore through Windows picker');
        await delay(100);
        assert.equal(clientWin.width, 400, 'restore changed client width');
        assert.equal(clientWin.height, 300, 'restore changed client height');
        assert.equal(configureLog.slice(mark).filter(e => e.wid === wid && (e.mask & 12)).length, 0,
            `restore sent a real client resize: ${JSON.stringify(configureLog.slice(mark))}`);
        assert.ok(server.root.children.indexOf(frame) < server.root.children.indexOf(bar), 'restore stacked frame above taskbar');

        // Twenty rapid withdrawals/reuses must preserve the application window.
        // The old WM destroyed it with the frame on the very first unmap.
        for (let cycle = 0; cycle < 20; cycle++) {
            const oldFrame = frame;
            X.UnmapWindow(wid);
            await waitFor(() => {
                const w = server.resources.get(wid);
                return w && w.parent && w.parent.id === root && !server.resources.has(oldFrame.id);
            }, `withdraw/reparent cycle ${cycle + 1}`);
            assert.ok(server.resources.has(wid), `client was destroyed on unmap cycle ${cycle + 1}`);
            X.MapWindow(wid);
            await waitFor(() => {
                const w = server.resources.get(wid);
                return w && w.parent && w.parent.id !== root && w.mapped && w.parent.mapped;
            }, `remanage reused client cycle ${cycle + 1}`);
            frame = clientWin.parent;
        }
        assert.equal(clientWin.width, 400);
        assert.equal(clientWin.height, 300);
        assert.ok(server.root.children.indexOf(frame) < server.root.children.indexOf(bar), 'reopened frame covered taskbar');

        // Start the real Bash terminal and check cursor inheritance, icon-enabled
        // desktop matching, live resize buffering, and move-without-resize.
        terminal = spawn(path.join(repo, 'scos-terminal'), [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
        terminal.stdout.on('data', b => terminalLog += b);
        terminal.stderr.on('data', b => terminalLog += b);
        const termWin = await waitFor(() => {
            for (const res of server.resources.values()) {
                if (res.type === 'window' && property(server, res, 'WM_NAME') === 'SCos Terminal' && res.parent && res.parent.id !== root)
                    return res;
            }
            return null;
        }, 'standalone terminal managed', 8000);
        assert.equal(terminal.exitCode, null, `terminal exited: ${terminalLog}`);
        assert.ok(server.root.cursor, 'WM did not establish a root cursor');
        assert.equal(termWin.cursor, 0, 'terminal overrode session cursor instead of inheriting it');
        // The terminal pin's image is 12x12 at this bar height. Check its
        // interior, not a border pixel shared with an ordinary control box.
        await waitFor(() => countColorPixels(barPixel, 179, 8, 12, 12, 0x1a1a1a) > 20,
            'dedicated SCos terminal taskbar pin icon');

        const termFrame = termWin.parent;
        // Running-window access now lives only in the picker, and the real
        // dedicated application icon travels with its window into that menu.
        server.injectPointerMove(100, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const appPicker = await waitFor(() => server.root.children.find(w => w.x === 48 && w.width === 300 && w.height === 54 && w.mapped), 'two-window picker');
        const pickerPixel = (x, y) => appPicker.raster.data[y * appPicker.width + x] & 0xffffff;
        const pickerIconMatchesPin = () => {
            let same = 0;
            for (let y = 0; y < 12; y++)
                for (let x = 0; x < 12; x++) if (pickerPixel(8 + x, 9 + y) === barPixel(179 + x, 8 + y)) same++;
            return same;
        };
        await waitFor(() => pickerIconMatchesPin() > 120,
            'Windows picker real SCos Terminal icon paint');
        server.compose();
        server.injectPointerMove(appPicker.absX() + 40, appPicker.absY() + 12);
        await waitFor(() => pickerPixel(290, 4) === main, 'bright Windows-picker hover row');
        assert.ok(countColorPixels(pickerPixel, 40, 3, 210, 24, 0x001500) > 0,
            'Windows-picker hover text lacks contrast');
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => !appPicker.mapped, 'picker activation');

        const startW = termWin.width, startH = termWin.height;
        const resizeX = termFrame.absX() + termFrame.width - 5;
        const resizeY = termFrame.absY() + termFrame.height - 5;
        server.injectPointerMove(resizeX, resizeY);
        await delay(60);
        server.injectButton(1, true);
        await delay(60); // let the WM thaw the passive edge-drag grab
        server.injectPointerMove(resizeX - 200, resizeY - 90);
        await waitFor(() => termWin.width === startW - 200 && termWin.height === startH - 90,
            'pointer-driven terminal resize');
        server.injectButton(1, false);
        await delay(200);

        mark = configureLog.length;
        const oldFrameX = termFrame.absX(), oldFrameY = termFrame.absY();
        server.injectPointerMove(oldFrameX + 100, oldFrameY + 10);
        await delay(60);
        server.injectButton(1, true);
        await delay(80); // XGrabPointer is asynchronous, as on a real X server
        server.injectPointerMove(oldFrameX + 140, oldFrameY + 40);
        await waitFor(() => termFrame.absX() === oldFrameX + 40 && termFrame.absY() === oldFrameY + 30,
            'titlebar-driven terminal move');
        server.injectButton(1, false);
        await delay(150);
        assert.equal(configureLog.slice(mark).filter(e => e.wid === termWin.id && (e.mask & 12)).length, 0,
            'moving terminal reissued a client resize');
        assert.equal(termWin.width, startW - 200);
        assert.equal(termWin.height, startH - 90);

        // Real Bash/readline input must repaint changed rows, not the entire
        // terminal for every echoed key or pressed arrow. This is a rendering
        // scheduling assertion, not a claim about physical-display latency.
        await delay(400);
        const rasterSignature = w => {
            let hash = 2166136261;
            for (const pixel of w.raster.data) hash = Math.imul(hash ^ pixel, 16777619) >>> 0;
            return hash;
        };
        const beforeInput = rasterSignature(termWin);
        const copyMark = copyAreas.length;
        const key = async code => {
            server.injectKey(code, true); server.injectKey(code, false);
            await delay(3);
        };
        for (const code of [26, 54, 43, 32, 65, 38, 56, 54]) await key(code); // echo abc
        for (let i = 0; i < 12; i++) await key(113); // Left
        for (let i = 0; i < 12; i++) await key(114); // Right
        await key(36); // Return executes the command through Bash/readline
        const inputCopies = copyAreas.slice(copyMark).filter(c => c.dst === termWin.id);
        await waitFor(() => rasterSignature(termWin) !== beforeInput &&
            copyAreas.slice(copyMark).some(c => c.dst === termWin.id && c.height <= 24),
            'incremental terminal row paint after Bash and arrow input');
        inputCopies.push(...copyAreas.slice(copyMark).filter(c => c.dst === termWin.id));
        assert.ok(inputCopies.some(c => c.height <= 24), 'no changed-row terminal CopyArea observed');
        assert.ok(inputCopies.every(c => c.height < termWin.height),
            `typing/arrows caused a full terminal repaint: ${JSON.stringify(inputCopies)}`);
        const subcellCopyMark = copyAreas.length;
        const subcellWidth = termWin.width + 1, subcellHeight = termWin.height;
        X.ResizeWindow(termWin.id, subcellWidth, subcellHeight);
        await waitFor(() => termWin.width === subcellWidth, 'one-pixel terminal resize');
        await waitFor(() => copyAreas.slice(subcellCopyMark).some(c => c.dst === termWin.id &&
            c.width === subcellWidth && c.height === subcellHeight),
            'unchanged cell grid repainted into its new sub-cell backing pixmap');

        // A long OSC title must be ellipsized before the tab's Close glyph.
        // Compare that control with the normal tab, then activate it for real.
        const longName = 'system-host-name-that-is-far-too-long-for-a-terminal-tab';
        longTerminal = spawn(path.join(repo, 'scos-terminal'),
            ['-e', '/bin/sh', '-c', `printf '\\033]2;${longName}\\007'; cat`],
            { env, stdio: ['ignore', 'pipe', 'pipe'] });
        longTerminal.stdout.on('data', b => terminalLog += b);
        longTerminal.stderr.on('data', b => terminalLog += b);
        const longWin = await waitFor(() => {
            for (const res of server.resources.values())
                if (res.type === 'window' && res.parent && res.parent.id !== root &&
                    (property(server, res, '_NET_WM_NAME') || '').includes(longName)) return res;
            return null;
        }, 'long-title terminal managed');
        await waitFor(() => {
            let bright = 0;
            for (let y = 3; y < 20; y++) {
                for (let x = 58; x < 74; x++) {
                    const pixel = longWin.raster.data[y * longWin.width + x];
                    if (pixel !== termWin.raster.data[y * termWin.width + x]) return false;
                    if ((pixel & 0xffffff) === main) bright++;
                }
            }
            return bright > 0;
        }, 'long tab title stayed outside its visible Close control');
        // node-x11 documents AllowEvents as unmodelled, so it cannot ReplayPointer
        // from the WM's ordinary left-click passive grab into the application.
        longWin.buttonGrabs = [];
        server.injectPointerMove(longWin.absX() + 66, longWin.absY() + 10);
        assert.equal(server.pointerWindow().id, longWin.id, 'long-title Close click targeted another window');
        assert.equal(server.grabs.pointer, null, 'stale pointer grab intercepted long-title Close');
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => longTerminal.exitCode !== null || longTerminal.signalCode !== null,
            'long-title tab Close click');

        // Open Settings from its permanent pin with a normal press/release.
        // The release must not close the panel, and live scaling must update
        // both the real bar and the persisted configuration.
        server.injectPointerMove(305, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const settings = await waitFor(() => server.root.children.find(w => w.width === 540 && w.height === 390 && w.mapped), 'Settings panel');
        const settingsPixel = (x, y) => settings.raster.data[y * settings.width + x] & 0xffffff;
        await waitFor(() => settingsPixel(0, 0) === main, 'Settings backing-image paint');
        await delay(100);
        assert.equal(settings.mapped, true, 'opening click release closed Settings');
        assert.equal(settingsPixel(130, 64), main, 'selected Settings theme is not a bright accent');
        assert.ok(countColorPixels(settingsPixel, 30, 70, 100, 30, 0x001500) > 0,
            'selected Settings theme text lacks contrast');
        server.injectPointerMove(settings.absX() + 200, settings.absY() + 65);
        await waitFor(() => settingsPixel(250, 70) === main, 'unselected Settings theme hover fill');
        assert.ok(countColorPixels(settingsPixel, 165, 60, 100, 30, 0x001500) > 0,
            'Settings theme hover text lacks contrast');
        server.injectPointerMove(settings.absX() + 200, settings.absY() + 139);
        await waitFor(() => settingsPixel(510, 130) === main, 'Settings field hover fill');
        assert.ok(countColorPixels(settingsPixel, 108, 129, 390, 24, 0x001500) > 0,
            'Settings field hover text lacks contrast');
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => settingsPixel(520, 126) === main, 'active Settings field highlight');
        server.injectPointerMove(settings.absX() + 220, settings.absY() + 270);
        await waitFor(() => settingsPixel(234, 275) === main, 'Settings taskbar increment hover fill');
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => bar.height === 32 && bar.y === 568, 'live 32px taskbar scaling');
        const configFile = path.join(home, '.config/scoswm/scoswm.conf');
        const configValue = name => {
            const line = fs.readFileSync(configFile, 'utf8').split('\n')
                .find(s => new RegExp(`^\\s*${name}\\s*=`).test(s));
            return line ? line.slice(line.indexOf('=') + 1).trim() : null;
        };
        await waitFor(() => /bar_height\s*=\s*32/.test(fs.readFileSync(configFile, 'utf8')),
            'taskbar height configuration save');
        const savedConfig = fs.readFileSync(configFile, 'utf8');
        assert.match(savedConfig, /bar_height\s*=\s*32/);
        server.injectPointerMove(settings.absX() + 116, settings.absY() + 270);
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => bar.height === 28 && bar.y === 572, 'live 28px taskbar restoration');

        server.injectPointerMove(settings.absX() + 400, settings.absY() + 283);
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => configValue('desktop_icon_size') === '32', '32px desktop icon save');
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => configValue('desktop_icon_size') === '48', '48px desktop icon save');
        server.injectPointerMove(settings.absX() + 316, settings.absY() + 283);
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => configValue('desktop_icon_size') === '32', '32px desktop icon reduction');
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => configValue('desktop_icon_size') === '24', '24px desktop icon restoration');
        server.compose();
        const gridPixel = rootPixel(64, 100);
        server.injectPointerMove(settings.absX() + 480, settings.absY() + 283);
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => configValue('desktop_grid') === '0', 'desktop grid configuration save');
        server.compose();
        assert.notEqual(rootPixel(64, 100), gridPixel, 'wallpaper grid toggle did not repaint');
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => configValue('desktop_grid') === '1', 'desktop grid restoration');

        server.injectPointerMove(settings.absX() + 520, settings.absY() + 12);
        await waitFor(() => settingsPixel(530, 5) === 0x001500, 'Settings Close hover inversion');
        assert.ok(countColorPixels(settingsPixel, 516, 2, 22, 20, main) > 0,
            'Settings Close hover glyph lacks contrast');
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => !settings.mapped, 'Settings close');

        // Cover the rule with a real managed application. The dedicated rule
        // must still be painted above that frame, not merely above wallpaper.
        X.ResizeWindow(wid, 800, 500);
        await waitFor(() => clientWin.width === 800 && clientWin.height === 500,
            'covering application resize');
        await waitFor(() => {
            server.compose();
            return rootPixel(783, 100) === main && rootPixel(784, 100) === main &&
                rootPixel(785, 100) !== main;
        }, 'right rule visible above covering application');
        assert.ok(server.root.children.indexOf(edgeRule) > server.root.children.indexOf(frame),
            'right rule stacked below covering application');

        // Clear the real clients so every desktop gesture below reaches the
        // root window, just as it would on an empty physical desktop.
        X.DestroyWindow(wid);
        if (terminal.exitCode === null) terminal.kill('SIGKILL');
        await waitFor(() => terminal.exitCode !== null || terminal.signalCode !== null,
            'standalone terminal exit');
        await delay(200);

        const click = (x, y, button = 1) => {
            server.injectPointerMove(x, y);
            server.injectButton(button, true);
            server.injectButton(button, false);
        };
        const beginDrag = async (x, y) => {
            server.injectPointerMove(x, y);
            await delay(50);
            server.injectButton(1, true);
            await delay(180); // WM must process the press and pointer grab before motion
        };
        const clickShortcut = async (x, y) => {
            await beginDrag(x, y);
            server.injectButton(1, false);
            await delay(40);
        };

        // Every disruptive power-menu action requires a separate confirmation.
        // Cancel is the default: Return and Escape must never execute it.
        for (let action = 1; action <= 5; action++) {
            server.injectPointerMove(651, 586);
            server.injectButton(1, true);
            const powerMenu = await waitFor(() => server.root.children.find(w =>
                w.width === 190 && w.height === 150 && w.mapped), `power menu action ${action}`);
            server.injectButton(1, false);
            await delay(40);
            click(powerMenu.absX() + 60, powerMenu.absY() + 3 + action * 24 + 12);
            await waitFor(() => powerMenu.mapped && powerMenu.width === 380 && powerMenu.height === 126,
                `confirmation for power action ${action}`);
            assert.equal(wm.exitCode, null, `power action ${action} ran before confirmation`);
            if (action === 1) await key(36); // Return activates safe default Cancel
            else if (action === 2) await key(9); // Escape
            else click(powerMenu.absX() + 50, powerMenu.absY() + 100); // Cancel
            await waitFor(() => !powerMenu.mapped, `cancel confirmation for power action ${action}`);
            assert.equal(wm.exitCode, null, `cancelled power action ${action} still ran`);
        }

        // Click-hold-drag on empty wallpaper paints an actual rubber band,
        // and releasing it leaves a selection only for intersected icons.
        await beginDrag(500, 400);
        server.injectPointerMove(560, 450);
        await waitFor(() => { server.compose(); return rootPixel(500, 420) === main; },
            'visible desktop rubber-band border');
        server.injectButton(1, false);
        await waitFor(() => { server.compose(); return rootPixel(500, 420) !== main; },
            'rubber-band release repaint');

        // Drag Terminal from slot 0 to empty slot 13 (column 5, row 1).
        await beginDrag(56, 40);
        server.injectPointerMove(500, 150);
        await waitFor(() => { server.compose(); return rootPixel(456, 140) === main; },
            'desktop nearest-grid snap preview');
        server.injectButton(1, false);
        await waitFor(() => configValue('desktop_slots') === '13,1,2,3',
            'persistent nearest-grid desktop placement');
        assert.equal(configValue('desktop_pins'), 'terminal,files,sysmon,settings');

        // Move the same shortcut to the end of the taskbar. This catches the
        // existing-pin append case as well as desktop removal and the marker.
        await beginDrag(496, 140);
        server.injectPointerMove(330, 586);
        await waitFor(() => { server.compose(); return barPixel(325, 10) === main; },
            'visible taskbar insertion marker');
        server.injectButton(1, false);
        await waitFor(() => configValue('pins') === 'files,sysmon,settings,terminal' &&
            configValue('desktop_pins') === 'files,sysmon,settings',
            'desktop-to-taskbar move persistence');
        assert.equal(configValue('desktop_slots'), '1,2,3');

        // Drag Terminal from its new taskbar position to empty desktop slot
        // 22 (column 6, row 2), removing only the taskbar copy.
        await beginDrag(305, 586);
        server.injectPointerMove(600, 250);
        await waitFor(() => { server.compose(); return rootPixel(544, 230) === main; },
            'taskbar-to-desktop snap preview');
        server.injectButton(1, false);
        await waitFor(() => configValue('pins') === 'files,sysmon,settings' &&
            configValue('desktop_pins') === 'files,sysmon,settings,terminal',
            'taskbar-to-desktop move persistence');
        assert.equal(configValue('desktop_slots'), '1,2,3,22');

        const shortcutMenu = () => server.root.children.find(w => w.width === 220 && w.mapped);
        const chooseShortcutRow = async row => {
            const menu = await waitFor(shortcutMenu, 'shortcut context menu');
            click(menu.absX() + 100, menu.absY() + 13 + row * 24);
            await waitFor(() => !menu.mapped, 'shortcut context action');
        };

        // Settings remains on the desktop when its taskbar pin is removed,
        // and can be added back without duplicating its desktop shortcut.
        click(265, 586, 3);
        await waitFor(shortcutMenu, 'taskbar shortcut menu');
        await chooseShortcutRow(3); // Remove from taskbar
        await waitFor(() => configValue('pins') === 'files,sysmon',
            'right-click taskbar removal');
        assert.equal(configValue('desktop_pins'), 'files,sysmon,settings,terminal');

        click(320, 40, 3); // desktop Settings, still slot 3
        await waitFor(shortcutMenu, 'desktop shortcut menu');
        await chooseShortcutRow(1); // Add to taskbar
        await waitFor(() => configValue('pins') === 'files,sysmon,settings',
            'desktop context Add to taskbar');
        assert.equal(configValue('desktop_pins'), 'files,sysmon,settings,terminal');

        // Surface-specific desktop removal must leave SysMon on the taskbar.
        click(232, 40, 3); // desktop SysMon, slot 2
        await waitFor(shortcutMenu, 'desktop removal menu');
        await chooseShortcutRow(3); // Remove from desktop
        await waitFor(() => configValue('desktop_pins') === 'files,settings,terminal',
            'right-click desktop removal');
        assert.equal(configValue('pins'), 'files,sysmon,settings');

        // Launch a separate process through the actual Files command. Its
        // multi-size _NET_WM_ICON changes during initialization, then the
        // window is withdrawn and launched again to probe the reported
        // file-manager path rather than an in-process-only approximation.
        const findFileClient = () => {
            for (const res of server.resources.values())
                if (res.type === 'window' && res.mapped && res.parent && res.parent.id !== 1 &&
                    property(server, res, 'WM_CLASS') === 'thunar' &&
                    property(server, res, 'WM_NAME') === 'Home - File Manager') return res;
            return null;
        };
        for (let cycle = 0; cycle < 12; cycle++) {
            click(185, 586); // Files is taskbar pin 0 after the shortcut tests
            const fileClient = await waitFor(findFileClient, `external Files launch ${cycle + 1}`);
            await delay(140); // several live icon replacements
            assert.equal(wm.exitCode, null, `WM exited on Files launch ${cycle + 1}: ${wmLog}`);
            assert.equal(wm.signalCode, null, `WM signalled on Files launch ${cycle + 1}: ${wmLog}`);
            const frame = fileClient.parent;
            X.DestroyWindow(fileClient.id);
            try {
                await waitFor(() => !server.resources.has(frame.id),
                    `external Files withdrawal ${cycle + 1}`);
            } catch (err) {
                console.error('Files withdrawal debug', {
                    cycle: cycle + 1, client: fileClient.id, frame: frame.id,
                    wmExit: wm.exitCode, wmSignal: wm.signalCode,
                    clientMapped: fileClient.mapped, frameMapped: frame.mapped,
                    fileWindows: [...server.resources.values()].filter(r => r.type === 'window' &&
                        property(server, r, 'WM_CLASS') === 'thunar')
                        .map(r => ({ id: r.id, mapped: r.mapped, parent: r.parent && r.parent.id })),
                    handlerErrors: handlerErrors.map(x => String(x.err))
                });
                throw err;
            }
            assert.equal(wm.exitCode, null, `WM exited on Files withdrawal ${cycle + 1}: ${wmLog}`);
            assert.equal(wm.signalCode, null, `WM signalled on Files withdrawal ${cycle + 1}: ${wmLog}`);
            await delay(60);
        }

        // Restart with a large installed-app set so the earlier curated-
        // launcher assertions remain small and deterministic. Unique icon
        // names/paths force separate cache entries and repeated PNG decodes.
        const stressApps = path.join(home, '.local/share/applications');
        const stressIcons = path.join(home, '.local/share/icons/hicolor/24x24/apps');
        const stressPng = fs.readFileSync(path.join(repo, 'packaging/icons/hicolor/24x24/apps/scos-files.png'));
        for (let i = 0; i < 250; i++) {
            const id = `stress-${String(i).padStart(3, '0')}`;
            fs.writeFileSync(path.join(stressApps, `${id}.desktop`),
                `[Desktop Entry]\nType=Application\nName=Stress ${String(i).padStart(3, '0')}\nExec=/bin/true\nIcon=${id}\n`);
            fs.writeFileSync(path.join(stressIcons, `${id}.png`), stressPng);
        }
        wm.kill('SIGKILL');
        await waitFor(() => wm.exitCode !== null || wm.signalCode !== null, 'WM restart for launcher stress');
        await delay(150);
        wm = spawn(path.join(repo, 'scoswm'), [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
        wm.stdout.on('data', b => wmLog += b);
        wm.stderr.on('data', b => wmLog += b);
        const stressBar = await waitFor(() => server.root.children.find(w => w.width === 800 && w.height === 28 && w.y === 572 && w.mapped),
            'restarted stress WM taskbar');
        const stressBarPixel = (x, y) => stressBar.raster.data[y * stressBar.width + x] & 0xffffff;
        assert.equal(wm.exitCode, null, `stress WM exited during startup: ${wmLog}`);

        click(25, 586);
        const stressLauncher = await waitFor(() => server.root.children.find(w => w.width === 300 && w.height === 308 && w.mapped),
            'stress launcher');
        const stressPixel = (x, y) => stressLauncher.raster.data[y * stressLauncher.width + x] & 0xffffff;
        click(stressLauncher.absX() + 100, stressLauncher.absY() + 162); // All installed apps..., row 4
        await waitFor(() => { server.compose(); return stressPixel(290, 54) === main; },
            'stress All Apps selection');
        server.injectPointerMove(stressLauncher.absX() + 150, stressLauncher.absY() + 150);
        for (let i = 0; i < 600; i++) {
            const button = i < 300 ? 5 : 4;
            server.injectButton(button, true);
            server.injectButton(button, false);
            if (i % 25 === 24) {
                await delay(12);
                assert.equal(wm.exitCode, null, `WM exited during launcher scroll ${i + 1}: ${wmLog}`);
                assert.equal(wm.signalCode, null, `WM signalled during launcher scroll ${i + 1}: ${wmLog}`);
            }
            if (i === 29)
                await waitFor(() => { server.compose(); return stressPixel(290, 270) === main; },
                    'populated launcher reached its lower scroll rows');
        }
        await waitFor(() => { server.compose(); return stressPixel(290, 54) === main; },
            'launcher selection after 600 wheel events');
        assert.equal(stressLauncher.mapped, true, 'launcher closed during prolonged scrolling');
        assert.equal(wm.exitCode, null, `WM exited after prolonged scrolling: ${wmLog}`);
        assert.equal(wm.signalCode, null, `WM signalled after prolonged scrolling: ${wmLog}`);

        click(25, 586);
        await waitFor(() => !stressLauncher.mapped, 'stress launcher close before standalone Files');

        // Exercise the bundled application itself, independently of the
        // external-manager override stressed above. All paths are real.
        filesApp = spawn(path.join(repo, 'scos-files'), [home], { env, stdio: ['ignore', 'pipe', 'pipe'] });
        filesApp.stdout.on('data', b => wmLog += b);
        filesApp.stderr.on('data', b => wmLog += b);
        const filesWin = await waitFor(() => {
            for (const res of server.resources.values())
                if (res.type === 'window' && res.mapped && res.parent && res.parent.id !== root &&
                    (property(server, res, 'WM_NAME') || '').startsWith('SCos Files -')) return res;
            return null;
        }, 'bundled standalone Files managed');
        filesWin.buttonGrabs = []; // same node-x11 ReplayPointer limitation as above
        click(filesWin.absX() + 50, filesWin.absY() + 58);
        await delay(50);
        click(filesWin.absX() + 50, filesWin.absY() + 58);
        await waitFor(() => property(server, filesWin, '_NET_WM_NAME') === 'SCos Files - bin',
            'double-click real directory navigation');
        click(filesWin.absX() + 125, filesWin.absY() + 21); // New Folder toolbar
        const dialogX = (filesWin.width - 420) / 2, dialogY = (filesWin.height - 142) / 2;
        click(filesWin.absX() + dialogX + 276, filesWin.absY() + dialogY + 120); // OK
        const createdFolder = path.join(home, 'bin/new folder');
        await waitFor(() => fs.existsSync(createdFolder), 'Files New Folder dialog created a real directory');
        click(filesWin.absX() + 50, filesWin.absY() + 58, 3);
        click(filesWin.absX() + 70, filesWin.absY() + 97); // Rename context row
        for (const code of [27, 26, 57, 38, 58, 26, 40]) await key(code); // renamed
        await key(36);
        await waitFor(() => fs.existsSync(path.join(home, 'bin/renamed')) && !fs.existsSync(createdFolder),
            'Files context rename changed the real directory');
        const filesFrame = filesWin.parent;
        const filesCloseX = filesFrame.absX() + filesFrame.width - 17; // x=783: thin-rule column
        const filesCloseY = filesFrame.absY() + 10;
        server.injectPointerMove(filesCloseX, filesCloseY);
        assert.equal(server.pointerWindow(), filesFrame, 'right rule intercepted Files Close input');
        click(filesCloseX, filesCloseY);
        await waitFor(() => filesApp.exitCode !== null || filesApp.signalCode !== null,
            'standalone Files WM_DELETE close');

        // The delayed fake nmcli proves the WM remains event-driven during a
        // scan, then verifies per-device rows, scrolling and ifname connect.
        click(611, 586);
        const wifiPanel = await waitFor(() => server.root.children.find(w => w.width === 540 && w.height === 390 && w.mapped),
            'Wi-Fi panel');
        await waitFor(() => fs.existsSync(`${nmcliLog}.started`), 'asynchronous nmcli scan started');
        assert.equal(fs.existsSync(`${nmcliLog}.finished`), false);
        server.injectPointerMove(651, 586);
        await waitFor(() => stressBarPixel(666, 7) === main, 'taskbar hover during delayed Wi-Fi scan', 800);
        assert.equal(fs.existsSync(`${nmcliLog}.finished`), false, 'WM blocked until nmcli finished');
        await waitFor(() => fs.existsSync(`${nmcliLog}.finished`), 'fake Wi-Fi scan results');
        await delay(150);
        const wifiPixel = (x, y) => wifiPanel.raster.data[y * wifiPanel.width + x] & 0xffffff;
        server.injectPointerMove(wifiPanel.absX() + 150, wifiPanel.absY() + 200);
        await waitFor(() => wifiPixel(530, 185) === main, 'Wi-Fi row hover fill');
        const unscrolledWifi = rasterSignature(wifiPanel);
        click(wifiPanel.absX() + 150, wifiPanel.absY() + 200, 5);
        await waitFor(() => rasterSignature(wifiPanel) !== unscrolledWifi, 'Wi-Fi network wheel scrolling');
        click(wifiPanel.absX() + 150, wifiPanel.absY() + 200, 4);
        await waitFor(() => rasterSignature(wifiPanel) === unscrolledWifi, 'Wi-Fi network scroll restoration');
        server.injectPointerMove(wifiPanel.absX() + 382, wifiPanel.absY() + 333);
        await waitFor(() => wifiPixel(410, 325) === main, 'Wi-Fi Connect hover fill');
        assert.ok(countColorPixels(wifiPixel, 340, 322, 160, 24, 0x001500) > 0,
            'Wi-Fi Connect hover text lacks contrast');
        server.injectPointerMove(wifiPanel.absX() + 480, wifiPanel.absY() + 333);
        await waitFor(() => wifiPixel(500, 325) === main, 'Wi-Fi Scan hover fill');
        click(wifiPanel.absX() + 30, wifiPanel.absY() + 168); // active HomeNet must sort before stronger inactive Coffee
        await waitFor(() => wifiPixel(10, 156) === main, 'connected network was not sorted first');
        click(wifiPanel.absX() + 382, wifiPanel.absY() + 333);
        await waitFor(() => fs.existsSync(nmcliLog), 'first active Wi-Fi row connect arguments');
        assert.deepEqual(fs.readFileSync(nmcliLog, 'utf8').trim().split('\n'),
            ['device', 'wifi', 'connect', 'HomeNet', 'ifname', 'wlp1s0']);
        fs.rmSync(nmcliLog);
        click(wifiPanel.absX() + 30, wifiPanel.absY() + 192); // second row: HomeNet on wlp2s0
        await waitFor(() => wifiPixel(10, 182) === main, 'second-interface Wi-Fi row selected');
        click(wifiPanel.absX() + 382, wifiPanel.absY() + 333); // Connect
        await waitFor(() => fs.existsSync(nmcliLog), 'Wi-Fi connect arguments');
        assert.deepEqual(fs.readFileSync(nmcliLog, 'utf8').trim().split('\n'),
            ['device', 'wifi', 'connect', 'HomeNet', 'ifname', 'wlp2s0']);
        server.injectPointerMove(wifiPanel.absX() + 520, wifiPanel.absY() + 12);
        await waitFor(() => wifiPixel(530, 5) === 0x001500, 'Wi-Fi Close hover inversion');
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => !wifiPanel.mapped, 'Wi-Fi panel close');

        click(265, 586);
        const sysmonPanel = await waitFor(() => server.root.children.find(w =>
            w.width === 540 && w.height === 390 && w.mapped), 'SysMon panel');
        const sysmonPixel = (x, y) => sysmonPanel.raster.data[y * sysmonPanel.width + x] & 0xffffff;
        server.injectPointerMove(sysmonPanel.absX() + 520, sysmonPanel.absY() + 12);
        await waitFor(() => sysmonPixel(530, 5) === 0x001500, 'SysMon Close hover inversion');
        server.injectButton(1, true); server.injectButton(1, false);
        await waitFor(() => !sysmonPanel.mapped, 'SysMon panel close');

        // Super+F1/F2/F3 are non-conflicting fallbacks for keyboards whose
        // firmware does not emit XF86AudioMute/LowerVolume/RaiseVolume.
        for (const code of [67, 68, 69]) {
            server.injectKey(133, true); // Super_L
            await key(code);
            server.injectKey(133, false);
        }
        const mediaLog = `${nmcliLog}.media`;
        await waitFor(() => fs.existsSync(mediaLog) &&
            fs.readFileSync(mediaLog, 'utf8').trim().split('\n').length >= 3,
            'configurable media-key commands');
        const mediaActions = fs.readFileSync(mediaLog, 'utf8').trim().split('\n');
        assert.equal(mediaActions.length, 3, 'media key was lost or executed more than once');
        assert.deepEqual(mediaActions.sort(), ['down', 'mute', 'up']);
        assert.equal(wm.exitCode, null, `WM exited during standalone Files/Wi-Fi checks: ${wmLog}`);

        // A non-core .desktop pin gives both surfaces the same real process
        // and WM_CLASS. Double-click must always spawn, while a single click
        // must restore/activate the most recently active matching client.
        fs.writeFileSync(path.join(stressApps, 'thunar.desktop'),
            `[Desktop Entry]\nType=Application\nName=Multi Instance Test\nExec=${path.join(repo, 'test_file_client')}\nIcon=scos-files\n`);
        fs.writeFileSync(configFile,
            `bar_height=28\npins=thunar\ndesktop_pins=thunar\ndesktop_slots=0\nfiles=${path.join(repo, 'test_file_client')}\n`);
        wm.kill('SIGKILL');
        await waitFor(() => wm.exitCode !== null || wm.signalCode !== null,
            'WM restart for multi-instance shortcuts');
        await delay(150);
        wm = spawn(path.join(repo, 'scoswm'), [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
        wm.stdout.on('data', b => wmLog += b);
        wm.stderr.on('data', b => wmLog += b);
        const multiBar = await waitFor(() => server.root.children.find(w =>
            w.width === 800 && w.height === 28 && w.y === 572 && w.mapped),
            'multi-instance taskbar');
        const multiClients = () => [...server.resources.values()].filter(res =>
            res.type === 'window' && res.parent && res.parent.id !== 1 &&
            property(server, res, 'WM_CLASS') === 'thunar' &&
            property(server, res, 'WM_NAME') === 'Home - File Manager');
        const activeWindow = () => {
            const atom = server.atomsByName.get('_NET_ACTIVE_WINDOW');
            const prop = atom && server.root.properties.get(atom);
            return prop && prop.data.length >= 4 ? prop.data.readUInt32LE(0) : 0;
        };

        await clickShortcut(56, 40); // desktop, first half of a double-click
        await delay(70);
        await clickShortcut(56, 40);
        await waitFor(() => multiClients().length === 1, 'desktop double-click initial instance');
        await delay(450); // outside the double-click interval on a real X server
        await clickShortcut(56, 40);
        await delay(70);
        await clickShortcut(56, 40);
        await waitFor(() => multiClients().length === 2, 'desktop double-click new instance while running');

        await clickShortcut(185, 586); // taskbar double-click with two instances running
        await delay(70);
        await clickShortcut(185, 586);
        await waitFor(() => multiClients().length === 3, 'taskbar double-click new instance while running');
        const instances = multiClients();
        const oldest = instances[0], middle = instances[1], newest = instances[2];
        await waitFor(() => activeWindow() === newest.id, 'new taskbar instance focused');
        const multiRule = await waitFor(() => server.root.children.find(w =>
            property(server, w, 'WM_NAME') === 'SCosWM desktop right rule' && w.mapped),
            'multi-instance right rule');

        // Right-clicking Windows shows the real clients in an MRU overview.
        // The newest window occupies the large upper-left tile; two other
        // clients surround it on the right. The 32px outer margin is clean.
        const openOverview = async () => {
            await waitFor(() => !server.grabs.pointer,
                'taskbar pointer grab settles before another overview gesture');
            click(100, 586, 3);
            return waitFor(() => server.root.children.find(w =>
                property(server, w, 'WM_NAME') === 'SCosWM window overview' && w.mapped),
                'right-click visual window overview');
        };
        let overview = await openOverview();
        assert.equal(overview.width, 800);
        assert.equal(overview.height, 572, 'overview must leave the taskbar uncovered');
        assert.equal(multiBar.mapped, true, 'taskbar hidden while overview is open');
        assert.equal(multiRule.mapped, false, 'desktop right rule was not hidden with the overview');
        assert.ok(server.root.children.indexOf(multiBar) > server.root.children.indexOf(overview),
            'taskbar stacked below overview');
        const overviewPixel = (x, y) => overview.raster.data[y * overview.width + x] & 0xffffff;
        await waitFor(() => overviewPixel(32, 80) === main, 'largest upper-left overview tile');
        assert.equal(overviewPixel(481, 80), main, 'second overview tile');
        assert.equal(overviewPixel(481, 310), main, 'third overview tile above taskbar');
        assert.equal(overviewPixel(464, 100), main, 'right side of overview tile is not green');
        assert.notEqual(overviewPixel(10, 10), overviewPixel(10, 550),
            'overview wallpaper gradient is missing');
        assert.equal(overviewPixel(10, 64), overviewPixel(20, 64),
            'overview wallpaper grid is missing from the clean margin');
        assert.notEqual(overviewPixel(10, 64), main,
            'desktop icon or tile painted in overview margin');
        click(550, 150); // second MRU tile: middle instance
        await waitFor(() => !overview.mapped && activeWindow() === middle.id && multiRule.mapped,
            'overview tile activates its real window and restores the rule');
        assert.equal(multiClients().length, 3, 'overview activation closed or spawned a client');

        overview = await openOverview();
        click(550, 150); // after activating middle, this tile is now newest
        await waitFor(() => !overview.mapped && activeWindow() === newest.id,
            'overview tiles did not refresh to MRU order');
        overview = await openOverview();
        click(100, 586, 3); // the same taskbar button toggles the overview shut
        await waitFor(() => !overview.mapped && multiRule.mapped,
            'Windows taskbar button closes visual overview');
        assert.equal(activeWindow(), newest.id, 'closing overview changed active window');
        assert.equal(multiClients().length, 3, 'right-click overview close affected a client');

        overview = await openOverview();
        await clickShortcut(185, 586); // taskbar action closes overview, then activates MRU match
        await waitFor(() => !overview.mapped && activeWindow() === newest.id,
            'taskbar pin did not activate normally after overview');
        assert.equal(multiClients().length, 3, 'taskbar pin spawned a client while closing overview');

        overview = await openOverview();
        click(25, 586); // Start is another ordinary taskbar action
        const overviewLauncher = await waitFor(() => server.root.children.find(w =>
            w.width === 300 && w.height === 308 && w.mapped),
            'launcher opens normally after closing overview');
        assert.equal(overview.mapped, false, 'Start left the overview open');
        click(25, 586);
        await waitFor(() => !overviewLauncher.mapped, 'launcher closes after overview taskbar test');
        assert.equal(multiClients().length, 3, 'Start taskbar action affected application instances');

        // Minimise newest to oldest. Each minimisation focuses the next
        // visible instance, so once all three are minimised the oldest is the
        // most recently active matching window. Each single-click surface
        // must restore that window, not spawn or choose by creation order.
        for (const [client, label] of [[newest, 'newest'], [middle, 'middle'], [oldest, 'oldest']]) {
            click(client.parent.absX() + client.parent.width - 57, client.parent.absY() + 10);
            await waitFor(() => !client.mapped, `${label} instance minimised before desktop single-click`);
        }
        await clickShortcut(56, 40);
        await waitFor(() => oldest.mapped && activeWindow() === oldest.id,
            'desktop single-click restores most recent matching window');
        assert.equal(multiClients().length, 3, 'desktop single-click spawned an extra instance');

        click(oldest.parent.absX() + oldest.parent.width - 57, oldest.parent.absY() + 10);
        await waitFor(() => !oldest.mapped, 'most recent instance minimised before taskbar single-click');
        await clickShortcut(185, 586);
        await waitFor(() => oldest.mapped && activeWindow() === oldest.id,
            'taskbar single-click restores most recent matching window');
        assert.equal(multiClients().length, 3, 'taskbar single-click spawned an extra instance');
        for (const client of multiClients()) X.DestroyWindow(client.id);
        await waitFor(() => multiClients().length === 0, 'multi-instance client cleanup');
        assert.equal(wm.exitCode, null, `WM exited during multi-instance shortcut checks: ${wmLog}`);

        assert.deepEqual(directDraws, [], `visible-window drawing bypassed backing pixmaps: ${JSON.stringify(directDraws)}`);
        assert.deepEqual(handlerErrors, [], `X server handler errors: ${handlerErrors.map(x => String(x.err)).join(', ')}`);
        console.log('graphical WM: visible click-through right rule, compact date/Wi-Fi geometry, 28px bounds and power glyph OK');
        console.log('graphical power: five disruptive actions require confirmation; mouse/Return/Escape cancellation OK');
        console.log('graphical WM: high-contrast launcher/menu/taskbar/Settings states, close hover and dock stacking OK');
        console.log('graphical icons: dedicated Terminal/Files/SysMon/Settings desktop, pin and picker images OK');
        console.log('graphical cursor: preserved Qt resources, GTK XSETTINGS and themed inheritance OK');
        console.log('graphical WM: pinned-only bar, picker minimise/restore and 20 safe window-reuse cycles OK');
        console.log('graphical WM: no real client resize during animation or move OK');
        console.log('graphical terminal: real Bash startup and buffered resize/move OK');
        console.log('graphical Settings: mouse-open persistence, taskbar scaling, desktop icon sizes and grid OK');
        console.log('graphical shortcuts: snap/drop previews, persistent bidirectional drags, independent pins and context removal OK');
        console.log('graphical desktop: visible click-hold rubber-band selection and release OK');
        console.log('graphical Files: 12 external launches, live multi-size client icons and safe withdrawals OK');
        console.log('graphical launcher: 250 apps, separate PNG cache entries and 600 wheel events OK');
        console.log('graphical terminal: real Bash/arrows use incremental rows; long-title Close remains visible/clickable OK');
        console.log('graphical standalone Files: real navigation, New Folder, rename and KDE-class application lifetime OK');
        console.log('graphical Wi-Fi: nonblocking scan, active-first rows, hover, scrolling and per-ifname connect arguments OK');
        console.log('graphical media keys: configurable Super+F1/F2/F3 mute/down/up commands execute once each OK');
        console.log('graphical shortcuts: desktop/taskbar double-click multi-instance and single-click last-window activation OK');
        console.log('graphical overview: wallpaper, green borders, taskbar stacking/actions and repeated MRU dismissal OK');
        console.log('graphical drawing: all monitored WM/terminal painting used backing pixmaps');
    } catch (err) {
        console.error('WM log on failure:', wmLog);
        console.error('Terminal log on failure:', terminalLog);
        throw err;
    } finally {
        if (terminal && terminal.exitCode === null && terminal.signalCode === null) terminal.kill('SIGTERM');
        if (longTerminal && longTerminal.exitCode === null && longTerminal.signalCode === null) longTerminal.kill('SIGTERM');
        if (filesApp && filesApp.exitCode === null && filesApp.signalCode === null) filesApp.kill('SIGTERM');
        if (wm && wm.exitCode === null && wm.signalCode === null) wm.kill('SIGTERM');
        if (conn) conn.terminate();
        await delay(100);
        net.close();
        fs.rmSync(home, { recursive: true, force: true });
        if (process.exitCode) {
            console.error('WM log:', wmLog);
            console.error('Terminal log:', terminalLog);
            console.error('Direct draws:', directDraws);
            console.error('Handler errors:', handlerErrors);
        }
    }
})().catch(err => {
    console.error(err.stack || err);
    process.exitCode = 1;
    // Do not leave the X listener alive after a failed assertion.
    setTimeout(() => process.exit(1), 250).unref();
});
