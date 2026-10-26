# scoswm — SCos-styled X11 window manager for real Linux applications.
# Only hard dependencies: libX11, libXext (SHAPE input regions), zlib and
# the POSIX PTY interface. No compositor, toolkit or terminal library.

CC       ?= cc
CFLAGS   ?= -O2 -g
CPPFLAGS += -DVERSION='"0.5.7"'
LDLIBS    = -lX11 -lz -ldl
WM_LDLIBS = -lX11 -lXext -lz -ldl
TERM_LDLIBS = -lX11 -lz -lutil -ldl
PREFIX   ?= /usr
DESTDIR  ?=

SRC = src/scoswm.c src/util.c src/wifi.c
TERM_SRC = src/scos-terminal.c src/term_emulation.c src/util.c
FILES_SRC = src/scos-files.c src/files_core.c src/util.c

all: scoswm scos-terminal scos-files

scoswm: $(SRC) src/util.h src/wifi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ $(SRC) $(LDFLAGS) $(WM_LDLIBS)

scos-terminal: $(TERM_SRC) src/term_emulation.h src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ $(TERM_SRC) $(LDFLAGS) $(TERM_LDLIBS)

scos-files: $(FILES_SRC) src/files_core.h src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ $(FILES_SRC) $(LDFLAGS) $(LDLIBS)

# Pure tests and the real-Bash PTY test run without an X server.
# If Xvfb is installed, the WM integration test also boots the WM and a
# real X client. tests/run_graphical.sh additionally uses node-x11 when
# SCOSWM_NODE_X11 (or an installed x11 module with lib/xserver) is present.
check: scoswm scos-terminal scos-files test_util test_terminal test_pty test_files test_wifi test_file_client
	./test_util
	./test_terminal
	./test_pty
	./test_files
	./test_wifi
	./tests/check_sddm.sh
	./tests/run_integration.sh ./scoswm
	./tests/run_graphical.sh

test_util: tests/test_util.c src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_util.c src/util.c $(LDFLAGS) -lz -ldl

test_terminal: tests/test_terminal.c src/term_emulation.c src/term_emulation.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_terminal.c src/term_emulation.c $(LDFLAGS)

test_pty: tests/test_pty.c src/term_emulation.c src/term_emulation.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_pty.c src/term_emulation.c $(LDFLAGS) -lutil

test_files: tests/test_files.c src/files_core.c src/files_core.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_files.c src/files_core.c $(LDFLAGS)

test_wifi: tests/test_wifi.c src/wifi.c src/wifi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_wifi.c src/wifi.c $(LDFLAGS)

test_file_client: tests/file_manager_client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -o $@ tests/file_manager_client.c $(LDFLAGS) -lX11

install: scoswm scos-terminal scos-files
	install -Dm755 scoswm $(DESTDIR)$(PREFIX)/bin/scoswm
	install -Dm755 scos-terminal $(DESTDIR)$(PREFIX)/bin/scos-terminal
	install -Dm755 scos-files $(DESTDIR)$(PREFIX)/bin/scos-files
	install -Dm755 packaging/scoswm-session $(DESTDIR)$(PREFIX)/bin/scoswm-session
	install -Dm644 packaging/scoswm.desktop $(DESTDIR)$(PREFIX)/share/xsessions/scoswm.desktop
	install -Dm644 packaging/scos-terminal.desktop $(DESTDIR)$(PREFIX)/share/applications/scos-terminal.desktop
	install -Dm644 packaging/scos-files.desktop $(DESTDIR)$(PREFIX)/share/applications/scos-files.desktop
	for size in 16x16 22x22 24x24 32x32 48x48 64x64 128x128 256x256; do \
		for name in scos-terminal scos-files scos-sysmon scos-settings; do \
			install -Dm644 packaging/icons/hicolor/$$size/apps/$$name.png \
				$(DESTDIR)$(PREFIX)/share/icons/hicolor/$$size/apps/$$name.png; \
		done; \
	done
	install -d $(DESTDIR)$(PREFIX)/share/icons/SCos/cursors
	cp -a packaging/icons/SCos/cursors/. $(DESTDIR)$(PREFIX)/share/icons/SCos/cursors/
	install -m644 packaging/icons/SCos/index.theme packaging/icons/SCos/cursor.theme \
		$(DESTDIR)$(PREFIX)/share/icons/SCos/
	install -Dm644 packaging/scoswm.conf.example $(DESTDIR)$(PREFIX)/share/doc/scoswm/scoswm.conf.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/scoswm/README.md

clean:
	rm -f scoswm scos-terminal scos-files test_util test_terminal test_pty test_files test_wifi test_file_client

.PHONY: all check install clean
