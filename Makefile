# VMC10 for Linux

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -D_GNU_SOURCE -Wno-format-truncation
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
PREFIX  ?= /usr/local

CORE = src/m6803.c src/mc10.c src/mcx128.c src/mcx32sd.c src/sdcard.c src/serial.c src/tape.c
SRC  = $(CORE) src/debugger.c src/main.c
HDR  = $(wildcard src/*.h)

all: vmc10

vmc10: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SRC) $(SDL_LIBS) -lm

build/headless: tests/headless.c $(CORE) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/headless.c $(CORE) -lm

test: build/headless
	@sh tests/run_tests.sh

install: vmc10
	install -Dm755 vmc10 $(DESTDIR)$(PREFIX)/bin/vmc10
	install -d $(DESTDIR)$(PREFIX)/share/vmc10/roms
	install -m644 roms/README.md $(DESTDIR)$(PREFIX)/share/vmc10/roms/README.md
	@echo "Copy your ROM images (see roms/README.md) to $(PREFIX)/share/vmc10/roms/"

clean:
	rm -rf vmc10 build

.PHONY: all test install clean
