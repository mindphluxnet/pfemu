# Headless build (Linux and other POSIX hosts).  Windows is built by build.bat.
#
# Produces pfemu-headless: the emulation core plus the session driver
# (src/run.c) on the null host (src/host_null.c).  No window, no audio device,
# no launcher - a run is whatever -replay, -keys, -untilemu and -secs say, and
# it reports through stderr, -wav, -shot and -shotevery.
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
#   make ubsan        the same, instrumented - surfaces the latent UB in
#                     cpu.c (shift counts >= width, signed overflow) that the
#                     determinism section expects to find.  Fixing what this
#                     reports improves the Windows build too.
#   make clean

CC      ?= cc
CSTD    ?= -std=c99
WARN    ?= -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
DETERM  := -ffp-contract=off -fno-strict-aliasing -fno-fast-math
CFLAGS  ?= -O2
CFLAGS  += $(CSTD) $(WARN) $(DETERM) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  += -MMD -MP        # header dependencies, so editing pfemu.h rebuilds
LDLIBS  += -lm

# The emulation core is platform-free; run.c is the session driver; the last
# two are this host.  launch.c (the Win32 picker) and main.c (the Win32 host)
# are excluded, which is the whole point of the split.
SRC := src/cpu.c src/vga.c src/dev.c src/bios.c src/dos.c src/sound.c \
       src/cfg.c src/fantasies.c src/release.c src/lzexe.c src/png.c \
       src/replay.c src/snapshot.c src/run.c src/vgafont.c \
       src/host_null.c src/posix.c

OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)
BIN := pfemu-headless

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Undefined-behaviour run.  -O1 keeps the traces readable; this binary is for
# finding bugs, not for timing anything.
ubsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g $(CSTD) $(WARN) $(DETERM) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -fsanitize=undefined -fno-omit-frame-pointer" \
	        LDLIBS="-lm -fsanitize=undefined" BIN=$(BIN)

clean:
	rm -f $(OBJ) $(DEP) $(BIN)

-include $(DEP)

.PHONY: all clean ubsan
