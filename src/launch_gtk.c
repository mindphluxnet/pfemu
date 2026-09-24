/* GTK 3 launcher: the Linux face of the launcher.
 *
 * The same window as src/launch.c, on GTK instead of Win32, and the same
 * rules, because both call src/launchcore.c for every decision that matters:
 * which replay may run against which installation, what Details says, where
 * a recording goes.  What is not here yet is the Leaderboard group - login,
 * Submit, the submissions list - because src/online.c is WinHTTP and DPAPI;
 * it comes with a libcurl port.  The Ranked checkbox is here: it decides how
 * a recording is made, which matters with or without an upload.
 *
 * Differences from the Win32 launcher, each on purpose:
 *
 *   - No saved window position.  Wayland does not let a program place its
 *     own windows, and GNOME on Wayland is what Ubuntu runs.
 *   - Replays: Delete is a button (and the Delete key) acting on the
 *     selected rows, not a link in every row, and it moves the files to the
 *     desktop's Trash.  A file on a filesystem without a trash stays where
 *     it is, with a message: a recording cannot be made again.
 *   - Leaving Replay mode puts the installation's own settings back into
 *     the window.  Replay shows the file's settings, display only; on
 *     Windows they stay in the window afterwards and a Play launch writes
 *     them to the installation.  Switching installations also reloads the
 *     six game options and "Start at", which the Win32 launcher leaves as
 *     they were.
 *
 * The game still runs as a child process, this program again with
 * -nolauncher -launched, for the reason src/launch.c gives: nothing from one
 * session may reach the next recording.  The child never initialises GTK.
 */
#ifndef _WIN32

#include <gtk/gtk.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/wait.h>
#include "compat.h"
#include "launchcore.h"
#include "build.h"

/* The SDL host (src/host_sdl.c): 1 when SDL_VIDEODRIVER is its own choice
 * rather than the user's. */
int plat_sdl_env_ours(void);

/* How many installation directories the picker will list.  Anyone with more
 * than this many collected side by side can pass -d. */
#define MAX_INSTALLS 8
#define RL_DIR       "sessions"

typedef struct {
    int sound, quality, volume, bass, treble, oomph, headphone;
    uint8_t cfg[6];
    int cheat_enable, fullscreen, start_table;
    RelResult inst[MAX_INSTALLS];
    int ninst, sel;
    /* Session record / replay, as in src/launch.c. */
    LaunchMode mode;
    char replay_path[512];
    int path_custom;         /* the user typed or picked a path; keep it */
    ReplayHeader rhdr;
    int rhdr_ok;
    char replay_err[256];
    int ranked;              /* Record mode's Ranked checkbox, default on */
    /* Set while the program moves a widget itself: GTK reports that as a
     * change like any other, and those handlers must only hear the user. */
    int updating;
    /* The running game, if any. */
    int child_running;
    LaunchMode child_mode;
    char child_path[512];    /* what that session records or replays */
    GtkWidget *win, *install, *detected, *details;
    GtkWidget *w_sound, *w_quality, *w_volume, *w_bass, *w_treble, *w_oomph, *w_headphone;
    GtkWidget *w_opt[6], *w_trainer, *w_fullscreen, *w_table;
    GtkWidget *mode_play, *mode_record, *mode_replay, *ranked_cb;
    GtkWidget *path_label, *path, *browse, *launch;
    struct RlState *replays; /* the Replays window while it is open */
} LaunchState;

static void show_detection(LaunchState *st);
static void reload_for_dir(LaunchState *st);
static void rl_rescan(struct RlState *d);

/* --------------------------------------------------------------- helpers */
static const RelResult *cur_inst(const LaunchState *st){
    return (st->ninst && st->sel >= 0 && st->sel < st->ninst)
           ? &st->inst[st->sel] : NULL;
}

static const char *cur_game_dir(const LaunchState *st){
    const RelResult *r = cur_inst(st);
    return r ? r->dir : "";
}

static void set_check(LaunchState *st, GtkWidget *w, int on){
    st->updating++;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), on ? TRUE : FALSE);
    st->updating--;
}

static void set_combo(LaunchState *st, GtkWidget *w, int i){
    st->updating++;
    gtk_combo_box_set_active(GTK_COMBO_BOX(w), i);
    st->updating--;
}

static void set_path_text(LaunchState *st){
    st->updating++;
    gtk_entry_set_text(GTK_ENTRY(st->path), st->replay_path);
    st->updating--;
}

/* The detection line, with the whole text as its tooltip: a record target
 * is long, and the label ends in an ellipsis where the window does. */
static void set_detected(LaunchState *st, const char *text){
    gtk_label_set_text(GTK_LABEL(st->detected), text);
    gtk_widget_set_tooltip_text(st->detected, text);
}

/* A message box owned by the launcher.  Yes/No returns 1 for Yes. */
static int message(GtkWidget *owner, GtkMessageType type, GtkButtonsType buttons,
                   const char *title, const char *text){
    GtkWidget *d = gtk_message_dialog_new(owner ? GTK_WINDOW(owner) : NULL,
                                          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          type, buttons, "%s", text);
    int r;
    gtk_window_set_title(GTK_WINDOW(d), title);
    if(buttons == GTK_BUTTONS_YES_NO)
        gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_NO);
    r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_YES;
}

/* Two paths naming the same file.  Record mode names files relative, a
 * picked one is absolute. */
static int same_file(const char *a, const char *b){
    char fa[PATH_MAX], fb[PATH_MAX];
    if(!a[0] || !b[0]) return 0;
    if(realpath(a, fa) && realpath(b, fb)) return !strcmp(fa, fb);
    return !strcmp(a, b);
}

/* ------------------------------------------------ Ranked, remembered
 * The Ranked checkbox lives in pfemu-online.cfg beside the program, as on
 * Windows (src/online.c), so a folder shared by both builds shares it.
 * Only the ranked= line is touched: the file also holds the Windows
 * launcher's account, which this build must not lose. */
#define ONLINE_FILE "pfemu-online.cfg"

static int ranked_load(void){
    char path[1100], line[2400];
    int on = 1;
    FILE *f;
    beside_exe(path, sizeof(path), ONLINE_FILE);
    f = fopen(path, "r");
    if(!f) return on;
    while(fgets(line, sizeof(line), f)){
        const char *k = line;
        while(*k == ' ' || *k == '\t') k++;
        if(!strncmp(k, "ranked", 6)){
            const char *v = k + 6;
            while(*v == ' ' || *v == '\t') v++;
            if(*v == '=') on = atoi(v + 1) != 0;
        }
    }
    fclose(f);
    return on;
}

static void ranked_save(int on){
    char path[1100], tmp[1110], line[2400];
    FILE *in, *out;
    int done = 0;
    beside_exe(path, sizeof(path), ONLINE_FILE);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    out = fopen(tmp, "w");
    if(!out) return;
    in = fopen(path, "r");
    if(!in) fprintf(out, "# pfemu launcher: leaderboard account and settings.\n");
    while(in && fgets(line, sizeof(line), in)){
        const char *k = line;
        while(*k == ' ' || *k == '\t') k++;
        if(!strncmp(k, "ranked", 6) && strchr(k, '=')){
            if(!done) fprintf(out, "ranked=%d\n", on ? 1 : 0);
            done = 1;
            continue;
        }
        fputs(line, out);
    }
    if(!done) fprintf(out, "ranked=%d\n", on ? 1 : 0);
    if(in) fclose(in);
    if(fclose(out) == 0) rename(tmp, path);
    else remove(tmp);
}

/* --------------------------------------------------- mode and detection */
/* Auto-restore on replay-file load (REPLAY.md 4.1): scan the installs and
 * pick one that IS the recording - exact vector match first (preferring the
 * recorded dir if it still matches), else the first runnable install of the
 * same release, else no auto-launch.  Never applies one release's layout to
 * another: that falls out of same_vector() refusing. */
static void replay_autorestore(LaunchState *st){
    int k, same = -1;
    if(!st->rhdr_ok || !st->rhdr.trainer_off) return;
    for(k=0;k<st->ninst;k++){
        const RelResult *r = &st->inst[k];
        if(!release_runnable(r) || !same_vector(&st->rhdr, r)) continue;
        if(same < 0) same = k;
        if(!_stricmp(r->dir, st->rhdr.dir_hint)){ same = k; break; }
    }
    if(same < 0)
        for(k=0;k<st->ninst;k++){
            const RelResult *r = &st->inst[k];
            if(release_runnable(r) && r->rel && !_stricmp(st->rhdr.release_id, r->rel->id)){
                same = k;
                break;
            }
        }
    /* No match: no auto-launch.  show_detection() leaves Launch disabled
     * and Details carries the recorded-vs-found report. */
    if(same < 0) return;
    st->sel = same;
    if(st->install) set_combo(st, st->install, st->sel);
    reload_for_dir(st);
}

/* Push one replay file through parse + restore + detection-line update.
 * Silent (no popups): typing a half-finished path must not nag. */
static void replay_load_file(LaunchState *st){
    if(!st->replay_path[0]){
        st->rhdr_ok = 0;
        snprintf(st->replay_err, sizeof(st->replay_err), "Pick a .pfr replay file.");
    } else if(replay_read_header(st->replay_path, &st->rhdr) != 0){
        st->rhdr_ok = 0;
        snprintf(st->replay_err, sizeof(st->replay_err),
                 "Not a readable replay file: %.200s", st->replay_path);
    } else {
        st->rhdr_ok = 1;
        st->replay_err[0] = 0;
        replay_autorestore(st);
    }
    show_detection(st);
}

/* Fill the path field on mode entry: a fresh record target in record mode,
 * else the install's last-used session file, else empty.  Never clobbers a
 * user-typed or picked path.  Install switches keep the current path,
 * except in record mode, which re-targets to the new install. */
static void restore_session_path(LaunchState *st, int install_switch){
    char saved[512];
    if(st->path_custom) return;
    if(install_switch && st->mode == LAUNCH_REPLAY) return;
    read_session_path(cur_game_dir(st), saved, sizeof(saved));
    if(st->mode == LAUNCH_RECORD)
        launch_record_path(cur_game_dir(st), st->replay_path, sizeof(st->replay_path));
    else if(saved[0]) snprintf(st->replay_path, sizeof(st->replay_path), "%s", saved);
    else st->replay_path[0] = 0;
    set_path_text(st);
    if(st->mode == LAUNCH_REPLAY) replay_load_file(st);
}

/* Mode switching: the trainer checkbox is greyed out in record and replay
 * modes (REPLAY.md 4), and entering those modes unchecks it; main() refuses
 * either mode with it on as a backstop.  Ranked only means something while
 * recording. */
static void apply_mode_ui(LaunchState *st, LaunchMode prev){
    int rec = (st->mode != LAUNCH_PLAY);
    set_check(st, st->mode_play, st->mode == LAUNCH_PLAY);
    set_check(st, st->mode_record, st->mode == LAUNCH_RECORD);
    set_check(st, st->mode_replay, st->mode == LAUNCH_REPLAY);
    /* The .pfr records how its session began - menu or a table - and that
     * has to win, so replay owns this control. */
    gtk_widget_set_sensitive(st->w_table, st->mode != LAUNCH_REPLAY);
    gtk_widget_set_sensitive(st->path, rec);
    gtk_widget_set_sensitive(st->path_label, rec);
    gtk_widget_set_sensitive(st->ranked_cb, st->mode == LAUNCH_RECORD);
    /* Record picks a target file; otherwise the button is the Replays
     * window, which lists the recordings and switches to Replay for one. */
    gtk_button_set_label(GTK_BUTTON(st->browse),
                         st->mode == LAUNCH_RECORD ? "Browse..." : "Replays...");
    /* Replay showed the file's settings; the installation's come back. */
    if(prev == LAUNCH_REPLAY && st->mode != LAUNCH_REPLAY) reload_for_dir(st);
    if(rec){
        st->cheat_enable = 0;
        set_check(st, st->w_trainer, 0);
        gtk_widget_set_sensitive(st->w_trainer, FALSE);
        restore_session_path(st, 0);
    } else {
        gtk_widget_set_sensitive(st->w_trainer, TRUE);
        st->cheat_enable = read_trainer_cfg(cur_game_dir(st));
        set_check(st, st->w_trainer, st->cheat_enable);
    }
    show_detection(st);
}

/* The read-only line saying what the detector concluded, and whether
 * Launch may be pressed: only for a release that was actually recognised,
 * and in replay mode only for a file that matches it. */
static void show_detection(LaunchState *st){
    const RelResult *r = cur_inst(st);
    char line[1400], cwd[1024];
    int can = 0, i, has_opts;
    if(!r){
        if(!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
        snprintf(line, sizeof(line), "No game found in %s. Put one release's files"
                 " in a folder there, GAME for example.", cwd);
    }
    if(st->mode == LAUNCH_REPLAY){
        char why[256];
        if(replay_check(st->rhdr_ok, &st->rhdr, st->replay_err, r, why, sizeof(why))){
            snprintf(line, sizeof(line), "Replay ready: %s", st->rhdr.summary);
            can = 1;
        } else {
            snprintf(line, sizeof(line), "Replay: %s", why);
        }
        gtk_widget_set_sensitive(st->details, TRUE);
    } else if(st->mode == LAUNCH_RECORD){
        if(r && release_runnable(r) && st->replay_path[0])
            snprintf(line, sizeof(line), "Record %.120s -> %.400s", r->summary, st->replay_path);
        else if(r && release_runnable(r))
            snprintf(line, sizeof(line), "Record %s (pick a target file)", r->summary);
        else if(r)
            snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
        can = r && release_runnable(r) && st->replay_path[0];
        gtk_widget_set_sensitive(st->details, r != NULL);
    } else {
        if(r && release_runnable(r)) snprintf(line, sizeof(line), "Detected: %s", r->summary);
        else if(r) snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
        can = r && release_runnable(r);
        gtk_widget_set_sensitive(st->details, r != NULL);
    }
    set_detected(st, line);
    /* The six game options are the intro's own PINBALL.CFG structure, and
     * the 1993 demo's intro has none: greyed rather than offered in vain. */
    has_opts = r && release_runnable(r) && r->rel && r->rel->cfg_buf != 0;
    for(i=0;i<6;i++) gtk_widget_set_sensitive(st->w_opt[i], has_opts);
    /* One game at a time: it owns the audio device and the install's
     * PFEMU-STATE/ while it runs. */
    gtk_widget_set_sensitive(st->launch, can && !st->child_running);
}

/* Every per-install setting for the selected directory, into the widgets:
 * at startup and whenever the installation changes, so one install's
 * settings are never applied to another. */
static void reload_for_dir(LaunchState *st){
    const char *dir = cur_game_dir(st);
    PfCfg c;
    int i;
    cfg_read(dir, &c);
    st->sound = read_sound_is_sb(dir);
    st->quality = read_sound_quality(dir);   /* SOUND.CFG wins over c.quality */
    st->volume = c.volume;
    st->bass = c.bass;
    st->treble = c.treble;
    st->oomph = c.oomph;
    st->headphone = c.headphone;
    memcpy(st->cfg, c.options, 6);
    st->cheat_enable = c.trainer;
    st->fullscreen = c.fullscreen;
    st->start_table = c.start_table;
    if(st->mode == LAUNCH_REPLAY && st->rhdr_ok){
        /* Replay shows the FILE's session, not the install's settings:
         * sound, quality, the six options and fullscreen are what the run
         * will use.  Display only - replay mode writes no configs. */
        st->sound = st->rhdr.sound ? 1 : 0;
        st->quality = st->rhdr.quality;
        if(st->quality < 0) st->quality = 0;
        if(st->quality > 4) st->quality = 4;
        for(i=0;i<6;i++){
            int v = st->rhdr.options[i];
            if(v < 0) v = 0;
            if(v >= launch_opts[i].n) v = launch_opts[i].n - 1;
            st->cfg[i] = (uint8_t)v;
        }
        st->fullscreen = st->rhdr.fullscreen ? 1 : 0;
    }
    set_check(st, st->w_sound, st->sound);
    set_combo(st, st->w_quality, st->quality);
    st->updating++;
    gtk_range_set_value(GTK_RANGE(st->w_volume), st->volume);
    st->updating--;
    set_combo(st, st->w_bass, eq_db_to_idx(st->bass));
    set_combo(st, st->w_treble, eq_db_to_idx(st->treble));
    set_combo(st, st->w_oomph, oomph_db_to_idx(st->oomph));
    set_check(st, st->w_headphone, st->headphone);
    for(i=0;i<6;i++) set_combo(st, st->w_opt[i], st->cfg[i]);
    set_check(st, st->w_fullscreen, st->fullscreen);
    set_combo(st, st->w_table, st->start_table);
    if(st->mode != LAUNCH_PLAY){
        /* The install switch must not resurrect the trainer behind the
         * mode's back: it stays unchecked and greyed until Play returns. */
        st->cheat_enable = 0;
        gtk_widget_set_sensitive(st->w_trainer, FALSE);
        if(st->mode == LAUNCH_RECORD && !st->path_custom)
            restore_session_path(st, 1);
    }
    set_check(st, st->w_trainer, st->cheat_enable);
    show_detection(st);
}

/* --------------------------------------------------------- details window */
/* A monospace, scrollable, resizable report with Copy (src/launch.c has the
 * story of why it is not a message box). */
enum { DET_COPY = 1 };

static void show_details(LaunchState *st){
    static char text[DET_MAX];
    GtkWidget *d, *sw, *tv, *copy;
    int r;
    details_report(text, sizeof(text), st->mode, st->replay_path, st->rhdr_ok,
                   &st->rhdr, st->replay_err, cur_inst(st));
    d = gtk_dialog_new_with_buttons("pfemu - details", GTK_WINDOW(st->win),
                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                    "Copy", DET_COPY, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 780, 540);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(sw), 8);
    tv = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 4);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), text, -1);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), sw,
                       TRUE, TRUE, 0);
    copy = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), DET_COPY);
    gtk_widget_show_all(d);
    while((r = gtk_dialog_run(GTK_DIALOG(d))) == DET_COPY){
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
        gtk_button_set_label(GTK_BUTTON(copy), "Copied");
    }
    gtk_widget_destroy(d);
}

/* ---------------------------------------------------------- replays window
 * Every recording in sessions/, newest first, what it holds, and Delete.
 * It is where Replay mode picks its file; Other file... is there for one
 * kept elsewhere. */
enum { RC_FILE, RC_WHEN, RC_SCORES, RC_LENGTH, RC_STATE, RC_INDEX, RC_COLS };
enum { RL_OTHER = 1, RL_REPLAY = 2 };

typedef struct {
    char path[512];          /* sessions/<name>.pfr */
    time_t mtime;
    long long size;
    ReplayHeader hd;
    int hd_ok;
    long long best[5];       /* read_claims() */
    int have_games;
} RlItem;

typedef struct RlState {
    LaunchState *st;
    GtkWidget *dlg, *view, *detail, *count, *del, *replay;
    GtkListStore *store;
    RlItem *it;
    int n;
    char picked[512];
} RlState;

static int rl_ends_pfr(const char *name){
    size_t l = strlen(name);
    return l > 4 && !_stricmp(name + l - 4, ".pfr");
}

static int rl_cmp(const void *a, const void *b){
    time_t x = ((const RlItem*)a)->mtime, y = ((const RlItem*)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void rl_scan(RlState *d){
    DIR *dir = opendir(RL_DIR);
    struct dirent *e;
    int cap = 0;
    free(d->it);
    d->it = NULL;
    d->n = 0;
    if(!dir) return;
    while((e = readdir(dir)) != NULL){
        RlItem *x;
        struct stat sb;
        char path[512];
        if(!rl_ends_pfr(e->d_name)) continue;
        snprintf(path, sizeof(path), RL_DIR "/%s", e->d_name);
        if(stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
        if(d->n == cap){
            int nc = cap ? cap * 2 : 32;
            RlItem *ni = (RlItem*)realloc(d->it, (size_t)nc * sizeof(*ni));
            if(!ni) break;
            d->it = ni;
            cap = nc;
        }
        x = &d->it[d->n++];
        memset(x, 0, sizeof(*x));
        snprintf(x->path, sizeof(x->path), "%s", path);
        x->mtime = sb.st_mtime;
        x->size = (long long)sb.st_size;
        x->hd_ok = replay_read_header(x->path, &x->hd) == 0;
        x->have_games = read_claims(x->path, x->best);
    }
    closedir(dir);
    if(d->n > 1) qsort(d->it, (size_t)d->n, sizeof(*d->it), rl_cmp);
}

/* The running game's own recording: still being written, so hands off. */
static int rl_busy(const RlState *d, const RlItem *x){
    return d->st->child_running && d->st->child_mode == LAUNCH_RECORD &&
           same_file(d->st->child_path, x->path);
}

static void rl_when(time_t t, char *out, size_t n){
    struct tm tm;
    if(!localtime_r(&t, &tm)){ snprintf(out, n, "?"); return; }
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void rl_fill(RlState *d, const char *select){
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    GtkTreeIter at, sel_it;
    int i, have_sel = 0;
    char line[96];
    gtk_list_store_clear(d->store);
    for(i = 0; i < d->n; i++){
        RlItem *x = &d->it[i];
        char when[32], scores[200] = "", len[16] = "", state[64];
        const char *b = base_name(x->path);
        char name[300];
        size_t k = 0;
        int t;
        snprintf(name, sizeof(name), "%.*s", (int)strlen(b) - 4, b);
        rl_when(x->mtime, when, sizeof(when));
        if(x->have_games){
            for(t = 1; t <= 4 && k < sizeof(scores); t++){
                char sc[32];
                if(x->best[t] < 0) continue;
                fmt_score(x->best[t], sc, sizeof(sc));
                k += (size_t)snprintf(scores + k, sizeof(scores) - k, "%s%s %s",
                                      k ? ", " : "", sc, table_name(t));
            }
            if(!k) snprintf(scores, sizeof(scores), "none finished");
        } else if(x->hd_ok && x->hd.have_end && !rl_busy(d, x)){
            snprintf(scores, sizeof(scores), "not counted yet - replay it once");
        }
        if(x->hd_ok && x->hd.have_end){
            int s = (int)(x->hd.end_emu + 0.5);
            snprintf(len, sizeof(len), "%d:%02d", s / 60, s % 60);
        }
        if(rl_busy(d, x)) snprintf(state, sizeof(state), "being recorded");
        else if(!x->hd_ok) snprintf(state, sizeof(state), "not a readable recording");
        else if(!x->hd.have_end) snprintf(state, sizeof(state), "incomplete");
        else if(!x->hd.state[0]) snprintf(state, sizeof(state), "not ranked");
        else snprintf(state, sizeof(state), "ranked");
        gtk_list_store_append(d->store, &at);
        gtk_list_store_set(d->store, &at, RC_FILE, name, RC_WHEN, when, RC_SCORES, scores,
                           RC_LENGTH, len, RC_STATE, state, RC_INDEX, i, -1);
        if(!have_sel || (select && select[0] && same_file(x->path, select))){
            sel_it = at;
            have_sel = 1;
        }
    }
    gtk_tree_selection_unselect_all(ts);
    if(have_sel){
        GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(d->store), &sel_it);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(d->view), p, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(d->view), p, NULL, FALSE, 0, 0);
        gtk_tree_path_free(p);
    }
    if(!d->n) snprintf(line, sizeof(line), "No recordings in " RL_DIR "/ yet.");
    else snprintf(line, sizeof(line), "%d recording%s in " RL_DIR "/", d->n,
                  d->n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(d->count), line);
}

/* The rows the selection covers, as indexes into it[]; the caller frees. */
static int *rl_selected_rows(RlState *d, int *count){
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    GList *rows = gtk_tree_selection_get_selected_rows(ts, NULL), *l;
    int *out = (int*)malloc(sizeof(int) * (size_t)(d->n ? d->n : 1)), k = 0;
    for(l = rows; l && out; l = l->next){
        GtkTreeIter it;
        int i;
        if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(d->store), &it, (GtkTreePath*)l->data))
            continue;
        gtk_tree_model_get(GTK_TREE_MODEL(d->store), &it, RC_INDEX, &i, -1);
        if(i >= 0 && i < d->n && k < d->n) out[k++] = i;
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    *count = k;
    return out;
}

/* The one row the details and Replay are about: the cursor row when it is
 * selected, else the first selected one. */
static int rl_selected(RlState *d){
    GtkTreePath *p = NULL;
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    int i = -1, n, *rows;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(d->view), &p, NULL);
    if(p && gtk_tree_selection_path_is_selected(ts, p)){
        GtkTreeIter it;
        if(gtk_tree_model_get_iter(GTK_TREE_MODEL(d->store), &it, p))
            gtk_tree_model_get(GTK_TREE_MODEL(d->store), &it, RC_INDEX, &i, -1);
    }
    if(p) gtk_tree_path_free(p);
    if(i >= 0 && i < d->n) return i;
    rows = rl_selected_rows(d, &n);
    i = (rows && n) ? rows[0] : -1;
    free(rows);
    return i;
}

/* Everything known about the selected recording, in the Details idiom. */
static void rl_detail(RlState *d){
    static char raw[8192];
    int i = rl_selected(d), any_free = 0, n, k, *rows;
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->detail));
    char buf[256], sz[32];
    RlItem *x;
    raw[0] = 0;
    rows = rl_selected_rows(d, &n);
    for(k = 0; rows && k < n; k++) if(!rl_busy(d, &d->it[rows[k]])) any_free = 1;
    free(rows);
    gtk_widget_set_sensitive(d->del, any_free);
    gtk_widget_set_sensitive(d->replay, i >= 0 && d->it[i].hd_ok && d->it[i].hd.have_end &&
                             !rl_busy(d, &d->it[i]));
    if(i < 0){
        gtk_text_buffer_set_text(tb, d->n ? "Pick a recording to see what it holds." : "", -1);
        return;
    }
    x = &d->it[i];
    fmt_score(x->size, sz, sizeof(sz));
    det_kv(raw, sizeof(raw), "File", "%s, %s bytes", x->path, sz);
    rl_when(x->mtime, buf, sizeof(buf));
    det_kv(raw, sizeof(raw), "Recorded", "%s", buf);
    if(!x->hd_ok){
        det_kv(raw, sizeof(raw), "", "Not a readable recording.");
    } else {
        const ReplayHeader *h = &x->hd;
        det_kv(raw, sizeof(raw), "Release", "%s - %s", h->release_id, h->summary);
        det_kv(raw, sizeof(raw), "Starts at", "%s", (h->start_table >= 1 && h->start_table <= 4)
               ? table_labels[h->start_table] : "the table menu");
        if(rl_busy(d, x)) det_kv(raw, sizeof(raw), "Session", "being recorded");
        else if(h->have_end)
            det_kv(raw, sizeof(raw), "Session", "%d events, %.1f s", h->nevents, h->end_emu);
        else det_kv(raw, sizeof(raw), "Session", "incomplete - the recording has no end");
        det_kv(raw, sizeof(raw), "Ranked", "%s", h->state[0]
               ? "yes, recorded against the canonical state"
               : "no, recorded with the player's own high-score tables");
    }
    games_report(raw, sizeof(raw), x->path, x->have_games);
    gtk_text_buffer_set_text(tb, raw, -1);
}

static void rl_rescan(RlState *d){
    char keep[600] = "";
    int i = rl_selected(d);
    if(i >= 0) snprintf(keep, sizeof(keep), "%s", d->it[i].path);
    rl_scan(d);
    rl_fill(d, keep);
    rl_detail(d);
}

/* Every selected row, to the desktop's Trash (GIO), .games file and all. */
static void rl_delete(RlState *d){
    LaunchState *st = d->st;
    int n, nr = 0, ranked = 0, failed = 0, i, *rows = rl_selected_rows(d, &n);
    char box[700], sel_path[600] = "";
    if(!rows) return;
    for(i = 0; i < n; i++) if(!rl_busy(d, &d->it[rows[i]])) rows[nr++] = rows[i];
    if(!nr){ free(rows); return; }
    for(i = 0; i < nr; i++){
        const RlItem *x = &d->it[rows[i]];
        if(x->hd_ok && x->hd.state[0] && x->hd.have_end) ranked++;
    }
    if(nr == 1)
        snprintf(box, sizeof(box), "Move %s to the Trash?%s", base_name(d->it[rows[0]].path),
                 ranked ? "\n\nIt is a complete ranked recording." : "");
    else
        snprintf(box, sizeof(box), "Move %d recordings to the Trash?", nr);
    if(nr > 1 && ranked == 1)
        snprintf(box + strlen(box), sizeof(box) - strlen(box),
                 "\n\nOne of them is a complete ranked recording.");
    else if(nr > 1 && ranked)
        snprintf(box + strlen(box), sizeof(box) - strlen(box),
                 "\n\n%d of them are complete ranked recordings.", ranked);
    if(!message(d->dlg, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "pfemu - delete", box)){
        free(rows);
        return;
    }
    /* Keep the place in the list: the row after the first deleted one. */
    { int after = rows[0], j;
      for(i = 0; i < nr; i++) if(rows[i] < after) after = rows[i];
      for(i = after; i < d->n; i++){
          int gone = 0;
          for(j = 0; j < nr; j++) if(rows[j] == i) gone = 1;
          if(!gone){ snprintf(sel_path, sizeof(sel_path), "%s", d->it[i].path); break; }
      } }
    for(i = 0; i < nr; i++){
        const char *p = d->it[rows[i]].path;
        char games[640];
        GFile *f = g_file_new_for_path(p);
        GError *err = NULL;
        /* Asked before the file goes: same_file() resolves both paths. */
        int current = same_file(p, st->replay_path);
        if(!g_file_trash(f, NULL, &err)){
            fprintf(stderr, "[launcher] %s not moved to the Trash: %s\n", p,
                    err ? err->message : "?");
            failed++;
        } else {
            GFile *g;
            snprintf(games, sizeof(games), "%s.games", p);
            g = g_file_new_for_path(games);
            if(g_file_query_exists(g, NULL)) g_file_trash(g, NULL, NULL);
            g_object_unref(g);
            /* The launcher must not keep pointing at a file that is gone. */
            if(current){
                st->replay_path[0] = 0;
                set_path_text(st);
                if(st->mode == LAUNCH_REPLAY) replay_load_file(st);
            }
        }
        if(err) g_error_free(err);
        g_object_unref(f);
    }
    free(rows);
    if(failed)
        message(d->dlg, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "pfemu - delete",
                "Not every recording could be moved to the Trash.\n\n"
                "The files are still there. A folder on a Windows drive or a"
                " network share has no Trash.");
    rl_scan(d);
    rl_fill(d, sel_path);
    rl_detail(d);
}

static void rl_on_changed(GtkTreeSelection *ts, gpointer p){
    (void)ts;
    rl_detail((RlState*)p);
}

static void rl_on_activated(GtkTreeView *v, GtkTreePath *path, GtkTreeViewColumn *c,
                            gpointer p){
    RlState *d = (RlState*)p;
    (void)v; (void)path; (void)c;
    if(gtk_widget_get_sensitive(d->replay)) gtk_dialog_response(GTK_DIALOG(d->dlg), RL_REPLAY);
}

static gboolean rl_on_key(GtkWidget *w, GdkEventKey *ev, gpointer p){
    (void)w;
    if(ev->keyval == GDK_KEY_Delete || ev->keyval == GDK_KEY_KP_Delete){
        rl_delete((RlState*)p);
        return TRUE;
    }
    return FALSE;
}

static void rl_on_delete(GtkButton *b, gpointer p){
    (void)b;
    rl_delete((RlState*)p);
}

static GtkFileFilter *pfr_filter(void){
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Pinball replays (*.pfr)");
    gtk_file_filter_add_pattern(f, "*.pfr");
    gtk_file_filter_add_pattern(f, "*.PFR");
    return f;
}

static GtkFileFilter *all_filter(void){
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "All files");
    gtk_file_filter_add_pattern(f, "*");
    return f;
}

/* Other file...: a recording kept anywhere. */
static int rl_other(RlState *d){
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Open a recording",
                                   GTK_WINDOW(d->dlg), GTK_FILE_CHOOSER_ACTION_OPEN,
                                   "_Open", "_Cancel");
    int ok = 0;
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), pfr_filter());
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), all_filter());
    if(d->st->replay_path[0] && strchr(d->st->replay_path, '/'))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(fc), d->st->replay_path);
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT){
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if(f){
            snprintf(d->picked, sizeof(d->picked), "%s", f);
            g_free(f);
            ok = 1;
        }
    }
    g_object_unref(fc);
    return ok;
}

static GtkTreeViewColumn *rl_column(RlState *d, const char *title, int col, int width,
                                    float xalign){
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c;
    g_object_set(r, "xalign", xalign, NULL);
    if(col == RC_SCORES || col == RC_FILE)
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_expand(c, col == RC_SCORES);
    gtk_tree_view_append_column(GTK_TREE_VIEW(d->view), c);
    return c;
}

/* Modal, like Details.  A recording picked for replay lands in the Session
 * group, switched to Replay. */
static void show_replays(LaunchState *st){
    RlState d;
    GtkWidget *box, *sw, *sw2, *row;
    int r;
    memset(&d, 0, sizeof(d));
    d.st = st;
    d.dlg = gtk_dialog_new_with_buttons("pfemu - replays", GTK_WINDOW(st->win),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        "Other file...", RL_OTHER,
                                        "Replay", RL_REPLAY,
                                        "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d.dlg), 900, 560);
    gtk_dialog_set_default_response(GTK_DIALOG(d.dlg), RL_REPLAY);
    d.replay = gtk_dialog_get_widget_for_response(GTK_DIALOG(d.dlg), RL_REPLAY);
    box = gtk_dialog_get_content_area(GTK_DIALOG(d.dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 8);

    d.store = gtk_list_store_new(RC_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    d.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d.store));
    g_object_unref(d.store);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(d.view), GTK_TREE_VIEW_GRID_LINES_BOTH);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(d.view)),
                                GTK_SELECTION_MULTIPLE);
    rl_column(&d, "Recording", RC_FILE, 230, 0.0f);
    rl_column(&d, "Recorded", RC_WHEN, 130, 0.0f);
    rl_column(&d, "Best 3-ball games", RC_SCORES, 230, 0.0f);
    rl_column(&d, "Length", RC_LENGTH, 64, 1.0f);
    rl_column(&d, "Status", RC_STATE, 170, 0.0f);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), d.view);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    d.detail = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(d.detail), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(d.detail), FALSE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d.detail), 4);
    sw2 = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw2), GTK_SHADOW_IN);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw2), 150);
    gtk_container_add(GTK_CONTAINER(sw2), d.detail);
    gtk_box_pack_start(GTK_BOX(box), sw2, FALSE, TRUE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    d.count = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d.count), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(d.count), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(row), d.count, TRUE, TRUE, 0);
    d.del = gtk_button_new_with_label("Delete");
    gtk_widget_set_tooltip_text(d.del, "Move the selected recordings to the Trash (Delete key)");
    gtk_box_pack_end(GTK_BOX(row), d.del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d.view)), "changed",
                     G_CALLBACK(rl_on_changed), &d);
    g_signal_connect(d.view, "row-activated", G_CALLBACK(rl_on_activated), &d);
    g_signal_connect(d.view, "key-press-event", G_CALLBACK(rl_on_key), &d);
    g_signal_connect(d.del, "clicked", G_CALLBACK(rl_on_delete), &d);

    rl_scan(&d);
    rl_fill(&d, st->replay_path);
    rl_detail(&d);
    gtk_widget_show_all(d.dlg);
    gtk_widget_grab_focus(d.view);
    st->replays = &d;
    for(;;){
        r = gtk_dialog_run(GTK_DIALOG(d.dlg));
        if(r == RL_OTHER){
            if(rl_other(&d)) break;
            continue;
        }
        if(r == RL_REPLAY){
            int i = rl_selected(&d);
            if(i < 0 || !gtk_widget_get_sensitive(d.replay)) continue;
            snprintf(d.picked, sizeof(d.picked), "%s", d.it[i].path);
        }
        break;
    }
    st->replays = NULL;
    gtk_widget_destroy(d.dlg);
    free(d.it);
    if(d.picked[0]){
        LaunchMode prev = st->mode;
        snprintf(st->replay_path, sizeof(st->replay_path), "%s", d.picked);
        st->path_custom = 1;
        if(st->mode != LAUNCH_REPLAY){
            st->mode = LAUNCH_REPLAY;
            apply_mode_ui(st, prev);   /* keeps the path: path_custom */
        }
        set_path_text(st);
        replay_load_file(st);
    }
}

/* Record: a target file, anywhere. */
static void pick_record_target(LaunchState *st){
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Record to",
                                   GTK_WINDOW(st->win), GTK_FILE_CHOOSER_ACTION_SAVE,
                                   "_Save", "_Cancel");
    char dir[1100];
    const char *b = base_name(st->replay_path);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), pfr_filter());
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), all_filter());
    /* Start where the current target is: sessions/ unless it was moved. */
    snprintf(dir, sizeof(dir), "%.*s", (int)(b - st->replay_path), st->replay_path);
    if(!dir[0]) snprintf(dir, sizeof(dir), ".");
    { char abs[PATH_MAX];
      if(realpath(dir, abs) || realpath(".", abs))
          gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), abs); }
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), b[0] ? b : "session.pfr");
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT){
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if(f){
            snprintf(st->replay_path, sizeof(st->replay_path), "%s", f);
            g_free(f);
            st->path_custom = 1;
            set_path_text(st);
            show_detection(st);
        }
    }
    g_object_unref(fc);
}

/* ------------------------------------------------------------- the game */
static void on_child_done(LaunchState *st){
    gtk_button_set_label(GTK_BUTTON(st->launch), "Launch");
    if(st->child_mode == LAUNCH_RECORD && st->child_path[0] && st->mode == LAUNCH_RECORD){
        /* The next recording gets a fresh name: a finished session is a
         * playthrough that cannot be reproduced, and aiming the next one
         * at the same file would overwrite it. */
        st->path_custom = 0;
        restore_session_path(st, 0);
    }
    show_detection(st);
    if(st->replays) rl_rescan(st->replays);
    gtk_window_present(GTK_WINDOW(st->win));
}

static void child_exited(GPid pid, gint status, gpointer p){
    LaunchState *st = (LaunchState*)p;
    g_spawn_close_pid(pid);
    if(WIFEXITED(status))
        fprintf(stderr, "[launcher] the game ended, exit code %d\n", WEXITSTATUS(status));
    else
        fprintf(stderr, "[launcher] the game ended, wait status %d\n", status);
    st->child_running = 0;
    on_child_done(st);
}

static int spawn_game(LaunchState *st){
    const RelResult *r = cur_inst(st);
    char exe[1024], table[8];
    const char *argv[16];
    int argc = 0;
    GPid pid;
    GError *err = NULL;
    DWORD len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    if(!r || st->child_running || len == 0 || len >= sizeof(exe)) return 0;
    argv[argc++] = exe;
    argv[argc++] = "-nolauncher";
    argv[argc++] = "-launched";
    argv[argc++] = "-d";
    argv[argc++] = r->dir;
    if(st->fullscreen) argv[argc++] = "-fullscreen";
    /* A replay carries its own start; run.c would only ignore this. */
    if(st->mode != LAUNCH_REPLAY && st->start_table){
        snprintf(table, sizeof(table), "%d", st->start_table);
        argv[argc++] = "-table";
        argv[argc++] = table;
    }
    if(st->mode == LAUNCH_RECORD){
        argv[argc++] = "-record";
        argv[argc++] = st->replay_path;
        if(st->ranked) argv[argc++] = "-ranked";
    } else if(st->mode == LAUNCH_REPLAY){
        argv[argc++] = "-replay";
        argv[argc++] = st->replay_path;
    }
    argv[argc] = NULL;
    /* Same working directory as this process: install directories are
     * stored relative to it. */
    if(!g_spawn_async(NULL, (gchar**)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &pid, &err)){
        char msg[600];
        snprintf(msg, sizeof(msg), "The game could not be started:\n\n%s",
                 err ? err->message : "?");
        if(err) g_error_free(err);
        message(st->win, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "pfemu", msg);
        return 0;
    }
    fprintf(stderr, "[launcher] started %s (%s)\n", r->dir, r->rel ? r->rel->id : "?");
    last_save(r);
    g_child_watch_add(pid, child_exited, st);
    st->child_running = 1;
    st->child_mode = st->mode;
    snprintf(st->child_path, sizeof(st->child_path), "%s",
             st->mode == LAUNCH_PLAY ? "" : st->replay_path);
    gtk_button_set_label(GTK_BUTTON(st->launch), "Running...");
    gtk_widget_set_sensitive(st->launch, FALSE);
    return 1;
}

static void on_launch(GtkButton *b, gpointer p){
    LaunchState *st = (LaunchState*)p;
    const RelResult *r = cur_inst(st);
    const char *dir;
    (void)b;
    if(st->child_running) return;
    if(st->mode == LAUNCH_REPLAY){
        /* No config writes in replay mode: the install's files are the
         * session's inputs, and replay promises never to write the real
         * overlay (REPLAY.md 3.3). */
        char why[256];
        if(!replay_check(st->rhdr_ok, &st->rhdr, st->replay_err, r, why, sizeof(why))){
            message(st->win, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "pfemu - cannot replay", why);
            return;
        }
        write_session_path(cur_game_dir(st), st->replay_path);
        spawn_game(st);
        return;
    }
    if(!r || !release_runnable(r)) return;
    if(st->mode == LAUNCH_RECORD && !st->replay_path[0]){
        message(st->win, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "pfemu - cannot record",
                "Pick a target .pfr file first.");
        return;
    }
    if(st->mode == LAUNCH_RECORD){
        /* A typed path may lack the extension - normalise so recordings
         * stay findable (the Replays window lists *.pfr). */
        const char *base = base_name(st->replay_path);
        if(!strchr(base, '.') && strlen(st->replay_path) + 4 < sizeof(st->replay_path)){
            strcat(st->replay_path, ".pfr");
            set_path_text(st);
        }
    }
    dir = r->dir;
    write_sound_cfg(dir, st->sound, st->quality);
    /* Everything the window owns lands in one file, in one write
     * (src/cfg.c).  Read first so a key this window does not show
     * survives. */
    { PfCfg c;
      cfg_read(dir, &c);
      c.volume = st->volume;
      c.bass = st->bass;
      c.treble = st->treble;
      c.oomph = st->oomph;
      c.headphone = st->headphone != 0;
      c.quality = st->quality;
      memcpy(c.options, st->cfg, 6);
      /* The trainer is incompatible with recording; main() refuses as a
       * backstop.  The session runs with it off. */
      c.trainer = (st->mode == LAUNCH_RECORD) ? 0 : (st->cheat_enable != 0);
      c.fullscreen = st->fullscreen != 0;
      c.start_table = st->start_table;
      if(st->mode == LAUNCH_RECORD && st->replay_path[0])
          snprintf(c.session, sizeof(c.session), "%s", st->replay_path);
      cfg_write(dir, &c); }
    spawn_game(st);
}

/* ------------------------------------------------------ widget handlers */
static void on_toggle(GtkToggleButton *b, gpointer p){
    LaunchState *st = (LaunchState*)p;
    GtkWidget *w = GTK_WIDGET(b);
    int on = gtk_toggle_button_get_active(b) ? 1 : 0;
    if(st->updating) return;
    if(w == st->w_sound) st->sound = on;
    else if(w == st->w_headphone) st->headphone = on;
    else if(w == st->w_trainer) st->cheat_enable = on;
    else if(w == st->w_fullscreen) st->fullscreen = on;
    else if(w == st->ranked_cb){ st->ranked = on; ranked_save(on); }
    else if(on && (w == st->mode_play || w == st->mode_record || w == st->mode_replay)){
        LaunchMode prev = st->mode;
        st->mode = (w == st->mode_record) ? LAUNCH_RECORD :
                   (w == st->mode_replay) ? LAUNCH_REPLAY : LAUNCH_PLAY;
        if(st->mode != prev) apply_mode_ui(st, prev);
    }
}

static void on_combo(GtkComboBox *cb, gpointer p){
    LaunchState *st = (LaunchState*)p;
    GtkWidget *w = GTK_WIDGET(cb);
    int i = gtk_combo_box_get_active(cb);
    int k;
    if(st->updating || i < 0) return;
    if(w == st->install){ st->sel = i; reload_for_dir(st); }
    else if(w == st->w_quality) st->quality = i;
    else if(w == st->w_bass) st->bass = eq_idx_to_db(i);
    else if(w == st->w_treble) st->treble = eq_idx_to_db(i);
    else if(w == st->w_oomph) st->oomph = oomph_idx_to_db(i);
    else if(w == st->w_table) st->start_table = i;
    else for(k = 0; k < 6; k++) if(w == st->w_opt[k]) st->cfg[k] = (uint8_t)i;
}

static void on_volume(GtkRange *r, gpointer p){
    LaunchState *st = (LaunchState*)p;
    if(st->updating) return;
    st->volume = (int)(gtk_range_get_value(r) + 0.5);
}

static gchar *on_volume_format(GtkScale *s, gdouble v, gpointer p){
    (void)s; (void)p;
    return g_strdup_printf("%d%%", (int)(v + 0.5));
}

static void on_path_changed(GtkEditable *e, gpointer p){
    LaunchState *st = (LaunchState*)p;
    (void)e;
    if(st->updating) return;
    snprintf(st->replay_path, sizeof(st->replay_path), "%s",
             gtk_entry_get_text(GTK_ENTRY(st->path)));
    st->path_custom = 1;
    if(st->mode == LAUNCH_REPLAY) replay_load_file(st);
    else show_detection(st);
}

static void on_browse(GtkButton *b, gpointer p){
    LaunchState *st = (LaunchState*)p;
    (void)b;
    if(st->mode == LAUNCH_RECORD) pick_record_target(st);
    else show_replays(st);
}

static void on_details(GtkButton *b, gpointer p){
    (void)b;
    show_details((LaunchState*)p);
}

static void on_quit(GtkButton *b, gpointer p){
    LaunchState *st = (LaunchState*)p;
    (void)b;
    gtk_window_close(GTK_WINDOW(st->win));
}

/* The game is its own process and keeps running; say so rather than let it
 * look as if closing the launcher had closed it. */
static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer p){
    LaunchState *st = (LaunchState*)p;
    (void)w; (void)ev;
    if(st->child_running &&
       !message(st->win, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "pfemu",
                "The game is still running. Close the launcher anyway? The game keeps running."))
        return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------- building */
static GtkWidget *combo(LaunchState *st, const char *const *items, int n, int sel){
    GtkWidget *c = gtk_combo_box_text_new();
    int i;
    for(i = 0; i < n; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), items[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), sel);
    g_signal_connect(c, "changed", G_CALLBACK(on_combo), st);
    return c;
}

static GtkWidget *check(LaunchState *st, const char *label, int on){
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on ? TRUE : FALSE);
    g_signal_connect(c, "toggled", G_CALLBACK(on_toggle), st);
    return c;
}

static GtkWidget *label(const char *text){
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    return l;
}

/* A titled group holding a grid, which is returned. */
static GtkWidget *group(GtkWidget *column, const char *title, int expand){
    GtkWidget *f = gtk_frame_new(title), *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_container_set_border_width(GTK_CONTAINER(g), 8);
    gtk_container_add(GTK_CONTAINER(f), g);
    gtk_box_pack_start(GTK_BOX(column), f, expand, TRUE, 0);
    return g;
}

static void build_window(LaunchState *st){
    GtkWidget *outer, *g, *cols, *left, *right, *row, *w;
    int i;
    st->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(st->win), "pfemu launcher");
    gtk_window_set_resizable(GTK_WINDOW(st->win), FALSE);
    gtk_window_set_position(GTK_WINDOW(st->win), GTK_WIN_POS_CENTER);
    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(st->win), outer);
    gtk_box_pack_start(GTK_BOX(outer), label("Pinball Fantasies"), FALSE, FALSE, 0);

    /* Game, across both columns: the installation (when there is a choice)
     * and what the detector made of it.  The files decide; Details
     * explains. */
    { GtkWidget *f = gtk_frame_new("Game");
      g = gtk_grid_new();
      gtk_grid_set_row_spacing(GTK_GRID(g), 6);
      gtk_grid_set_column_spacing(GTK_GRID(g), 8);
      gtk_container_set_border_width(GTK_CONTAINER(g), 8);
      gtk_container_add(GTK_CONTAINER(f), g);
      gtk_box_pack_start(GTK_BOX(outer), f, FALSE, TRUE, 0); }
    if(st->ninst > 1){
        GtkWidget *c = gtk_combo_box_text_new();
        for(i = 0; i < st->ninst; i++){
            char item[300];
            snprintf(item, sizeof(item), "%s  -  %s", st->inst[i].dir, st->inst[i].summary);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), item);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(c), st->sel);
        g_signal_connect(c, "changed", G_CALLBACK(on_combo), st);
        gtk_widget_set_hexpand(c, TRUE);
        /* A long summary must not widen the window: it ends in "...". */
        { GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(c));
          if(cells) g_object_set(cells->data, "ellipsize", PANGO_ELLIPSIZE_END,
                                 "width-chars", 40, NULL);
          g_list_free(cells); }
        st->install = c;
        gtk_grid_attach(GTK_GRID(g), label("Installation:"), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(g), c, 1, 0, 2, 1);
    }
    st->detected = label("");
    gtk_label_set_ellipsize(GTK_LABEL(st->detected), PANGO_ELLIPSIZE_END);
    /* Both limits, or the label's natural width is its whole text and a long
     * record target widens the window. */
    gtk_label_set_width_chars(GTK_LABEL(st->detected), 40);
    gtk_label_set_max_width_chars(GTK_LABEL(st->detected), 40);
    gtk_widget_set_hexpand(st->detected, TRUE);
    gtk_grid_attach(GTK_GRID(g), st->detected, 0, 1, 2, 1);
    st->details = gtk_button_new_with_label("Details");
    g_signal_connect(st->details, "clicked", G_CALLBACK(on_details), st);
    gtk_grid_attach(GTK_GRID(g), st->details, 2, 1, 1, 1);

    /* Two columns below it.  Left: what the game sounds like and the
     * intro's own options.  Right: how the next session starts and what it
     * records.  The shorter column's last group grows to end on one line. */
    cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(cols), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), cols, TRUE, TRUE, 0);
    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(cols), left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cols), right, TRUE, TRUE, 0);

    g = group(left, "Sound", FALSE);
    st->w_sound = check(st, "Sound on (SoundBlaster 220h / IRQ 7)", st->sound);
    gtk_grid_attach(GTK_GRID(g), st->w_sound, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(g), label("Quality:"), 0, 1, 1, 1);
    st->w_quality = combo(st, quality_labels, 5, st->quality);
    gtk_grid_attach(GTK_GRID(g), st->w_quality, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(g), label("Volume:"), 0, 2, 1, 1);
    /* Host gain only, applied where the sound leaves pfemu. */
    st->w_volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_digits(GTK_SCALE(st->w_volume), 0);
    gtk_scale_set_value_pos(GTK_SCALE(st->w_volume), GTK_POS_RIGHT);
    for(i = 0; i <= 100; i += 25)
        gtk_scale_add_mark(GTK_SCALE(st->w_volume), i, GTK_POS_BOTTOM, NULL);
    gtk_range_set_value(GTK_RANGE(st->w_volume), st->volume);
    gtk_range_set_increments(GTK_RANGE(st->w_volume), 1, 10);
    gtk_widget_set_hexpand(st->w_volume, TRUE);
    g_signal_connect(st->w_volume, "value-changed", G_CALLBACK(on_volume), st);
    g_signal_connect(st->w_volume, "format-value", G_CALLBACK(on_volume_format), st);
    gtk_grid_attach(GTK_GRID(g), st->w_volume, 1, 2, 1, 1);

    /* Enhancement: host DSP only (src/sound.c), downstream of -wav.  Flat
     * and Off are the original sound; like volume, never recorded. */
    g = group(left, "Audio enhancement", FALSE);
    gtk_grid_attach(GTK_GRID(g), label("Bass:"), 0, 0, 1, 1);
    st->w_bass = combo(st, eq_labels, 9, eq_db_to_idx(st->bass));
    gtk_grid_attach(GTK_GRID(g), st->w_bass, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), label("Treble:"), 2, 0, 1, 1);
    st->w_treble = combo(st, eq_labels, 9, eq_db_to_idx(st->treble));
    gtk_grid_attach(GTK_GRID(g), st->w_treble, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), label("Oomph:"), 0, 1, 1, 1);
    st->w_oomph = combo(st, oomph_labels, 5, oomph_db_to_idx(st->oomph));
    gtk_grid_attach(GTK_GRID(g), st->w_oomph, 1, 1, 1, 1);
    st->w_headphone = check(st, "Headphone mode", st->headphone);
    gtk_grid_attach(GTK_GRID(g), st->w_headphone, 2, 1, 2, 1);

    /* Game options, two to a row in PINBALL.CFG order: Balls|Angle,
     * Scrolling|Music, Resolution|Color. */
    g = group(left, "Game options", TRUE);
    for(i = 0; i < 6; i++){
        int row_i = i / 2, col = (i % 2) * 2;
        gtk_grid_attach(GTK_GRID(g), label(launch_opts[i].label), col, row_i, 1, 1);
        st->w_opt[i] = combo(st, launch_opts[i].values, launch_opts[i].n, st->cfg[i]);
        gtk_widget_set_hexpand(st->w_opt[i], TRUE);
        gtk_grid_attach(GTK_GRID(g), st->w_opt[i], col + 1, row_i, 1, 1);
    }

    /* Extras: the trainer, fullscreen, and where the session starts. */
    g = group(right, "Extras", FALSE);
    st->w_trainer = check(st, "Enable trainer", st->cheat_enable);
    gtk_grid_attach(GTK_GRID(g), st->w_trainer, 0, 0, 2, 1);
    st->w_fullscreen = check(st, "Start in fullscreen", st->fullscreen);
    gtk_grid_attach(GTK_GRID(g), st->w_fullscreen, 2, 0, 1, 1);
    /* Start at: skip the intro and the menu and boot straight into a
     * table (docs/EMULATOR.md).  Greyed out in replay mode: the .pfr
     * records how its session began and that has to win. */
    gtk_grid_attach(GTK_GRID(g), label("Start at:"), 0, 1, 1, 1);
    st->w_table = combo(st, table_labels, 5, st->start_table);
    gtk_widget_set_hexpand(st->w_table, TRUE);
    gtk_grid_attach(GTK_GRID(g), st->w_table, 1, 1, 2, 1);
    w = label("");
    gtk_label_set_markup(GTK_LABEL(w), "<small>Trainer: '1'-'3' toggle, arrows / 'Z' move"
                         " the ball. Alt+Enter toggles fullscreen.</small>");
    gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(w), 50);
    gtk_grid_attach(GTK_GRID(g), w, 0, 2, 3, 1);

    /* Session record / replay (docs/REPLAY.md section 4). */
    g = group(right, "Session", TRUE);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    st->mode_play = gtk_radio_button_new_with_label(NULL, "Play");
    st->mode_record = gtk_radio_button_new_with_label_from_widget(
                          GTK_RADIO_BUTTON(st->mode_play), "Record");
    st->mode_replay = gtk_radio_button_new_with_label_from_widget(
                          GTK_RADIO_BUTTON(st->mode_play), "Replay");
    gtk_box_pack_start(GTK_BOX(row), st->mode_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->mode_record, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), st->mode_replay, FALSE, FALSE, 0);
    /* Ranked: record against the canonical state (docs/REPLAY.md, Ranked
     * recordings), which is what the leaderboard accepts.  Off keeps the
     * install's own high-score tables. */
    st->ranked_cb = check(st, "Ranked", st->ranked);
    gtk_box_pack_end(GTK_BOX(row), st->ranked_cb, FALSE, FALSE, 0);
    g_signal_connect(st->mode_play, "toggled", G_CALLBACK(on_toggle), st);
    g_signal_connect(st->mode_record, "toggled", G_CALLBACK(on_toggle), st);
    g_signal_connect(st->mode_replay, "toggled", G_CALLBACK(on_toggle), st);
    gtk_widget_set_hexpand(row, TRUE);
    gtk_grid_attach(GTK_GRID(g), row, 0, 0, 3, 1);
    st->path_label = label("File:");
    gtk_grid_attach(GTK_GRID(g), st->path_label, 0, 1, 1, 1);
    st->path = gtk_entry_new();
    gtk_widget_set_hexpand(st->path, TRUE);
    gtk_entry_set_text(GTK_ENTRY(st->path), st->replay_path);
    g_signal_connect(st->path, "changed", G_CALLBACK(on_path_changed), st);
    gtk_grid_attach(GTK_GRID(g), st->path, 1, 1, 1, 1);
    st->browse = gtk_button_new_with_label("Replays...");
    g_signal_connect(st->browse, "clicked", G_CALLBACK(on_browse), st);
    gtk_grid_attach(GTK_GRID(g), st->browse, 2, 1, 1, 1);

    /* Bottom line: which pfemu this is (the id -verify carries), then
     * Launch and Quit. */
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    w = label("Build " PFEMU_BUILD);
    gtk_style_context_add_class(gtk_widget_get_style_context(w), "dim-label");
    gtk_box_pack_start(GTK_BOX(row), w, TRUE, TRUE, 0);
    w = gtk_button_new_with_label("Quit");
    g_signal_connect(w, "clicked", G_CALLBACK(on_quit), st);
    gtk_box_pack_end(GTK_BOX(row), w, FALSE, FALSE, 0);
    st->launch = gtk_button_new_with_label("Launch");
    gtk_style_context_add_class(gtk_widget_get_style_context(st->launch),
                                "suggested-action");
    gtk_widget_set_can_default(st->launch, TRUE);
    g_signal_connect(st->launch, "clicked", G_CALLBACK(on_launch), st);
    gtk_box_pack_end(GTK_BOX(row), st->launch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    g_signal_connect(st->win, "delete-event", G_CALLBACK(on_delete), st);
    g_signal_connect(st->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
}

/* ---------------------------------------------------------------- start */
/* GOG.com's Deluxe: src/gog.c finds game.gog and decides whether to offer
 * the import; asking is this window's.  1 when GOG/ was just filled. */
static int gog_offer(const RelResult *inst, int ninst){
    char image[1024], msg[1400], err[800];
    if(!gog_candidate(inst, ninst, image, sizeof(image))) return 0;
    gog_offer_text(msg, sizeof(msg), image);
    if(!message(NULL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                "pfemu - GOG version found", msg)){
        gog_decline(image);
        return 0;
    }
    if(cdimage_import(image, GOG_DIR, err, sizeof(err)) != 0){
        snprintf(msg, sizeof(msg), "The GOG version could not be imported:\n\n%s", err);
        fprintf(stderr, "[gog] %s\n", msg);
        message(NULL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "pfemu - GOG import", msg);
        return 0;
    }
    fprintf(stderr, "[gog] imported %s into %s: %s\n", image, GOG_DIR, err);
    return 1;
}

/* The launcher, for as long as it is open.  Launch starts the game as a
 * child process and the window stays; the process exits when the launcher
 * is closed. */
int run_launcher(void){
    static LaunchState st;
    char icon[1100];
    int imported, i;
    /* The game window's own choice, remade by each game from scratch: a
     * child that inherited it would think the user had asked for it and
     * lose its fallback (src/host_sdl.c, plat_init). */
    if(plat_sdl_env_ours()) g_unsetenv("SDL_VIDEODRIVER");
    /* The application id the desktop matches against the menu entry
     * (res/linux/install-desktop-entry.sh, StartupWMClass=pfemu). */
    g_set_prgname("pfemu");
    gdk_set_program_class("pfemu");
    if(!gtk_init_check(NULL, NULL)){
        fprintf(stderr, "[pfemu] cannot open the launcher: no display.\n"
                        "    pfemu -nolauncher -d <folder> starts a game without it;\n"
                        "    pfemu-headless runs without a display.\n");
        return 1;
    }
    beside_exe(icon, sizeof(icon), "pfemu.png");
    gtk_window_set_default_icon_from_file(icon, NULL);   /* none is fine */

    memset(&st, 0, sizeof(st));
    st.ranked = ranked_load();
    /* Every directory that holds an INTRO.PRG, identified by content.  GAME
     * comes first when it exists, then the rest alphabetically. */
    st.ninst = release_scan(st.inst, MAX_INSTALLS);
    /* Before the window exists: its layout depends on how many there are. */
    imported = gog_offer(st.inst, st.ninst);
    if(imported) st.ninst = release_scan(st.inst, MAX_INSTALLS);
    /* One that can run, else the first; the one launched last wins when it
     * is still there, and a copy just imported wins over both. */
    for(i = 0; i < st.ninst; i++)
        if(release_runnable(&st.inst[i])){ st.sel = i; break; }
    i = last_pick(st.inst, st.ninst);
    if(i >= 0) st.sel = i;
    if(imported)
        for(i = 0; i < st.ninst; i++)
            if(!_stricmp(st.inst[i].dir, GOG_DIR)){ st.sel = i; break; }

    build_window(&st);
    reload_for_dir(&st);
    apply_mode_ui(&st, LAUNCH_PLAY);
    gtk_widget_show_all(st.win);
    gtk_widget_grab_default(st.launch);
    gtk_widget_grab_focus(st.launch);
    gtk_main();
    return 0;
}

#endif /* !_WIN32 */
