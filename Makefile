# Linux and other POSIX hosts.  Windows is built by build.bat.
#
# Produces pfemu-headless: the emulation core plus the session driver
# (src/run.c) on the null host (src/host_null.c).  No window, no audio device,
# no launcher - a run is whatever -replay, -keys, -untilemu and -secs say, and
# it reports through stderr, -wav, -shot and -shotevery.
#
# `make gui` produces pfemu, the playable Linux build: the same objects on the
# SDL2 host (src/host_sdl.c) - a window, the keyboard, sound.  It needs the
# SDL2 development files (Debian/Ubuntu: libsdl2-dev), found through
# pkg-config or sdl2-config; set SDL_CFLAGS and SDL_LIBS to use others.
#
# The flags are not preference.  docs/VERIFY.md's determinism section names
# the first two as the things most likely to make this build disagree with the
# Windows one:
#
#   -ffp-contract=off   emu_time, the PIT phase math and the VGA phase math
#                       are doubles.  IEEE + - * / is exactly specified, but
#                       gcc and clang fuse a*b+c into an FMA by default, which
#                       changes the result.  MSVC is not doing that (/fp:fast
#                       is deliberately off there, because the sound driver's
#                       PLL lock needs exact comparisons), so fusing here
#                       would be a divergence this build invented.
#   -fno-strict-aliasing
#                       the codebase type-puns over guest RAM constantly.
#                       MSVC is effectively no-strict-aliasing; gcc at -O2 is
#                       not.
#
# and no -ffast-math anywhere, for the same reason as /fp:fast.
#
# Targets:
#   make              the headless binary
#   make gui          pfemu, the same emulator in an SDL2 window (see above)
#   make ubsan        pfemu-headless-ubsan, instrumented.  Drive it with
#                     tests/golden/ubsan.sh, which keeps its logs.  The
#                     first pass found 32 unaligned guest-RAM accesses in
#                     dos.c and bios.c, every one a wider pointer punned at
#                     &ram[a], and none in cpu.c - whose hot path assembles
#                     bytes by hand and was already clean.  Fixed via ld16u/
#                     st16u in pfemu.h.  Two vectors now report nothing at
#                     all, across two different table programs and one
#                     complete game, so the shift counts >= width this file
#                     used to predict are not in the code these games
#                     execute.  Absence of a report is still only that:
#                     ubsan instruments what runs.
#   make fuzz         pfemu-fuzz-pfr, the .pfr parser under ASan+UBSan with a
#                     standalone mutation driver (tests/fuzz/README.md).
#                     docs/VERIFY.md gates the verification service on this:
#                     the parser is C eating attacker-controlled text and is
#                     a larger attack surface than the emulator behind it.
#   make fuzz-clang   the same target built as a libFuzzer harness, when a
#                     clang is available.  Coverage guided, so it reaches
#                     much further; the gcc driver exists because this
#                     repository does not otherwise need clang.
#   make clean

CC      ?= cc
CSTD    ?= -std=c99
WARN    ?= -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
DETERM  := -ffp-contract=off -fno-strict-aliasing -fno-fast-math
CFLAGS  ?= -O2
CFLAGS  += $(CSTD) $(WARN) $(DETERM) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  += -MMD -MP        # header dependencies, so editing pfemu.h rebuilds
LDLIBS  += -lm

# The emulation core is platform-free; run.c is the session driver; posix.c
# answers the few Win32 calls the rest makes.  launch.c (the Win32 picker)
# and main.c (the Win32 host) are excluded, which is the whole point of the
# split.  What is left is the host: host_null.c here, host_sdl.c for `make
# gui`.  Everything else is the same objects in both.
COMMON := src/cpu.c src/vga.c src/dev.c src/bios.c src/dos.c src/sound.c \
       src/cfg.c src/fantasies.c src/release.c src/lzexe.c src/png.c \
       src/replay.c src/snapshot.c src/verify.c src/run.c src/vgafont.c \
       src/cdimage.c src/gog.c src/posix.c
SRC := $(COMMON) src/host_null.c

# The build identity the -verify object carries (src/verify.c).  The
# header is regenerated on every make but rewritten only when the ID
# changes, so a new commit recompiles verify.o and nothing else - and a
# stale ID cannot survive a rebuild, which a -D on the command line with
# no dependency behind it would allow.  -dirty marks a build from a tree
# with uncommitted changes, which a verifier should not be running.
BUILD_ID := $(shell git describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)

OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)
BIN := pfemu-headless

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

# The game window.  Only host_sdl.o sees the SDL headers; the other objects
# are the headless build's, compiled with exactly its flags, so the two
# binaries run the same emulator.  ?= defines them deferred, so a plain
# `make` never asks for SDL at all.
GUIBIN     := pfemu
GUIOBJ     := $(COMMON:.c=.o) src/host_sdl.o
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
SDL_LIBS   ?= $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)

gui: $(GUIBIN)

$(GUIBIN): $(GUIOBJ)
	$(CC) $(CFLAGS) -o $@ $(GUIOBJ) $(LDLIBS) $(SDL_LIBS)

src/host_sdl.o: CFLAGS += $(SDL_CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/build.h: FORCE
	@printf '#define PFEMU_BUILD "%s"\n' '$(BUILD_ID)' > $@.tmp
	@if cmp -s $@ $@.tmp; then rm -f $@.tmp; else mv $@.tmp $@; fi

src/verify.o: src/build.h

# Undefined-behaviour run.  -O1 keeps the traces readable; this binary is for
# finding bugs, not for timing anything.
#
# It links to its own name and deletes its objects afterwards, because the two
# configurations otherwise share $(OBJ) with nothing to tell them apart: an
# instrumented build followed by a plain one relinks sanitizer-laced objects
# without -fsanitize=undefined, and the link fails on a wall of undefined
# __ubsan_handle_* references that looks like a source problem and is not.
# tests/golden/run.sh takes a binary as its first argument, so either build can
# be handed to it.
UBBIN := $(BIN)-ubsan

ubsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g $(CSTD) $(WARN) $(DETERM) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -fsanitize=undefined -fno-omit-frame-pointer" \
	        LDLIBS="-lm -fsanitize=undefined" BIN=$(UBBIN)
	rm -f $(OBJ) $(DEP)
	@echo "built $(UBBIN); objects removed so a later 'make' recompiles clean"

# ---------------------------------------------------------------- fuzzing
# The parser is linked against stubs, not against the emulator, so a case is
# a parse and nothing else - thousands a second, and exactly the surface a
# verifier exposes before it decides to simulate anything.
#
# Built straight from sources with no intermediate objects on purpose: the
# sanitizer flags here must not leave instrumented .o files in src/ for a
# later plain `make` to relink, which is the stale-object trap the ubsan
# target above had to be restructured to avoid.
#
# -fno-sanitize-recover=all makes a UBSan finding exit non-zero instead of
# printing and continuing, so a CI job can believe the status.
FUZZSRC := tests/fuzz/fuzz_pfr.c src/replay.c src/posix.c
FUZZBIN := pfemu-fuzz-pfr
FUZZFLAGS := -O1 -g $(CSTD) $(WARN) $(DETERM) \
             -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
             -fsanitize=address,undefined -fno-sanitize-recover=all \
             -fno-omit-frame-pointer

fuzz:
	$(CC) $(FUZZFLAGS) -o $(FUZZBIN) $(FUZZSRC) -lm
	@echo "built $(FUZZBIN); see tests/fuzz/README.md"

# The -verify verdict (src/verify.c), against stubs rather than the emulator.
# The JSON object is an interface a service parses, so it gets a regression
# test for the same reason the .pfr parser does.  Sanitized like the fuzz
# harness, and for the same reason: it is cheap here and the object is built
# by hand with fprintf.  Needs no installation, so unlike tests/golden it can
# run on a stock CI runner.
VERSRC := tests/verify/verify_selftest.c src/verify.c
VERBIN := pfemu-verify-test

verify-test: src/build.h
	$(CC) $(FUZZFLAGS) -o $(VERBIN) $(VERSRC) -lm
	@echo "built $(VERBIN); run it with ./$(VERBIN)"

# LLVMFuzzerTestOneInput instead of main().  CC=clang is not a default
# because the rest of this Makefile is deliberately gcc-shaped.
fuzz-clang:
	clang $(FUZZFLAGS) -DFUZZ_LIBFUZZER -fsanitize=fuzzer \
	      -o $(FUZZBIN)-libfuzzer $(FUZZSRC) -lm
	@echo "built $(FUZZBIN)-libfuzzer; run it with tests/fuzz/corpus/"

clean:
	rm -f $(OBJ) $(DEP) $(BIN) $(BIN)-ubsan $(FUZZBIN) $(FUZZBIN)-libfuzzer $(VERBIN) src/build.h
	rm -f src/host_sdl.o src/host_sdl.d $(GUIBIN)

-include $(DEP) src/host_sdl.d

FORCE:

.PHONY: all gui clean ubsan fuzz fuzz-clang verify-test FORCE
