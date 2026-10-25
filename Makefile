# scoswm — SCos-styled X11 window manager for real Linux applications.
# Only hard dependencies: libX11, zlib and the POSIX PTY interface.
# No compositor, no toolkit, no terminal emulation library.

CC       ?= cc
CFLAGS   ?= -O2 -g
CPPFLAGS += -DVERSION='"0.5.3"'
LDLIBS    = -lX11 -lz -ldl
TERM_LDLIBS = -lX11 -lz -lutil -ldl
PREFIX   ?= /usr
DESTDIR  ?=

SRC = src/scoswm.c src/util.c
TERM_SRC = src/scos-terminal.c src/term_emulation.c src/util.c

all: scoswm scos-terminal

scoswm: $(SRC) src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

scos-terminal: $(TERM_SRC) src/term_emulation.h src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ $(TERM_SRC) $(LDFLAGS) $(TERM_LDLIBS)

# Pure tests and the real-Bash PTY test run without an X server.
# If Xvfb is installed, the WM integration test also boots the WM and a
# real X client. tests/run_graphical.sh additionally uses node-x11 when
# SCOSWM_NODE_X11 (or an installed x11 module with lib/xserver) is present.
check: scoswm scos-terminal test_util test_terminal test_pty
	./test_util
	./test_terminal
	./test_pty
	./tests/check_sddm.sh
	./tests/run_integration.sh ./scoswm
	./tests/run_graphical.sh

test_util: tests/test_util.c src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_util.c src/util.c $(LDFLAGS) -lz -ldl

test_terminal: tests/test_terminal.c src/term_emulation.c src/term_emulation.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_terminal.c src/term_emulation.c $(LDFLAGS)

test_pty: tests/test_pty.c src/term_emulation.c src/term_emulation.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_pty.c src/term_emulation.c $(LDFLAGS) -lutil

install: scoswm scos-terminal
	install -Dm755 scoswm $(DESTDIR)$(PREFIX)/bin/scoswm
	install -Dm755 scos-terminal $(DESTDIR)$(PREFIX)/bin/scos-terminal
	install -Dm755 packaging/scoswm-session $(DESTDIR)$(PREFIX)/bin/scoswm-session
	install -Dm644 packaging/scoswm.desktop $(DESTDIR)$(PREFIX)/share/xsessions/scoswm.desktop
	install -Dm644 packaging/scos-terminal.desktop $(DESTDIR)$(PREFIX)/share/applications/scos-terminal.desktop
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
	rm -f scoswm scos-terminal test_util test_terminal test_pty

.PHONY: all check install clean
