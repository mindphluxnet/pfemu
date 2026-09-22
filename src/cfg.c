/* One settings file per install: PFEMU-STATE/pfemu.cfg.
 *
 * Everything the launcher remembers - volume, sound quality notch, the six
 * PINBALL.CFG option bytes, the trainer switch, fullscreen, "Start at" and
 * the last session file - used to live in six little binary files
 * (pfemu_audio.cfg, pfemu_options.cfg, pfemu_cheats.cfg, pfemu_display.cfg,
 * pfemu_table.cfg, pfemu_session.cfg).  They are all host-only - the guest
 * never opens any of them (src/fantasies.c pokes the option bytes into
 * INTRO.PRG's own buffer instead; see the big comment in src/launch.c) - so
 * nothing about their layout was ever forced by the game.  One key=value
 * text file is easier to read, easier to hand-edit when something needs
 * debugging, and does not grow a new file per setting.
 *
 * Old installs are picked up automatically: when pfemu.cfg is missing,
 * cfg_read() imports whatever the six legacy files hold, and the next
 * cfg_write() lays down pfemu.cfg and deletes them.  A read on its own never
 * writes, which is what keeps replay's promise never to touch the user's
 * real PFEMU-STATE/ (docs/REPLAY.md section 3.3) intact.
 *
 * Unknown keys in the file are dropped on the next write, and an unreadable
 * or partial file just reads as defaults - these are preferences, and losing
 * one is a smaller problem than refusing to start over it.
 */
#include "compat.h"
#include "pfemu.h"

#define CFG_NAME "pfemu.cfg"

/* Also the game's own hardcoded default: a fresh install with no PINBALL.CFG
 * boots with the intro's option buffer already at 00 00 01 00 00 00. */
const uint8_t cfg_option_defaults[6] = {0,0,1,0,0,0};

/* The six option bytes, in PINBALL.CFG's own order (src/launch.c documents
 * what each value means).  One key each rather than one opaque blob: the
 * file is text now, so it may as well say what it is setting. */
static const char *option_keys[6] = {
    "balls", "angle", "scrolling", "music", "resolution", "color"
};

static const char *legacy_names[6] = {
    "pfemu_audio.cfg", "pfemu_options.cfg", "pfemu_cheats.cfg",
    "pfemu_display.cfg", "pfemu_table.cfg", "pfemu_session.cfg"
};

static void cfg_defaults(PfCfg *c){
    memset(c, 0, sizeof(*c));
    c->volume = AUDIO_VOLUME_DEFAULT;
    c->bass = AUDIO_BASS_DEFAULT;
    c->treble = AUDIO_TREBLE_DEFAULT;
    c->oomph = AUDIO_OOMPH_DEFAULT;
    c->headphone = 0;
    c->quality = 0;
    memcpy(c->options, cfg_option_defaults, 6);
}

static void state_path(const char *dir, const char *name, char *out, size_t n){
    snprintf(out, n, "%s/PFEMU-STATE/%s", dir ? dir : "", name);
}

/* ------------------------------------------------------- legacy import --
 *
 * Exactly the reads the old per-file helpers did, including their tolerance
 * for a short file.  Only reached once per install, and only until the
 * first write replaces them. */
static void import_legacy(const char *dir, PfCfg *c){
    char path[600];
    FILE *f;
    uint8_t b[6];
    size_t n;
    int v;

    state_path(dir, legacy_names[0], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 2, f);
        fclose(f);
        if(n >= 1 && b[0] <= 100) c->volume = b[0];
        if(n >= 2 && b[1] <= 4) c->quality = b[1];
    }
    state_path(dir, legacy_names[1], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 6, f);
        fclose(f);
        if(n == 6) memcpy(c->options, b, 6);
    }
    /* Two bytes, once two separate checkboxes: either one on means the
     * trainer is on (src/fantasies.c load_cheat_cfg said the same). */
    state_path(dir, legacy_names[2], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 2, f);
        fclose(f);
        if(n == 2) c->trainer = (b[0] != 0) || (b[1] != 0);
    }
    state_path(dir, legacy_names[3], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 1, f);
        fclose(f);
        if(n == 1) c->fullscreen = b[0] != 0;
    }
    state_path(dir, legacy_names[4], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        if(fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 4) c->start_table = v;
        fclose(f);
    }
    state_path(dir, legacy_names[5], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        if(fgets(c->session, (int)sizeof(c->session), f))
            c->session[strcspn(c->session, "\r\n")] = 0;
        else
            c->session[0] = 0;
        fclose(f);
    }
}

static void drop_legacy(const char *dir){
    char path[600];
    int i;
    for(i=0;i<6;i++){
        state_path(dir, legacy_names[i], path, sizeof(path));
        DeleteFileA(path);
    }
}

/* ------------------------------------------------------------- parsing -- */
static char *trim(char *s){
    char *e;
    while(*s==' '||*s=='\t') s++;
    e = s + strlen(s);
    while(e > s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
    return s;
}

static void apply_pair(PfCfg *c, const char *key, const char *val){
    int i, v = atoi(val);
    if(!strcmp(key, "volume")){ if(v >= 0 && v <= 100) c->volume = v; return; }
    if(!strcmp(key, "bass")){ if(v >= -12 && v <= 12) c->bass = v; return; }
    if(!strcmp(key, "treble")){ if(v >= -12 && v <= 12) c->treble = v; return; }
    if(!strcmp(key, "oomph")){ if(v >= 0 && v <= 12) c->oomph = v; return; }
    if(!strcmp(key, "headphone")){ c->headphone = v != 0; return; }
    if(!strcmp(key, "quality")){ if(v >= 0 && v <= 4) c->quality = v; return; }
    if(!strcmp(key, "trainer")){ c->trainer = v != 0; return; }
    if(!strcmp(key, "fullscreen")){ c->fullscreen = v != 0; return; }
    if(!strcmp(key, "start_table")){ if(v >= 0 && v <= 4) c->start_table = v; return; }
    if(!strcmp(key, "session")){
        snprintf(c->session, sizeof(c->session), "%s", val);
        return;
    }
    for(i=0;i<6;i++)
        if(!strcmp(key, option_keys[i])){
            /* Range is per option (2 or 3 choices), but the intro's own
             * buffer is a byte and out-of-range values are the launcher's
             * problem to clamp, not this parser's. */
            if(v >= 0 && v <= 255) c->options[i] = (uint8_t)v;
            return;
        }
}

void cfg_read(const char *dir, PfCfg *c){
    char path[600], line[700];
    FILE *f;
    cfg_defaults(c);
    if(!dir || !dir[0]) return;
    state_path(dir, CFG_NAME, path, sizeof(path));
    f = fopen(path, "rb");
    if(!f){ import_legacy(dir, c); return; }
    while(fgets(line, sizeof(line), f)){
        char *k, *eq;
        k = trim(line);
        if(!*k || *k=='#' || *k==';') continue;
        eq = strchr(k, '=');
        if(!eq) continue;
        *eq = 0;
        apply_pair(c, trim(k), trim(eq+1));
    }
    fclose(f);
}

void cfg_write(const char *dir, const PfCfg *c){
    char path[600], sub[600];
    FILE *f;
    int i, vol, qual, tbl;
    if(!dir || !dir[0]) return;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    state_path(dir, CFG_NAME, path, sizeof(path));
    f = fopen(path, "wb");
    if(!f) return;
    vol = c->volume < 0 ? 0 : (c->volume > 100 ? 100 : c->volume);
    qual = c->quality < 0 ? 0 : (c->quality > 4 ? 4 : c->quality);
    tbl = (c->start_table >= 1 && c->start_table <= 4) ? c->start_table : 0;
    fprintf(f, "# pfemu settings for this install - written by the launcher.\n"
               "# The game never reads this file; delete it to start over.\n");
    fprintf(f, "volume=%d\n", vol);
    { int b = c->bass < -12 ? -12 : (c->bass > 12 ? 12 : c->bass);
      int t = c->treble < -12 ? -12 : (c->treble > 12 ? 12 : c->treble);
      int o = c->oomph < 0 ? 0 : (c->oomph > 12 ? 12 : c->oomph);
      fprintf(f, "bass=%d\n", b);
      fprintf(f, "treble=%d\n", t);
      fprintf(f, "oomph=%d\n", o);
      fprintf(f, "headphone=%d\n", c->headphone ? 1 : 0); }
    fprintf(f, "quality=%d\n", qual);
    for(i=0;i<6;i++) fprintf(f, "%s=%d\n", option_keys[i], c->options[i]);
    fprintf(f, "trainer=%d\n", c->trainer ? 1 : 0);
    fprintf(f, "fullscreen=%d\n", c->fullscreen ? 1 : 0);
    fprintf(f, "start_table=%d\n", tbl);
    fprintf(f, "session=%s\n", c->session);
    fclose(f);
    drop_legacy(dir);
}

/* ----------------------------------------------- effective-setting readers
 *
 * These four used to live in src/launch.c, purely because the launcher was
 * their first caller.  Nothing in them is a dialog: they are the same
 * settings this file already owns, read the way the rest of the program
 * needs them - the effective value, which for sound means the overlay's
 * SOUND.CFG before the installed one before the mirror kept here.
 *
 * src/run.c calls all four on every run, headless included, so leaving them
 * in the Win32-only launcher meant a build without a GUI could not read its
 * own configuration.  They are plain stdio and belong here.
 */
/* 1 when the effective config (overlay first, then installed file) selects
 * a SoundBlaster-family driver; used for the checkbox initial state. */
int read_sound_is_sb(const char *dir){
    FILE *f;
    char name[13];
    int i;
    const char *cands[2];
    static char ov[600], orig[600];
    snprintf(ov, sizeof(ov), "%s/PFEMU-STATE/SOUND.CFG", dir);
    snprintf(orig, sizeof(orig), "%s/SOUND.CFG", dir);
    cands[0]=ov; cands[1]=orig;
    for(i=0;i<2;i++){
        f = fopen(cands[i], "rb");
        if(!f) continue;
        memset(name, 0, sizeof(name));
        fread(name, 1, 12, f);
        fclose(f);
        return strncmp(name, "SBLASTER", 8)==0 || strncmp(name, "SBPRO", 5)==0 ||
               strncmp(name, "SB16", 4)==0 || strncmp(name, "SB20", 4)==0;
    }
    return 0;
}

/* ------------------------------------------------------------- audio I/O
 *
 * Two of the settings in the install's PFEMU-STATE/pfemu.cfg (src/cfg.c):
 * volume 0-100 and the quality notch 0-4.
 *
 * Volume is host-only by nature.  The guest has nothing to configure: the
 * driver mixes at full scale, and the card's mixer registers are an SB Pro
 * feature this DSP-1.05 card does not have - so the setting is a property of
 * the host sink alone (see the comment over audio_volume in src/sound.c).
 *
 * Quality *is* a guest setting - it goes in SOUND.CFG byte 0x14, where the
 * driver reads it - but it is mirrored here so that turning sound off and on
 * again doesn't silently reset it: a NOSOUND.SDR config is 16 bytes long and
 * has no byte 0x14 to remember it in.  SOUND.CFG still wins when it has one,
 * so running the real SETSOUND (-setup) is still picked up here. */
int read_volume_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    return c.volume;
}

/* Volume alone, for the in-window -/+ keys on their way out (src/main.c).
 * Reads the whole file first so every other setting survives: the game
 * window can change none of them, and clobbering one would silently undo
 * the launcher. */
void write_volume_cfg(const char *dir, int vol){
    PfCfg c;
    cfg_read(dir, &c);
    c.volume = vol;
    cfg_write(dir, &c);
}

/* Quality notch actually in force: the effective SOUND.CFG when it is an SB
 * config long enough to carry one (overlay first, then installed, same order
 * as read_sound_is_sb), otherwise the mirror above. */
int read_sound_quality(const char *dir){
    FILE *f;
    uint8_t buf[25];
    size_t n;
    int i;
    char path[600];
    PfCfg c;
    for(i=0;i<2;i++){
        snprintf(path, sizeof(path), i==0 ? "%s/PFEMU-STATE/SOUND.CFG" : "%s/SOUND.CFG", dir);
        f = fopen(path, "rb");
        if(!f) continue;
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if(n > 0x14 && buf[0x14] <= 4) return buf[0x14];
        break;
    }
    cfg_read(dir, &c);
    return c.quality;
}
