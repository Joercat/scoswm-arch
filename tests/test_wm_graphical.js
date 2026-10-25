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

    const home = fs.mkdtempSync(path.join(os.tmpdir(), 'scoswm-graphical-'));
    fs.mkdirSync(path.join(home, '.config/scoswm'), { recursive: true });
    fs.writeFileSync(path.join(home, '.config/scoswm/scoswm.conf'), 'bar_height=28\npins=terminal,files,sysmon,settings\n');
    fs.mkdirSync(path.join(home, '.local/share/applications'), { recursive: true });
    fs.mkdirSync(path.join(home, '.local/share/icons'), { recursive: true });
    fs.copyFileSync(path.join(repo, 'packaging/scos-terminal.desktop'), path.join(home, '.local/share/applications/scos-terminal.desktop'));
    fs.cpSync(path.join(repo, 'packaging/icons/hicolor'), path.join(home, '.local/share/icons/hicolor'), { recursive: true });
    const env = { ...process.env, DISPLAY: displayName, HOME: home };
    delete env.XDG_CONFIG_HOME;
    delete env.XDG_DATA_HOME;
    delete env.XDG_DATA_DIRS;
    let wm, terminal, conn;
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
        const main = 0x39ff14;
        server.compose();
        const rootPixel = (x, y) => server.root.raster.data[y * 800 + x] & 0xffffff;
        assert.equal(rootPixel(799, 100), main, 'right desktop border');
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
        const controlRanges = [[8, 42], [48, 158], [168, 323], [650, 684]];
        for (const [x0, x1] of controlRanges) {
            for (let y = 22; y < 28; y++) {
                for (let x = x0; x < x1; x++)
                    assert.notEqual(barPixel(x, y), main, `taskbar glyph escaped box at ${x},${y}`);
            }
        }
        // At 28px the power circle is centred at (667,15), radius 6.
        // Its right-hand side must be drawn: a right-side gap was the old,
        // incorrectly rotated symbol; the gap is now upward around the stem.
        assert.equal(barPixel(673, 15), main, 'power symbol does not open upward');

        // Launcher selection keeps application names in the normal bright
        // text colour, rather than the dark text-on-accent title colour.
        server.injectPointerMove(20, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const launcher = await waitFor(() => server.root.children.find(w => w.width === 300 && w.height === 308 && w.mapped), 'launcher');
        const launchPixel = (x, y) => launcher.raster.data[y * launcher.width + x] & 0xffffff;
        await waitFor(() => launchPixel(0, 0) === main, 'launcher backing-image paint');
        server.compose();
        let brightNamePixels = 0, darkNamePixels = 0;
        for (let y = 53; y < 75; y++) {
            for (let x = 44; x < 150; x++) {
                if (launchPixel(x, y) === main) brightNamePixels++;
                if (launchPixel(x, y) === 0x001500) darkNamePixels++;
            }
        }
        assert.ok(brightNamePixels > 0, 'selected launcher application name was not painted in normal text colour');
        assert.equal(darkNamePixels, 0, 'selected launcher application name used dark titlebar text');
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
        assert.ok(server.root.children.indexOf(frame) < server.root.children.indexOf(bar), 'frame must stack below taskbar');
        assert.equal(server.root.children[server.root.children.indexOf(bar) - 1], frame, 'active frame should be immediately below dock');

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
            for (let x = 328; x < 640; x++)
                assert.notEqual(barPixel(x, y), main, `running-window taskbar box at ${x},${y}`);
        }

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
        const countMainPixels = (pixel, x0, y0, w, h) => {
            let count = 0;
            for (let y = y0; y < y0 + h; y++)
                for (let x = x0; x < x0 + w; x++) if (pixel(x, y) === main) count++;
            return count;
        };
        // The terminal pin's image is 12x12 at this bar height. Check its
        // interior, not a border pixel shared with an ordinary control box.
        await waitFor(() => countMainPixels(barPixel, 179, 8, 12, 12) > 20,
            'dedicated SCos terminal taskbar pin icon');

        const termFrame = termWin.parent;
        // Running-window access now lives only in the picker, and the real
        // dedicated application icon travels with its window into that menu.
        server.injectPointerMove(100, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const appPicker = await waitFor(() => server.root.children.find(w => w.x === 48 && w.width === 300 && w.height === 54 && w.mapped), 'two-window picker');
        const pickerPixel = (x, y) => appPicker.raster.data[y * appPicker.width + x] & 0xffffff;
        await waitFor(() => countMainPixels(pickerPixel, 8, 9, 12, 12) > 20,
            'Windows picker real SCos Terminal icon paint');
        server.compose();
        server.injectPointerMove(appPicker.absX() + 40, appPicker.absY() + 12);
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

        // Open Settings from its permanent pin with a normal press/release.
        // The release must not close the panel, and live scaling must update
        // both the real bar and the persisted configuration.
        server.injectPointerMove(305, 586);
        server.injectButton(1, true);
        server.injectButton(1, false);
        const settings = await waitFor(() => server.root.children.find(w => w.width === 540 && w.height === 390 && w.mapped), 'Settings panel');
        await delay(100);
        assert.equal(settings.mapped, true, 'opening click release closed Settings');
        server.injectPointerMove(settings.absX() + 220, settings.absY() + 270);
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => bar.height === 32 && bar.y === 568, 'live 32px taskbar scaling');
        const configFile = path.join(home, '.config/scoswm/scoswm.conf');
        await waitFor(() => /bar_height\s*=\s*32/.test(fs.readFileSync(configFile, 'utf8')),
            'taskbar height configuration save');
        const savedConfig = fs.readFileSync(configFile, 'utf8');
        assert.match(savedConfig, /bar_height\s*=\s*32/);
        server.injectPointerMove(settings.absX() + 116, settings.absY() + 270);
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => bar.height === 28 && bar.y === 572, 'live 28px taskbar restoration');
        server.injectPointerMove(settings.absX() + 520, settings.absY() + 12);
        server.injectButton(1, true);
        server.injectButton(1, false);
        await waitFor(() => !settings.mapped, 'Settings close');

        assert.deepEqual(directDraws, [], `visible-window drawing bypassed backing pixmaps: ${JSON.stringify(directDraws)}`);
        assert.deepEqual(handlerErrors, [], `X server handler errors: ${handlerErrors.map(x => String(x.err)).join(', ')}`);
        console.log('graphical WM: border, centered labels, 28px taskbar bounds and upward power gap OK');
        console.log('graphical WM: readable launcher selection, close hover and dock stacking OK');
        console.log('graphical WM: pinned-only bar, picker minimise/restore and 20 safe window-reuse cycles OK');
        console.log('graphical WM: no real client resize during animation or move OK');
        console.log('graphical terminal: pin/picker icons, themed cursor inheritance and buffered resize OK');
        console.log('graphical Settings: mouse-open persistence and live 28/32px taskbar scaling OK');
        console.log('graphical drawing: all monitored WM/terminal painting used backing pixmaps');
    } catch (err) {
        console.error('WM log on failure:', wmLog);
        console.error('Terminal log on failure:', terminalLog);
        throw err;
    } finally {
        if (terminal && terminal.exitCode === null) terminal.kill('SIGTERM');
        if (wm && wm.exitCode === null) wm.kill('SIGTERM');
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
