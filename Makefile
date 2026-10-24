# scoswm — SCos-styled X11 window manager for real Linux applications.
# Only hard dependency: libX11 (present on every X11 system; NVIDIA
# proprietary and nouveau installs included). No compositor, no toolkit.

CC      ?= cc
CFLAGS  ?= -O2 -g
CPPFLAGS += -DVERSION='"0.4.0"'
LDLIBS   = -lX11 -lz
PREFIX  ?= /usr
DESTDIR ?=

SRC = src/scoswm.c src/util.c

all: scoswm

scoswm: $(SRC) src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -o $@ $(SRC) $(LDLIBS)

# Runs anywhere (no X server needed): themes/config/.desktop/exec logic.
# If Xvfb is installed it also boots the WM and pastes a real X client
# onto the screen to be screenshotted.
check: scoswm test_util
	./test_util
	./tests/run_integration.sh ./scoswm

test_util: tests/test_util.c src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Isrc -o $@ tests/test_util.c src/util.c -lz

install: scoswm
	install -Dm755 scoswm $(DESTDIR)$(PREFIX)/bin/scoswm
	install -Dm755 packaging/scoswm-session $(DESTDIR)$(PREFIX)/bin/scoswm-session
	install -Dm644 packaging/scoswm.desktop $(DESTDIR)$(PREFIX)/share/xsessions/scoswm.desktop
	install -Dm644 packaging/scoswm.conf.example $(DESTDIR)$(PREFIX)/share/doc/scoswm/scoswm.conf.example
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/scoswm/README.md

clean:
	rm -f scoswm test_util

.PHONY: all check install clean
