# SCos window manager — Linux port.
#
#   make            build ./scoswm (SDL2) and ./scoswm-headless
#   make headless   build only ./scoswm-headless (no SDL2 needed)
#   make check      build the headless binary and run the smoke tests
#   make install    install to $(PREFIX) (default /usr/local)
#
# The only build dependency of the windowed binary is SDL2 (+ a C
# compiler and GNU make).  On Arch:  sudo pacman -S sdl2 pkgconf
# See README.md for other distributions.

CC       ?= cc
PREFIX   ?= /usr/local
DESTDIR  ?=
BUILD    := build

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null || echo -lSDL2)

CFLAGS   ?= -O2
CPPFLAGS += -Isrc -I$(BUILD) -D_GNU_SOURCE
ALL_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter $(CFLAGS)

# Ported SCos desktop/app sources (src/), in registry order.
CORE_SRC := \
    src/wm.c src/fb.c src/theme.c src/appearance.c src/lib.c \
    src/textwrap.c src/cards.c src/heap.c src/vfs.c src/apps.c \
    src/cat.c src/klog.c src/err.c src/confirm.c src/buildinfo.c \
    src/boot_screen.c src/neofetch_art.c src/panic_art.c src/font.c \
    src/app_files.c src/app_terminal.c src/app_notepad.c src/app_browser.c \
    src/app_calendar.c src/app_settings.c src/app_about.c \
    src/app_blackjack.c src/app_sysmon.c src/app_solitaire.c \
    src/platform_linux.c src/main.c

CORE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC))
LDSCRIPT := src/scos_apps.ld

.PHONY: all headless check clean install
all: scoswm scoswm-headless

headless: scoswm-headless

scoswm: $(CORE_OBJ) $(BUILD)/backend_sdl.o
	$(CC) $(ALL_CFLAGS) $^ -o $@ $(SDL_LIBS) -Wl,-T,$(LDSCRIPT) -lm

scoswm-headless: $(CORE_OBJ) $(BUILD)/backend_headless.o
	$(CC) $(ALL_CFLAGS) $^ -o $@ -Wl,-T,$(LDSCRIPT) -lm

$(BUILD):
	mkdir -p $@

# Build stamp, generated exactly like the bare-metal build did
# (tools/buildinfo.py there): commit/branch/tree state of this repo.
$(BUILD)/scosbuild.h: | $(BUILD)
	@{ \
	  commit=$$(git rev-parse --short HEAD 2>/dev/null || echo unknown); \
	  branch=$$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown); \
	  if [ -n "$$(git status --porcelain 2>/dev/null)" ]; then tree=modified; else tree=clean; fi; \
	  date=$$(date -u +%Y-%m-%dT%H:%M:%SZ); \
	  printf '#define SCOS_BUILD_COMMIT "%s"\n#define SCOS_BUILD_BRANCH "%s"\n#define SCOS_BUILD_TREE "%s"\n#define SCOS_BUILD_DATE "%s"\n' \
	    "$$commit" "$$branch" "$$tree" "$$date"; \
	} > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm $@.tmp; fi

$(BUILD)/%.o: src/%.c src/scos.h src/platform.h $(BUILD)/scosbuild.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/backend_sdl.o: src/backend_sdl.c src/scos.h src/platform.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(SDL_CFLAGS) -c $< -o $@

check: scoswm-headless
	python3 tests/test_port.py ./scoswm-headless

install: scoswm
	install -Dm755 scoswm $(DESTDIR)$(PREFIX)/bin/scoswm
	install -Dm644 packaging/scoswm.desktop $(DESTDIR)$(PREFIX)/share/applications/scoswm.desktop
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/scoswm/README.md
	install -Dm644 PORTING.md $(DESTDIR)$(PREFIX)/share/doc/scoswm/PORTING.md

clean:
	rm -rf $(BUILD) scoswm scoswm-headless
