#!/usr/bin/env python3
"""Static byte-scan for the -scoredbg locators (Spike A, docs/VERIFY.md).

src/fantasies.c locates the score, the attract-mode boundaries and the
ball/player site by masked opcode signature in the loaded image.  A signature
is only worth trusting if it matches exactly where it is supposed to and
nowhere else, and that is a question about the shipped files, not about a
running emulator - so it is answered here, offline, over every TABLE1-4.PRG of
every release, before the emulator is asked to rely on it.

    python tools/scorescan.py FANTASY FANTASYA FANTASYDX

Each row prints the addresses a signature yields and the number of sites that
produced them.  A row is OK when every signature matched the expected number of
times and every address that two signatures both name agrees.  This is the
evidence behind "confirmed unique by static byte-scan" in docs/VERIFY.md;
re-run it whenever a new release joins the database.

The signatures here must stay identical to the ones in src/fantasies.c.  They
are written as regular expressions over the load image rather than as
(sig, mask) pairs because that is what reads well in Python; the bytes are the
same bytes.
"""
import os
import re
import struct
import sys

# --- the same shapes as fantasies_find_score(), see its comment for the map ---

# PUSH ES / PUSH DS / POP ES / MOV CX,6 / MOV AX,0 / MOV DI,SIFFRORNA /
# REP STOSW / POP ES / RETN                              (PLAND.ASM zeroscore)
ZEROSCORE = re.compile(rb'\x06\x1e\x07\xb9\x06\x00\xb8\x00\x00\xbf(..)\xf3\xab\x07\xc3', re.S)
# MOV SI,SIFFRORNA / MOV BX,0C8h / PUSH CS / POP ES / CALL DWORD PTR ES:[PEKOR]
#                                                    (FANTASIE.ASM ONLY_SCORE)
DMDPRINT = re.compile(rb'\xbe(..)\xbb\xc8\x00\x0e\x07\x26\xff\x1e..', re.S)
# MOV CS:[ALREADY_QUITTING],FALSE / MOV CS:[KEYBOARD_ENABLED],TRUE /
# MOV [DEMOMODE],TRUE / RETN                        (FANTASIE.ASM GO_DEMO_MODE)
GO_DEMO = re.compile(rb'\x2e\xc6\x06..\x00\x2e\xc6\x06..\xff\xc6\x06(..)\xff\xc3', re.S)
# MOV AL,[PLAYER] / CMP AL,[PLAYERS] / JE / NOP*3 / INC [PLAYER] / JMP / NOP /
# INC [BALLS+11] / MOV AL,[NO_OF_BALLS] / CMP [BALLS+11],AL / JA
#                                                    (PLAND.ASM _CHANGE_PLAYER)
BALLSITE = re.compile(rb'\xa0(..)\x3a\x06(..)\x74.\x90\x90\x90\xfe\x06(..)\xeb.\x90'
                      rb'\xfe\x06(..)\xa0(..)\x38\x06(..)\x77.', re.S)
# SUB AL,3Ah / MOV [PLAYERS],AL / ADD AL,37h / MOV [text],AL  (F1-F8 handler)
ADDPLAYER = re.compile(rb'\x2c\x3a\xa2(..)\x04\x37\xa2.', re.S)
# SPRINGUP's tail: the plunger speed, dithered with the free-running counter,
# stored into the ball's vertical velocity.  That store is the launch.  Its
# Y_HAST must be the word the ball-jump locator in src/fantasies.c finds from
# the motion integrator - INTEGRATOR below, which shares nothing with it.
#   IMUL CX / MOV BP,AX / MOV AX,[slump] / AND AX,255 / SUB BP,AX /
#   MOV BX,offset slump / ADD BX,2 / CMP BYTE PTR [BX],0FFh / JE +13 /
#   NOP*3 / MOV [Y_HAST],BP
LAUNCH = re.compile(rb'\xf7\xe9\x8b\xe8\xa1(..)\x25\xff\x00\x2b\xe8\xbb(..)'
                    rb'\x83\xc3\x02\x80\x3f\xff\x74\x0d\x90\x90\x90\x89\x2e(..)', re.S)
# MOV AX,[vel] / CWD / ADD [acc_lo],AX / ADC [acc_hi],DX / ... / IDIV BX
INTEGRATOR = re.compile(rb'\xa1(..)\x99\x01\x06..\x11\x16..\xa1..\x8b\x16..'
                        rb'\xbb\x00\x04\xf7\xfb', re.S)
# PUSH imm16 / POP DS - the DATA-segment majority vote
DATASEG = re.compile(rb'\x68(..)\x1f', re.S)


def load_image(path):
    """The bytes the loader puts in RAM: an MZ file past its header."""
    d = open(path, 'rb').read()
    if d[:2] in (b'MZ', b'ZM'):
        return d[struct.unpack_from('<H', d, 8)[0] * 16:]
    return d                                    # already a flat image


def u16(b):
    return b[0] | (b[1] << 8)


def agree(img, rx, group=1):
    """(address, sites) when every match names the same one, else (None, sites)."""
    seen, n = set(), 0
    for m in rx.finditer(img):
        seen.add(u16(m.group(group)))
        n += 1
    if len(seen) == 1:
        return seen.pop(), n
    return None, n


def data_seg(img):
    counts = {}
    for m in DATASEG.finditer(img):
        v = u16(m.group(1))
        counts[v] = counts.get(v, 0) + 1
    if not counts:
        return 0, 0
    seg, best = max(counts.items(), key=lambda kv: kv[1])
    return (seg if best >= 8 else 0), best


def scan(path):
    img = load_image(path)
    bad = []

    seg, votes = data_seg(img)
    if not seg:
        bad.append("DATA segment vote too thin (%d)" % votes)

    score, n_zero = agree(img, ZEROSCORE)
    if n_zero != 1:
        bad.append("score clear matched %d times, want 1" % n_zero)
    dmd, n_dmd = agree(img, DMDPRINT)
    if n_dmd < 1:
        bad.append("no dot-matrix score print")
    elif dmd is None:
        bad.append("dot-matrix prints disagree")
    elif dmd != score:
        bad.append("dot-matrix DS:%04X != score clear DS:%04X" % (dmd, score))

    demo, n_demo = agree(img, GO_DEMO)
    if n_demo != 1:
        bad.append("attract entry matched %d times, want 1" % n_demo)

    n_ggm = 0
    if demo is not None:
        pat = b'\xc6\x06' + struct.pack('<H', demo) + b'\x00\xc3'
        n_ggm = len(re.findall(re.escape(pat), img))
        if n_ggm != 1:
            bad.append("game start matched %d times, want 1" % n_ggm)

    ball = list(BALLSITE.finditer(img))
    player = players = balls11 = nballs = None
    if len(ball) != 1:
        bad.append("ball/player site matched %d times, want 1" % len(ball))
    else:
        g = [u16(x) for x in ball[0].groups()]
        player, players, player2, balls11, nballs, balls11b = g
        if player != player2 or balls11 != balls11b:
            bad.append("ball/player site disagrees with itself")
        if nballs != balls11 + 1:
            bad.append("NO_OF_BALLS %04X is not BALLS+12 (%04X)" % (nballs, balls11 + 1))

    launch = list(LAUNCH.finditer(img))
    yhast = None
    if len(launch) != 1:
        bad.append("plunger launch matched %d times, want 1" % len(launch))
    else:
        slump, slump2, yhast = [u16(x) for x in launch[0].groups()]
        if slump != slump2:
            bad.append("plunger launch disagrees with itself")
        vel = INTEGRATOR.search(img)
        if vel is None:
            bad.append("no motion integrator to confirm Y_HAST against")
        elif u16(vel.group(1)) != yhast:
            bad.append("plunger launch writes DS:%04X but the integrator reads "
                       "DS:%04X" % (yhast, u16(vel.group(1))))

    players2, n_add = agree(img, ADDPLAYER)
    if n_add < 1:
        bad.append("no add-player handler")
    elif players2 is None:
        bad.append("add-player handlers disagree")
    elif players is not None and players2 != players:
        bad.append("add-player DS:%04X != ball site DS:%04X" % (players2, players))

    def h(v):
        return "%04X" % v if v is not None else "  ??"

    print("%-28s dseg=%s(x%-2d) score=%s(x%d,dmd x%d) demo=%s(x%d,start x%d) "
          "ball=%s nballs=%s players=%s(x%d) player=%s launch=%s(x%d)  %s"
          % (path, h(seg), votes, h(score), n_zero, n_dmd, h(demo), n_demo,
             n_ggm, h(balls11), h(nballs), h(players), n_add, h(player),
             h(yhast), len(launch),
             "OK" if not bad else "FAIL: " + "; ".join(bad)))
    return not bad


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ok = True
    for arg in argv[1:]:
        if os.path.isdir(arg):
            names = sorted(f for f in os.listdir(arg) if f.upper().endswith('.PRG'))
            files = [os.path.join(arg, f) for f in names]
            if not files:
                print("%s: no .PRG files" % arg)
                ok = False
        else:
            files = [arg]
        for f in files:
            # The intro is not a table and carries none of this.
            if os.path.basename(f).upper().startswith('INTRO'):
                continue
            ok &= scan(f)
    print("\n%s" % ("all scanned programs OK" if ok else
                    "at least one program FAILED - see the rows above"))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
