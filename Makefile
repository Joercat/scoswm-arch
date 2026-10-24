# scoswm — SCos-styled X11 window manager for real Linux applications.
# Only hard dependencies: libX11, zlib and the POSIX PTY interface.
# No compositor, no toolkit, no terminal emulation library.

CC       ?= cc
CFLAGS   ?= -O2 -g
CPPFLAGS += -DVERSION='"0.5.0"'
LDLIBS    = -lX11 -lz -ldl
TERM_LDLIBS = -lX11 -lz -lutil
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
# If Xvfb is installed, the WM integration test also boots the WM and
# pastes a real X client onto the screen to be screenshotted.
check: scoswm scos-terminal test_util test_terminal test_pty
	./test_util
	./test_terminal
	./test_pty
	./tests/check_sddm.sh
	./tests/run_integration.sh ./scoswm

test_util: tests/test_util.c src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_util.c src/util.c $(LDFLAGS) -lz

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
	install -Dm644 packaging/scoswm.conf.example $(DESTDIR)$(PREFIX)/share/doc/scoswm/scoswm.conf.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/scoswm/README.md

clean:
	rm -f scoswm scos-terminal test_util test_terminal test_pty

.PHONY: all check install clean
