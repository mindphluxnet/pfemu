/* Fuzz the .pfr parser (docs/VERIFY.md: "Fuzz the .pfr parser before writing
 * a line of server code.  It is C eating attacker-controlled text, and it -
 * not the emulator - is the real attack surface.")
 *
 * This links src/replay.c against stubs rather than against the emulator, so
 * a case is a parse and nothing else: no guest, no timing, thousands of
 * iterations a second.  That is the whole surface a verifier exposes before
 * it decides to simulate anything.
 *
 * It checks two different things, and the second is the interesting one:
 *
 *   1. Memory safety.  Built with -fsanitize=address,undefined; a crash is
 *      the finding.
 *
 *   2. That an ACCEPTED file cannot hang the run loop.  Parsing safely is
 *      not enough - the bug that started this parsed perfectly and then ran
 *      forever, because replay_should_stop() returns 0 while ev_idx < nev
 *      and an event stamped past the footer never comes due.  Every accepted
 *      file is asserted against the invariants validate_events() is supposed
 *      to guarantee.  A sanitizer cannot see that property; an assertion can.
 *
 * THE INTEGRITY LINE MUST BE REPAIRED AFTER MUTATING, or this tests almost
 * nothing.  A .pfr carries an FNV-1a over its own bytes and parse_file()
 * refuses a file whose hash does not match, so a blind mutator is rejected
 * at the door: the first run of this harness accepted 8 cases out of 20001
 * and never reached the event loop at all.  That is also the wrong threat
 * model.  FNV-1a is a checksum, not a MAC - VERIFY.md says so in as many
 * words - and the client holds no key, so an attacker edits the file and
 * recomputes the hash exactly the way fixup_hash() below does.  Repairing it
 * is what makes this fuzz the parser rather than the checksum.
 *
 * Three entry points:
 *
 *   -selftest    fixed hostile cases, each with an expected verdict.  This
 *                is the regression test for the validation pass in
 *                src/replay.c; it needs no corpus and runs instantly.
 *   standalone   main(), for gcc: mutates seed files with a fixed PRNG.
 *                Deterministic, so a finding reproduces from its -s seed.
 *   libFuzzer    LLVMFuzzerTestOneInput, when built with clang.  Coverage
 *                guided and far better for a real campaign; the gcc driver
 *                exists because this repository does not otherwise need
 *                clang.
 *
 * Build: make fuzz     (see tests/fuzz/README.md)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "../../src/compat.h"
#include "../../src/pfemu.h"

/* ------------------------------------------------------------- the stubs */
/* src/replay.c reaches into the emulator for state it records and restores.
 * None of it is reachable from parse_file(), which is the only thing under
 * test here, so these exist to satisfy the linker. */
CPU cpu;
double emu_time = 0.0;
double emu_ips = 6000000.0;
double emu_inv_ips = 1.0 / 6000000.0;
int dos_no_patch = 0;
const uint8_t cfg_option_defaults[6] = { 0, 0, 0, 0, 0, 0 };

double emu_now(void){ return emu_time; }
void kbd_key(int scancode, int down){ (void)scancode; (void)down; }
void dos_set_time_frozen(int on){ (void)on; }
void dos_remap_writedir(const char *dir){ (void)dir; }
int fantasies_trainer_enabled(void){ return 0; }
int read_sound_is_sb(const char *dir){ (void)dir; return 1; }
int release_runnable(const RelResult *r){ (void)r; return 1; }
const char *release_state_name(RelState s){ (void)s; return "stub"; }
void cfg_read(const char *dir, PfCfg *c){ (void)dir; memset(c, 0, sizeof(*c)); }
void wav_current_hash(char out[17], unsigned long *samples_out){
    snprintf(out, 17, "none");
    if(samples_out) *samples_out = 0;
}

/* ----------------------------------------------------------- the checker */
/* The bounds the parser promises to enforce.  Deliberately duplicated from
 * src/replay.c rather than shared through a header: if someone loosens a
 * limit there, this should start failing and make them say so out loud,
 * which is what a test is for. */
#define CHK_IPS_MIN     1000.0
#define CHK_IPS_MAX     1.0e9
#define CHK_EVENTS      1000000
#define CHK_END_EMU     86400.0
#define CHK_END_CYCLES  1000000000000ULL

static const char *tmp_path = NULL;
static unsigned long n_run = 0, n_ok = 0, n_rejected = 0;

/* Same FNV-1a as append_file_hash()/verify_file_hash() in src/replay.c. */
static uint64_t fnv1a(const void *p, size_t n){
    const uint8_t *b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for(i = 0; i < n; i++){ h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* Recompute the integrity line over the mutated bytes, exactly as the
 * recorder does: truncate at the start of the file_hash: line and append a
 * fresh one.  Returns the new length.  A case with no such line is left
 * alone - that is the "legacy file" path, which is worth fuzzing too. */
static size_t fixup_hash(uint8_t *buf, size_t len, size_t cap){
    size_t i, cut = (size_t)-1;
    int n;
    for(i = 0; i < len; i++){
        if((i == 0 || buf[i-1] == '\n') && len - i >= 10 &&
           !memcmp(buf + i, "file_hash:", 10)){ cut = i; break; }
    }
    if(cut == (size_t)-1) return len;
    if(cut + 32 >= cap) return cut;            /* no room; leave it truncated */
    n = snprintf((char*)buf + cut, cap - cut, "file_hash: %016llx\n",
                 (unsigned long long)fnv1a(buf, cut));
    return (n > 0) ? cut + (size_t)n : cut;
}

/* Write the candidate out and parse it.  parse_file() takes a path, not a
 * buffer, so every case is a real file - which means this also covers the
 * open/read/seek paths and the hash re-read, not just the lexer. */
static void one_case(const uint8_t *data, size_t n){
    ReplayHeader h;
    FILE *f;

    n_run++;
    f = fopen(tmp_path, "wb");
    if(!f) return;
    if(n) fwrite(data, 1, n, f);
    fclose(f);

    /* Header-only first: this is what the launcher's Details pane runs on a
     * file the user merely pointed at, so it must be safe on its own - and
     * it does NOT check the integrity line, so it is reachable with far less
     * effort than a full load. */
    if(replay_read_header(tmp_path, &h) == 0){
        assert(h.ips >= CHK_IPS_MIN && h.ips <= CHK_IPS_MAX);
        assert(h.quality >= 0 && h.quality <= 4);
        assert(h.start_table >= 0 && h.start_table <= 4);
        if(h.have_end){
            assert(h.end_emu >= 0.0 && h.end_emu <= CHK_END_EMU);
            assert(h.end_cycles <= CHK_END_CYCLES);
        }
    }

    /* Then the real thing, events and all. */
    if(replay_begin_replay(tmp_path) != 0){ n_rejected++; replay_abort(); return; }
    n_ok++;

    if(replay_read_header(tmp_path, &h) == 0){
        assert(h.nevents >= 0 && h.nevents <= CHK_EVENTS);
        /* THE invariant.  An accepted file must not leave an event that
         * never comes due; if it can, replay_should_stop() is unreachable
         * and the run never ends.  A file is only accepted with a footer,
         * so end_cycles is meaningful here. */
        if(h.end_cycles > 0)
            assert(replay_last_event_cycle() <= h.end_cycles);
    }
    replay_abort();
}

/* ------------------------------------------------------------- self-test */
/* The regression test for the validation pass.  Each case is a complete file
 * body; the hash is repaired before it is offered, so every one of these
 * reaches the checks it is aimed at rather than dying on the integrity line.
 *
 * `want` is what should happen: 1 accepted, 0 refused. */
typedef struct { const char *name; int want; int strict; const char *body; } Case;

#define BASE_HEAD \
    "PFEMU-REPLAY 1\n" \
    "release: deluxe\n" \
    "program: PINBALL.EXE\n" \
    "ips: 6000000.000000\n" \
    "speed: 1\n" \
    "quality: 2\n" \
    "start_table: 3\n" \
    "trainer_off: 1\n" \
    "events:\n"
/* BASE_HEAD as a ranked recording.  The overlay values are the canonical
 * state's hash for sound off (any quality) and for sound on at quality 2,
 * computed outside src/replay.c from the bytes docs/REPLAY.md lists. */
#define CANON_HEAD(extra) \
    "PFEMU-REPLAY 1\n" \
    "release: deluxe\n" \
    "program: PINBALL.EXE\n" \
    "ips: 6000000.000000\n" \
    "speed: 1\n" \
    "quality: 2\n" \
    "start_table: 3\n" \
    "trainer_off: 1\n" \
    extra \
    "events:\n"
#define CANON_OFF "61da2dea96515ac2"
#define CANON_ON2 "90a24b3867a30a13"
#define BASE_FOOT \
    "end_emu: 1.000000\n" \
    "end_cycles: 6000000\n" \
    "file_hash: 0000000000000000\n"

static const Case cases[] = {
 { "valid baseline", 1, 0,
   BASE_HEAD "1000 0.000167 1e 1\n2000 0.000333 1e 0\n" BASE_FOOT },

 /* The hang that started this.  Parses fine; would never stop. */
 { "event past the footer", 0, 0,
   BASE_HEAD "1000 0.000167 1e 1\n9223372036854775807 0.1 1e 0\n" BASE_FOOT },

 /* The other hang: NaN defeats every `<= 0.0` clamp downstream. */
 { "ips: nan", 0, 0,
   "PFEMU-REPLAY 1\nrelease: deluxe\nips: nan\nevents:\n" BASE_FOOT },
 { "ips: inf", 0, 0,
   "PFEMU-REPLAY 1\nrelease: deluxe\nips: inf\nevents:\n" BASE_FOOT },
 { "ips: 0", 0, 0,
   "PFEMU-REPLAY 1\nrelease: deluxe\nips: 0\nevents:\n" BASE_FOOT },
 { "speed: 0 (divides in run.c)", 0, 0,
   "PFEMU-REPLAY 1\nrelease: deluxe\nspeed: 0\nevents:\n" BASE_FOOT },

 /* Truncation used to turn 260 into 4 silently, in the overlay patch. */
 { "quality: 260 wraps to 4", 0, 0,
   "PFEMU-REPLAY 1\nrelease: deluxe\nquality: 260\nevents:\n" BASE_FOOT },
 { "start_table: 99", 0, 0,
   "PFEMU-REPLAY 1\nrelease: deluxe\nstart_table: 99\nevents:\n" BASE_FOOT },

 { "end_cycles: 2^64-1", 0, 0,
   BASE_HEAD "end_emu: 1.0\nend_cycles: 18446744073709551615\n"
   "file_hash: 0000000000000000\n" },
 { "end_emu: 1e308", 0, 0,
   BASE_HEAD "end_emu: 1e308\nend_cycles: 6000000\n"
   "file_hash: 0000000000000000\n" },
 { "end_emu negative", 0, 0,
   BASE_HEAD "end_emu: -1.0\nend_cycles: 6000000\n"
   "file_hash: 0000000000000000\n" },

 { "events out of order", 0, 0,
   BASE_HEAD "5000 0.000833 1e 1\n1000 0.000167 1e 0\n" BASE_FOOT },
 { "mixed cycle and legacy events", 0, 0,
   BASE_HEAD "1000 0.000167 1e 1\n0.000333 1e 0\n" BASE_FOOT },

 /* Pre-existing refusals, here so a future edit cannot quietly drop them. */
 { "bad magic", 0, 0, "NOT-A-PFR 1\nrelease: deluxe\nevents:\n" BASE_FOOT },
 { "no release", 0, 0, "PFEMU-REPLAY 1\nevents:\n" BASE_FOOT },
 { "no footer", 0, 0, BASE_HEAD "1000 0.000167 1e 1\n" },
 { "empty", 0, 0, "" },
 { "magic only", 0, 0, "PFEMU-REPLAY 1\n" },

 /* Legacy tolerance: allowed in play, refused by a verifier. */
 { "legacy events, default", 1, 0,
   BASE_HEAD "0.000167 1e 1\n0.000333 1e 0\n" BASE_FOOT },
 { "legacy events, -strict", 0, 1,
   BASE_HEAD "0.000167 1e 1\n0.000333 1e 0\n" BASE_FOOT },
 { "no integrity line, default", 1, 0,
   BASE_HEAD "1000 0.000167 1e 1\n" "end_emu: 1.0\nend_cycles: 6000000\n" },
 { "no integrity line, -strict", 0, 1,
   BASE_HEAD "1000 0.000167 1e 1\n" "end_emu: 1.0\nend_cycles: 6000000\n" },

 /* Canonical state.  A known state is rebuilt from sound and quality, so
  * the overlay: line has to be the one those two produce. */
 { "canonical, sound off", 1, 1,
   CANON_HEAD("state: canonical-1\noverlay: " CANON_OFF "\n")
   "1000 0.000167 1e 1\n" BASE_FOOT },
 { "canonical, sound on", 1, 1,
   CANON_HEAD("sound: 1\nstate: canonical-1\noverlay: " CANON_ON2 "\n")
   "1000 0.000167 1e 1\n" BASE_FOOT },
 { "canonical, overlay of the other sound", 0, 0,
   CANON_HEAD("sound: 1\nstate: canonical-1\noverlay: " CANON_OFF "\n")
   "1000 0.000167 1e 1\n" BASE_FOOT },
 { "canonical, no overlay line", 0, 0,
   CANON_HEAD("state: canonical-1\n") "1000 0.000167 1e 1\n" BASE_FOOT },
 { "state this build does not know", 0, 0,
   CANON_HEAD("state: canonical-2\noverlay: " CANON_OFF "\n")
   "1000 0.000167 1e 1\n" BASE_FOOT },
};
#define NCASES ((int)(sizeof(cases)/sizeof(cases[0])))

static uint8_t selfbuf[1 << 16];

static int selftest(void){
    int i, fails = 0;
    for(i = 0; i < NCASES; i++){
        size_t len = strlen(cases[i].body);
        FILE *f;
        int got;
        if(len >= sizeof(selfbuf)) continue;
        memcpy(selfbuf, cases[i].body, len);
        len = fixup_hash(selfbuf, len, sizeof(selfbuf));

        f = fopen(tmp_path, "wb");
        if(!f){ fprintf(stderr, "cannot write %s\n", tmp_path); return 2; }
        fwrite(selfbuf, 1, len, f);
        fclose(f);

        replay_set_strict(cases[i].strict);
        got = (replay_begin_replay(tmp_path) == 0) ? 1 : 0;
        /* An accepted file must also satisfy the run-loop invariant. */
        if(got && cases[i].want){
            ReplayHeader h;
            if(replay_read_header(tmp_path, &h) == 0 && h.end_cycles > 0 &&
               replay_last_event_cycle() > h.end_cycles){
                fprintf(stderr, "  %-32s ACCEPTED WITH UNREACHABLE EVENT\n",
                        cases[i].name);
                fails++;
                replay_abort();
                replay_set_strict(0);
                continue;
            }
        }
        replay_abort();
        replay_set_strict(0);

        if(got != cases[i].want){
            fprintf(stderr, "  %-32s FAIL (wanted %s, got %s)%s\n",
                    cases[i].name,
                    cases[i].want ? "accept" : "refuse",
                    got ? "accept" : "refuse",
                    cases[i].strict ? " [-strict]" : "");
            fails++;
        } else {
            fprintf(stderr, "  %-32s ok  (%s)%s\n", cases[i].name,
                    got ? "accepted" : "refused",
                    cases[i].strict ? " [-strict]" : "");
        }
    }
    remove(tmp_path);
    fprintf(stderr, "[selftest] %d cases, %d failed\n", NCASES, fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------- libFuzzer */
#ifdef FUZZ_LIBFUZZER
static uint8_t lf_buf[1 << 20];
int LLVMFuzzerInitialize(int *argc, char ***argv){
    (void)argc; (void)argv;
    tmp_path = getenv("PFR_FUZZ_TMP");
    if(!tmp_path) tmp_path = "/tmp/pfr_fuzz_case.pfr";
    return 0;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size){
    size_t len = size;
    if(!tmp_path) tmp_path = "/tmp/pfr_fuzz_case.pfr";
    if(len >= sizeof(lf_buf) - 64) len = sizeof(lf_buf) - 64;
    memcpy(lf_buf, data, len);
    /* Same reason as the standalone driver: without this, coverage stops at
     * the integrity check and the event loop is never reached. */
    len = fixup_hash(lf_buf, len, sizeof(lf_buf));
    one_case(lf_buf, len);
    return 0;
}
#else

/* ------------------------------------------------------ standalone driver */
/* xorshift64*, so a run is reproducible from its seed and a finding can be
 * handed to someone else as a number. */
static uint64_t rng_s = 88172645463325252ULL;
static uint64_t rnd(void){
    rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27;
    return rng_s * 2685821657736338717ULL;
}
static size_t rnd_below(size_t n){ return n ? (size_t)(rnd() % n) : 0; }

#define MAXCASE (1 << 20)

/* Tokens worth splicing in.  A blind byte mutator essentially never produces
 * "ips: nan" or a 20-digit cycle count, and those are exactly the inputs
 * that motivated the validation pass - so hand them to it. */
static const char *tokens[] = {
    "nan", "inf", "-inf", "NaN", "-nan", "0", "-1", "1e400", "0x7fffffff",
    "18446744073709551615", "9223372036854775807", "4294967296",
    "-9223372036854775808", "1e308", "-0.0",
    "ips: nan\n", "speed: 0\n", "quality: 260\n", "start_table: 99\n",
    "end_cycles: 18446744073709551615\n", "end_emu: 1e308\n",
    "events:\n", "file_hash: 0000000000000000\n", "PFEMU-REPLAY 1\n",
    "release: deluxe\n", "code: X 0 ", "9223372036854775807 0.0 1e 1\n",
    "0.5 1e 1\n", "\n", ":", " ", "%s", "%n", "\r\n",
};
#define NTOKENS ((int)(sizeof(tokens)/sizeof(tokens[0])))

static void mutate(uint8_t *buf, size_t *len, size_t cap){
    int op = (int)rnd_below(6);
    size_t L = *len;
    switch(op){
    case 0:                                  /* flip a bit */
        if(L) buf[rnd_below(L)] ^= (uint8_t)(1u << rnd_below(8));
        break;
    case 1:                                  /* random byte */
        if(L) buf[rnd_below(L)] = (uint8_t)rnd_below(256);
        break;
    case 2: {                                /* splice a token in */
        const char *t = tokens[rnd_below(NTOKENS)];
        size_t tl = strlen(t), at = rnd_below(L + 1);
        if(L + tl >= cap) break;
        memmove(buf + at + tl, buf + at, L - at);
        memcpy(buf + at, t, tl);
        *len = L + tl;
        break;
    }
    case 3: {                                /* delete a span */
        size_t at, sp;
        if(L < 2) break;
        at = rnd_below(L);
        sp = 1 + rnd_below(L - at);
        memmove(buf + at, buf + at + sp, L - at - sp);
        *len = L - sp;
        break;
    }
    case 4: {                                /* duplicate a span */
        size_t at, sp;
        if(!L || L * 2 >= cap) break;
        at = rnd_below(L);
        sp = 1 + rnd_below(L - at);
        memmove(buf + at + sp, buf + at, L - at);
        *len = L + sp;
        break;
    }
    default:                                 /* truncate */
        if(L) *len = rnd_below(L);
        break;
    }
}

static uint8_t seedbuf[MAXCASE], casebuf[MAXCASE];

int main(int argc, char **argv){
    long iters = 20000;
    int i, nseeds = 0, do_selftest = 0;
    static uint8_t *seeds[64];
    static size_t seedlen[64];
    const char *tmp = getenv("PFR_FUZZ_TMP");

    tmp_path = tmp ? tmp : "/tmp/pfr_fuzz_case.pfr";

    for(i = 1; i < argc; i++){
        if(!strcmp(argv[i], "-selftest")){ do_selftest = 1; }
        else if(!strcmp(argv[i], "-n") && i + 1 < argc){ iters = atol(argv[++i]); }
        else if(!strcmp(argv[i], "-s") && i + 1 < argc){ rng_s = strtoull(argv[++i], NULL, 10); }
        else if(argv[i][0] == '-'){
            fprintf(stderr,
                "usage: %s [-selftest] [-n iters] [-s seed] [seed.pfr ...]\n",
                argv[0]);
            return 2;
        } else if(nseeds < 64){
            FILE *f = fopen(argv[i], "rb");
            size_t got;
            if(!f){ fprintf(stderr, "cannot open seed '%s'\n", argv[i]); return 2; }
            got = fread(seedbuf, 1, sizeof(seedbuf), f);
            fclose(f);
            seeds[nseeds] = (uint8_t*)malloc(got ? got : 1);
            memcpy(seeds[nseeds], seedbuf, got);
            seedlen[nseeds] = got;
            nseeds++;
        }
    }
    if(!rng_s) rng_s = 1;

    if(do_selftest){
        fprintf(stderr, "[selftest] validation pass, src/replay.c\n");
        return selftest();
    }

    if(!nseeds){
        fprintf(stderr, "[fuzz] no seed files given; mutating from empty\n");
        seeds[0] = (uint8_t*)malloc(1);
        seedlen[0] = 0;
        nseeds = 1;
    }

    fprintf(stderr, "[fuzz] %ld iterations, %d seed(s), rng=%llu, tmp=%s\n",
            iters, nseeds, (unsigned long long)rng_s, tmp_path);

    /* Every seed verbatim first: if the corpus itself is refused, everything
     * after it fuzzes error paths while the summary still looks healthy. */
    for(i = 0; i < nseeds; i++) one_case(seeds[i], seedlen[i]);
    if(n_ok == 0)
        fprintf(stderr, "[fuzz] WARNING: no seed parsed cleanly -"
                        " coverage will be shallow\n");

    for(; iters > 0; iters--){
        int s = (int)rnd_below((size_t)nseeds);
        size_t len = seedlen[s], nmut = 1 + rnd_below(8);
        size_t j;
        if(len > sizeof(casebuf)) len = sizeof(casebuf);
        memcpy(casebuf, seeds[s], len);
        for(j = 0; j < nmut; j++) mutate(casebuf, &len, sizeof(casebuf));
        /* Repair the integrity line unless this case is deliberately about
         * the rejection path - roughly one in ten, so both are covered. */
        if(rnd_below(10)) len = fixup_hash(casebuf, len, sizeof(casebuf));
        one_case(casebuf, len);
    }

    remove(tmp_path);
    fprintf(stderr, "[fuzz] %lu cases: %lu accepted, %lu refused."
                    " No crash, no assertion.\n", n_run, n_ok, n_rejected);
    /* Accepting nothing is not a pass.  It means the mutator never built a
     * file the parser would take, so the deep paths - the event loop, the
     * footer, the hash re-read - were never reached at all. */
    if(n_ok * 100 < n_run){
        fprintf(stderr, "[fuzz] FAIL: under 1%% of cases were accepted;"
                        " this run barely reached the parser\n");
        return 1;
    }
    return 0;
}
#endif
