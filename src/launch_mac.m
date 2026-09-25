/* AppKit launcher: the Mac face of the launcher.
 *
 * The same window as src/launch_gtk.c, on AppKit instead of GTK, and the
 * same rules, because both call src/launchcore.c for every decision that
 * matters: which replay may run against which installation, what Details
 * says, where a recording goes, what the leaderboard's answers mean.
 * AppKit is part of every Mac and this file is plain Objective-C, so the
 * Command Line Tools' clang builds it; there is no Xcode project, no nib and
 * no storyboard.  The Leaderboard group talks to pfemu-web through
 * src/online.c, which is the system's libcurl and the Keychain here.
 *
 * Differences from the GTK launcher, each on purpose:
 *
 *   - pfemu.app works in ~/Library/Application Support/pfemu, not beside
 *     the program (mac_app_home(), src/posix.c), so Show Folder opens that
 *     folder in the Finder: it is hidden there otherwise.
 *   - The window position is AppKit's own (setFrameAutosaveName:).
 *   - Setting a control from code sends no action in AppKit, so there is
 *     no "updating" guard: every action here is the user's.
 *   - A finished result while the launcher is in the background bounces
 *     its Dock icon once, where GTK marks the window urgent.
 *
 * Requests run on a GCD queue and come back to the main thread through
 * CFRunLoopPerformBlock() in the common modes, so an answer lands while a
 * modal window (Replays, the login) is open, and a window an answer opens
 * does not hold up the answers after it.  No window waits on the network.
 *
 * The game still runs as a child process, this program again with
 * -nolauncher -launched, for the reason src/launch.c gives: nothing from one
 * session may reach the next recording.  The child never touches AppKit
 * itself; SDL does, in the child.
 */
#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "launchcore.h"
#include "build.h"

/* src/posix.c.  compat.h is not included here: its Win32 names (BOOL
 * among them) collide with Objective-C's. */
uint32_t GetModuleFileNameA(void *module, char *out, uint32_t n);

extern char **environ;

#define MAX_INSTALLS 8
#define RL_DIR       "sessions"

/* A C string as an NSString, never nil: a path that is not UTF-8 must not
 * take a whole window with it. */
static NSString *S(const char *s){
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    if(!r && s) r = [NSString stringWithCString:s encoding:NSISOLatin1StringEncoding];
    return r ? r : @"";
}

static void bring_to_front(void){
    if(@available(macOS 14.0, *)) [NSApp activate];
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}

/* An alert with one button, or with a choice: 1 when the first was taken. */
static int alert(NSAlertStyle style, NSString *head, NSString *text,
                 NSString *yes, NSString *no){
    NSAlert *a = [[NSAlert alloc] init];
    a.alertStyle = style;
    a.messageText = head;
    a.informativeText = text;
    [a addButtonWithTitle:yes ? yes : @"OK"];
    if(no) [a addButtonWithTitle:no];
    return [a runModal] == NSAlertFirstButtonReturn;
}

/* End the innermost modal window.  stopModal only takes effect with the
 * next event, and an answer from the network is not one. */
static void end_modal(NSModalResponse code){
    [NSApp stopModalWithCode:code];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint modifierFlags:0
                                       timestamp:0 windowNumber:0 context:nil
                                         subtype:0 data1:0 data2:0]
             atStart:NO];
}

/* Two paths naming the same file.  Record mode names files relative, a
 * picked one is absolute. */
static int same_file(const char *a, const char *b){
    char fa[PATH_MAX], fb[PATH_MAX];
    if(!a[0] || !b[0]) return 0;
    if(realpath(a, fa) && realpath(b, fb)) return !strcmp(fa, fb);
    return !strcmp(a, b);
}

/* Colours for the tables: green counts, grey waits or is only pfemu's own
 * count.  The system's, so they read in light and dark mode. */
static NSColor *kind_color(int kind){
    return kind == SL_COUNTS ? NSColor.systemGreenColor :
           kind == SL_PENDING ? NSColor.secondaryLabelColor : NSColor.labelColor;
}

/* ----------------------------------------------------------- leaderboard
 * Log in, submit a ranked recording, follow it until the service has an
 * answer (pfemu-web/docs/API.md), as in src/launch.c. */
typedef enum {
    NJ_LOGIN, NJ_REGISTER, NJ_LOGOUT, NJ_SUBMIT, NJ_POLL, NJ_LIST, NJ_LIST_SHOW,
    NJ_ME, NJ_RL_LIST
} NetKind;
typedef struct {
    NetKind kind;
    unsigned gen;            /* NJ_LOGIN, NJ_REGISTER: the window that asked */
    char server[256], token[160];
    char method[8], path[96], ctype[40];
    char *data;
    size_t n;
    char file[512];          /* NJ_SUBMIT: the recording sent; NJ_ME: checked */
    HttpResp r;
} NetJob;

static void net_free(NetJob *j){
    if(!j) return;
    online_resp_free(&j->r);
    if(j->data){ secure_wipe(j->data, j->n); free(j->data); }
    secure_wipe(j->token, sizeof(j->token));
    free(j);
}

/* The Replays window's rows (as in src/launch_gtk.c). */
typedef struct {
    char path[512];          /* sessions/<name>.pfr */
    time_t mtime;
    long long size;
    ReplayHeader hd;
    int hd_ok;
    long long best[5];       /* read_claims() */
    int have_games;
    uint8_t sha[32];         /* what the server keeps an upload under */
    int have_sha;
    long sub_s, sub_e;       /* its object in rlSubs, -1 when none */
    int kind;                /* SL_*: how its Leaderboard cell is coloured */
    int verified;            /* its scores cell is the server's, not ours */
    int uploading;           /* its Submit was clicked, no answer yet */
} RlItem;
typedef struct { char file[300], when[32], scores[SL_CELL], len[16], board[SL_CELL]; } RlCells;
enum { RC_FILE, RC_WHEN, RC_SCORES, RC_LENGTH, RC_BOARD, RC_COLS };

/* Delete (and Forward Delete) in the Replays table move the selection to
 * the Trash, as the button does. */
@interface PFTable : NSTableView
@property (nonatomic, weak) id deleteTarget;
@property (nonatomic) SEL deleteAction;
@end
@implementation PFTable
- (void)keyDown:(NSEvent *)e {
    unichar c = e.charactersIgnoringModifiers.length ?
                [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
    if((c == NSDeleteCharacter || c == NSDeleteFunctionKey) && self.deleteTarget){
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [self.deleteTarget performSelector:self.deleteAction withObject:self];
#pragma clang diagnostic pop
        return;
    }
    [super keyDown:e];
}
@end

@interface PFLauncher : NSObject <NSApplicationDelegate, NSWindowDelegate,
                                  NSTextFieldDelegate, NSTableViewDataSource,
                                  NSTableViewDelegate>
@end

@implementation PFLauncher {
    int sound, quality, volume, bass, treble, oomph, headphone;
    uint8_t cfg[6];
    int cheatEnable, fullscreen, startTable;
    RelResult inst[MAX_INSTALLS];
    int ninst, sel;
    /* Session record / replay, as in src/launch.c. */
    LaunchMode mode;
    char replayPath[512];
    int pathCustom;          /* the user typed or picked a path; keep it */
    ReplayHeader rhdr;
    int rhdrOk;
    char replayErr[256];
    /* The running game, if any. */
    int childRunning;
    LaunchMode childMode;
    char childPath[512];     /* what that session records or replays */
    pid_t childPid;
    dispatch_source_t childWatch;
    int quitConfirmed;
    /* The leaderboard (src/online.c, pfemu-web/docs/API.md). */
    OnlineCfg online;        /* server, login, and the Ranked checkbox */
    char lastRec[512];       /* the last finished recording, until submitted */
    int submitting;          /* an upload (or the check before it) is out */
    int polling;             /* a poll is out */
    long long subId;         /* the submission being followed, 0 if none */
    int subPending;          /* ...and it is not done yet */
    NSTimeInterval lastList; /* when the list was last asked for */
    char subLine[256];       /* what the status line says about it */
    NSTimer *pollTimer;      /* while following */
    int sessionOnly;         /* logged in, but the Keychain would not keep it */
    /* The main window. */
    NSWindow *win;
    NSPopUpButton *install;
    NSTextField *detected;
    NSButton *details;
    NSButton *wSound, *wHeadphone, *wTrainer, *wFullscreen;
    NSPopUpButton *wQuality, *wBass, *wTreble, *wOomph, *wTable;
    NSPopUpButton *wOpt[6];
    NSSlider *wVolume;
    NSTextField *wVolumeText;
    NSButton *modePlay, *modeRecord, *modeReplay, *rankedCb;
    NSTextField *pathLabel, *path;
    NSButton *browse, *launch;
    NSTextField *account, *subfile, *substate;
    NSButton *loginBtn, *sublistBtn, *submitBtn, *meLink;
    /* Details. */
    NSWindow *detailsWin;
    NSString *detailsText;
    NSButton *detailsCopy;
    /* The login window, while it is open. */
    NSWindow *loginWin;
    NSTextField *lgUser, *lgEmail, *lgMsg;
    NSSecureTextField *lgPass;
    NSButton *lgLogin, *lgRegister;
    unsigned loginGen, loginOpenGen;
    int loginOk;
    /* The Submissions window, while it is open. */
    NSWindow *slWin;
    NSTableView *slTable;
    char (*slCells)[SL_COLS][SL_CELL];
    int *slKind, slN;
    NSString *slTsv;
    NSButton *slCopy;
    /* The Replays window, while it is open. */
    NSWindow *rlWin;
    PFTable *rlTable;
    NSTextView *rlDetailView;
    NSTextField *rlCount;
    NSButton *rlDel, *rlSubmitBtn, *rlReplay;
    RlItem *rlIt;
    RlCells *rlCellsArr;
    int rlN;
    char *rlSubs;            /* the last GET /api/v1/submissions body */
    size_t rlNsubs;
    int rlListing;           /* that request is out */
    char rlPicked[512];
}

/* --------------------------------------------------------------- helpers */
- (const RelResult *)cur {
    return (ninst && sel >= 0 && sel < ninst) ? &inst[sel] : NULL;
}

- (const char *)gameDir {
    const RelResult *r = [self cur];
    return r ? r->dir : "";
}

static void set_check(NSButton *b, int on){
    b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
}

/* A status line, with the whole text as its tooltip: it ends in an
 * ellipsis where the window does. */
static void set_line(NSTextField *l, const char *text){
    l.stringValue = S(text);
    l.toolTip = text[0] ? S(text) : nil;
}

- (void)setPathText {
    path.stringValue = S(replayPath);
}

- (void)setDetected:(const char *)text {
    set_line(detected, text);
}

/* --------------------------------------------------- mode and detection */
/* Auto-restore on replay-file load (REPLAY.md 4.1): scan the installs and
 * pick one that IS the recording - exact vector match first (preferring the
 * recorded dir if it still matches), else the first runnable install of the
 * same release, else no auto-launch.  Never applies one release's layout to
 * another: that falls out of same_vector() refusing. */
- (void)replayAutorestore {
    int k, same = -1;
    if(!rhdrOk || !rhdr.trainer_off) return;
    for(k = 0; k < ninst; k++){
        const RelResult *r = &inst[k];
        if(!release_runnable(r) || !same_vector(&rhdr, r)) continue;
        if(same < 0) same = k;
        if(!_stricmp(r->dir, rhdr.dir_hint)){ same = k; break; }
    }
    if(same < 0)
        for(k = 0; k < ninst; k++){
            const RelResult *r = &inst[k];
            if(release_runnable(r) && r->rel && !_stricmp(rhdr.release_id, r->rel->id)){
                same = k;
                break;
            }
        }
    /* No match: no auto-launch.  showDetection leaves Launch disabled and
     * Details carries the recorded-vs-found report. */
    if(same < 0) return;
    sel = same;
    if(install) [install selectItemAtIndex:sel];
    [self reloadForDir];
}

/* Push one replay file through parse + restore + detection-line update.
 * Silent (no alerts): typing a half-finished path must not nag. */
- (void)replayLoadFile {
    if(!replayPath[0]){
        rhdrOk = 0;
        snprintf(replayErr, sizeof(replayErr), "Pick a .pfr replay file.");
    } else if(replay_read_header(replayPath, &rhdr) != 0){
        rhdrOk = 0;
        snprintf(replayErr, sizeof(replayErr),
                 "Not a readable replay file: %.200s", replayPath);
    } else {
        rhdrOk = 1;
        replayErr[0] = 0;
        [self replayAutorestore];
    }
    [self showDetection];
    /* A picked ranked recording can be submitted from Replay mode. */
    [self updateOnlineUI];
}

/* Fill the path field on mode entry: a fresh record target in record mode,
 * else the install's last-used session file, else empty.  Never clobbers a
 * user-typed or picked path.  Install switches keep the current path,
 * except in record mode, which re-targets to the new install. */
- (void)restoreSessionPath:(int)installSwitch {
    char saved[512];
    if(pathCustom) return;
    if(installSwitch && mode == LAUNCH_REPLAY) return;
    read_session_path([self gameDir], saved, sizeof(saved));
    if(mode == LAUNCH_RECORD)
        launch_record_path([self gameDir], replayPath, sizeof(replayPath));
    else if(saved[0]) snprintf(replayPath, sizeof(replayPath), "%s", saved);
    else replayPath[0] = 0;
    [self setPathText];
    if(mode == LAUNCH_REPLAY) [self replayLoadFile];
}

/* Mode switching: the trainer checkbox is greyed out in record and replay
 * modes (REPLAY.md 4), and entering those modes unchecks it; main() refuses
 * either mode with it on as a backstop.  Ranked only means something while
 * recording. */
- (void)applyModeUI:(LaunchMode)prev {
    int rec = (mode != LAUNCH_PLAY);
    set_check(modePlay, mode == LAUNCH_PLAY);
    set_check(modeRecord, mode == LAUNCH_RECORD);
    set_check(modeReplay, mode == LAUNCH_REPLAY);
    /* The .pfr records how its session began - menu or a table - and that
     * has to win, so replay owns this control. */
    wTable.enabled = mode != LAUNCH_REPLAY;
    path.enabled = rec;
    pathLabel.textColor = rec ? NSColor.labelColor : NSColor.disabledControlTextColor;
    /* Record picks a target file; otherwise the button is the Replays
     * window, which lists the recordings and switches to Replay for one. */
    browse.title = mode == LAUNCH_RECORD ? @"Browse…" : @"Replays…";
    /* Replay showed the file's settings; the installation's come back. */
    if(prev == LAUNCH_REPLAY && mode != LAUNCH_REPLAY) [self reloadForDir];
    if(rec){
        cheatEnable = 0;
        set_check(wTrainer, 0);
        wTrainer.enabled = NO;
        [self restoreSessionPath:0];
    } else {
        wTrainer.enabled = YES;
        cheatEnable = read_trainer_cfg([self gameDir]);
        set_check(wTrainer, cheatEnable);
    }
    [self showDetection];
    [self updateOnlineUI];
}

/* The read-only line saying what the detector concluded, and whether
 * Launch may be pressed: only for a release that was actually recognised,
 * and in replay mode only for a file that matches it. */
- (void)showDetection {
    const RelResult *r = [self cur];
    char line[1400], cwd[1024];
    int can = 0, i, hasOpts;
    if(!r){
        if(!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
        snprintf(line, sizeof(line), "No game found in %s. Show Folder opens it: put"
                 " one release's files in a folder there, GAME for example.", cwd);
    }
    if(mode == LAUNCH_REPLAY){
        char why[256];
        if(replay_check(rhdrOk, &rhdr, replayErr, r, why, sizeof(why))){
            snprintf(line, sizeof(line), "Replay ready: %s", rhdr.summary);
            can = 1;
        } else {
            snprintf(line, sizeof(line), "Replay: %s", why);
        }
        details.enabled = YES;
    } else if(mode == LAUNCH_RECORD){
        if(r && release_runnable(r) && replayPath[0])
            snprintf(line, sizeof(line), "Record %.120s -> %.400s", r->summary, replayPath);
        else if(r && release_runnable(r))
            snprintf(line, sizeof(line), "Record %s (pick a target file)", r->summary);
        else if(r)
            snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
        can = r && release_runnable(r) && replayPath[0];
        details.enabled = r != NULL;
    } else {
        if(r && release_runnable(r)) snprintf(line, sizeof(line), "Detected: %s", r->summary);
        else if(r) snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
        can = r && release_runnable(r);
        details.enabled = r != NULL;
    }
    [self setDetected:line];
    /* The six game options are the intro's own PINBALL.CFG structure, which
     * only a recognised release has a known address for: greyed otherwise. */
    hasOpts = r && release_runnable(r);
    for(i = 0; i < 6; i++) wOpt[i].enabled = hasOpts;
    /* One game at a time: it owns the audio device and the install's
     * PFEMU-STATE/ while it runs. */
    launch.enabled = can && !childRunning;
}

/* Every per-install setting for the selected directory, into the controls:
 * at startup and whenever the installation changes, so one install's
 * settings are never applied to another. */
- (void)reloadForDir {
    const char *dir = [self gameDir];
    PfCfg c;
    int i;
    cfg_read(dir, &c);
    sound = read_sound_is_sb(dir);
    quality = read_sound_quality(dir);   /* SOUND.CFG wins over c.quality */
    volume = c.volume;
    bass = c.bass;
    treble = c.treble;
    oomph = c.oomph;
    headphone = c.headphone;
    memcpy(cfg, c.options, 6);
    cheatEnable = c.trainer;
    fullscreen = c.fullscreen;
    startTable = c.start_table;
    if(mode == LAUNCH_REPLAY && rhdrOk){
        /* Replay shows the FILE's session, not the install's settings:
         * sound, quality, the six options and fullscreen are what the run
         * will use.  Display only - replay mode writes no configs. */
        sound = rhdr.sound ? 1 : 0;
        quality = rhdr.quality;
        if(quality < 0) quality = 0;
        if(quality > 4) quality = 4;
        for(i = 0; i < 6; i++){
            int v = rhdr.options[i];
            if(v < 0) v = 0;
            if(v >= launch_opts[i].n) v = launch_opts[i].n - 1;
            cfg[i] = (uint8_t)v;
        }
        fullscreen = rhdr.fullscreen ? 1 : 0;
    }
    set_check(wSound, sound);
    [wQuality selectItemAtIndex:quality];
    wVolume.intValue = volume;
    wVolumeText.stringValue = [NSString stringWithFormat:@"%d%%", volume];
    [wBass selectItemAtIndex:eq_db_to_idx(bass)];
    [wTreble selectItemAtIndex:eq_db_to_idx(treble)];
    [wOomph selectItemAtIndex:oomph_db_to_idx(oomph)];
    set_check(wHeadphone, headphone);
    for(i = 0; i < 6; i++) [wOpt[i] selectItemAtIndex:cfg[i]];
    set_check(wFullscreen, fullscreen);
    [wTable selectItemAtIndex:startTable];
    if(mode != LAUNCH_PLAY){
        /* The install switch must not resurrect the trainer behind the
         * mode's back: it stays unchecked and greyed until Play returns. */
        cheatEnable = 0;
        wTrainer.enabled = NO;
        if(mode == LAUNCH_RECORD && !pathCustom)
            [self restoreSessionPath:1];
    }
    set_check(wTrainer, cheatEnable);
    [self showDetection];
}

/* ------------------------------------------------------- small windows */
/* A titled window for a modal job, closed by its buttons or its close box
 * (windowWillClose:, which ends the modal loop). */
- (NSWindow *)panel:(NSString *)title size:(NSSize)size resizable:(int)resizable {
    NSWindowStyleMask m = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
    NSWindow *w;
    if(resizable) m |= NSWindowStyleMaskResizable;
    w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                    styleMask:m backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;
    w.title = title;
    w.delegate = self;
    return w;
}

/* A read-only monospace text in a scroll view. */
static NSScrollView *mono_text(NSTextView **out){
    NSScrollView *sv = [NSTextView scrollableTextView];
    NSTextView *tv = (NSTextView *)sv.documentView;
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.borderType = NSBezelBorder;
    tv.editable = NO;
    tv.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    tv.textContainerInset = NSMakeSize(4, 4);
    if(out) *out = tv;
    return sv;
}

static void copy_text(NSString *s){
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
}

/* --------------------------------------------------------- details window */
/* A monospace, scrollable, resizable report with Copy (src/launch.c has the
 * story of why it is not an alert). */
- (void)showDetails:(id)sender {
    static char text[DET_MAX];
    NSScrollView *sv;
    NSTextView *tv;
    NSButton *close;
    NSView *cv;
    details_report(text, sizeof(text), mode, replayPath, rhdrOk, &rhdr, replayErr,
                   [self cur]);
    detailsText = S(text);
    detailsWin = [self panel:@"pfemu - details" size:NSMakeSize(780, 540) resizable:1];
    cv = detailsWin.contentView;
    sv = mono_text(&tv);
    tv.string = detailsText;
    detailsCopy = [NSButton buttonWithTitle:@"Copy" target:self action:@selector(copyDetails:)];
    detailsCopy.translatesAutoresizingMaskIntoConstraints = NO;
    close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(closeModal:)];
    close.translatesAutoresizingMaskIntoConstraints = NO;
    close.keyEquivalent = @"\r";
    [cv addSubview:sv];
    [cv addSubview:detailsCopy];
    [cv addSubview:close];
    [NSLayoutConstraint activateConstraints:@[
        [sv.topAnchor constraintEqualToAnchor:cv.topAnchor constant:12],
        [sv.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:12],
        [sv.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-12],
        [close.topAnchor constraintEqualToAnchor:sv.bottomAnchor constant:12],
        [close.trailingAnchor constraintEqualToAnchor:sv.trailingAnchor],
        [close.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor constant:-12],
        [detailsCopy.firstBaselineAnchor constraintEqualToAnchor:close.firstBaselineAnchor],
        [detailsCopy.trailingAnchor constraintEqualToAnchor:close.leadingAnchor constant:-8],
    ]];
    [detailsWin center];
    [NSApp runModalForWindow:detailsWin];
    [detailsWin orderOut:nil];
    detailsWin = nil;
    detailsCopy = nil;
}

- (void)copyDetails:(id)sender {
    copy_text(detailsText);
    detailsCopy.title = @"Copied";
}

- (void)closeModal:(id)sender {
    end_modal(NSModalResponseCancel);
}

/* ------------------------------------------------------------ file panels */
static UTType *pfr_type(void){
    return [UTType typeWithFilenameExtension:@"pfr"];
}

/* Record: a target file, anywhere.  The panel asks before it overwrites. */
- (void)pickRecordTarget {
    NSSavePanel *p = [NSSavePanel savePanel];
    UTType *t = pfr_type();
    char dir[1100], abs[PATH_MAX];
    const char *b = base_name(replayPath);
    p.title = @"Record to";
    if(t) p.allowedContentTypes = @[t];
    /* Start where the current target is: sessions/ unless it was moved. */
    snprintf(dir, sizeof(dir), "%.*s", (int)(b - replayPath), replayPath);
    if(!dir[0]) snprintf(dir, sizeof(dir), ".");
    if(realpath(dir, abs) || realpath(".", abs))
        p.directoryURL = [NSURL fileURLWithPath:S(abs) isDirectory:YES];
    p.nameFieldStringValue = S(b[0] ? b : "session.pfr");
    if([p runModal] == NSModalResponseOK && p.URL){
        snprintf(replayPath, sizeof(replayPath), "%s", p.URL.fileSystemRepresentation);
        pathCustom = 1;
        [self setPathText];
        [self showDetection];
    }
}

/* Other File...: a recording kept anywhere.  1 with its path in out. */
- (int)openRecording:(char *)out size:(size_t)n {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    UTType *t = pfr_type();
    char dir[1100], abs[PATH_MAX];
    const char *b = base_name(replayPath);
    p.title = @"Open a recording";
    p.canChooseFiles = YES;
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    if(t) p.allowedContentTypes = @[t];
    snprintf(dir, sizeof(dir), "%.*s", (int)(b - replayPath), replayPath);
    if(!dir[0]) snprintf(dir, sizeof(dir), RL_DIR);
    if(realpath(dir, abs) || realpath(".", abs))
        p.directoryURL = [NSURL fileURLWithPath:S(abs) isDirectory:YES];
    if([p runModal] != NSModalResponseOK || !p.URL) return 0;
    snprintf(out, n, "%s", p.URL.fileSystemRepresentation);
    return 1;
}

/* ------------------------------------------------------------- the network */
- (NetJob *)netNew:(NetKind)k method:(const char *)method path:(const char *)p {
    NetJob *j = (NetJob*)calloc(1, sizeof(*j));
    if(!j) return NULL;
    j->kind = k;
    snprintf(j->server, sizeof(j->server), "%s", online.server);
    snprintf(j->token, sizeof(j->token), "%s", online.token);
    snprintf(j->method, sizeof(j->method), "%s", method);
    snprintf(j->path, sizeof(j->path), "%s", p);
    return j;
}

/* The exchange on a worker, the answer on the main thread. */
- (int)netStart:(NetJob *)j {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        online_http(j->server, j->method, j->path, j->token, j->ctype, j->data, j->n, &j->r);
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            [self netDone:j];
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    return 1;
}

/* The recording Submit would send: in Replay mode the picked file, else the
 * last recording that finished.  Only a complete ranked one qualifies, and
 * why names the reason when the picked file does not. */
- (const char *)submitCandidate:(const char **)why {
    *why = NULL;
    if(mode == LAUNCH_REPLAY){
        if(!rhdrOk || !replayPath[0]) return NULL;
        if(!rhdr.state[0]){
            *why = "Recorded without Ranked, so it cannot be submitted.";
            return NULL;
        }
        if(!rhdr.have_end){ *why = "This recording is incomplete."; return NULL; }
        return replayPath;
    }
    return lastRec[0] ? lastRec : NULL;
}

/* Every leaderboard control, from the state alone. */
- (void)updateOnlineUI {
    const char *why, *cand = [self submitCandidate:&why];
    int logged = online.token[0] != 0;
    char line[320];
    rankedCb.enabled = mode == LAUNCH_RECORD;
    if(!account) return;
    if(logged) snprintf(line, sizeof(line), "Logged in as %s", online.username);
    else snprintf(line, sizeof(line), "Not logged in");
    set_line(account, line);
    loginBtn.title = logged ? @"Log Out" : @"Log In…";
    /* The website keeps its own login, so the account page would work
     * either way; offering it is only sensible once there is an account. */
    meLink.hidden = !logged;
    sublistBtn.enabled = logged;
    submitBtn.enabled = logged && cand && !submitting;
    if(submitting) snprintf(line, sizeof(line), "Uploading...");
    else if(cand && logged) snprintf(line, sizeof(line), "Ready to submit: %s", base_name(cand));
    else if(cand) snprintf(line, sizeof(line), "Log in to submit %s", base_name(cand));
    else if(why) snprintf(line, sizeof(line), "%s", why);
    else if(!logged) snprintf(line, sizeof(line), "Log in to submit ranked recordings.");
    else line[0] = 0;
    set_line(subfile, line);
    set_line(substate, subLine);
}

/* Follow one submission: poll it every 10 s (API.md) until it is done.
 * The timer runs in the common modes, so it keeps going under a modal
 * window. */
- (void)follow:(long long)ident pending:(int)pending {
    subId = ident;
    subPending = pending;
    if(pending && !pollTimer){
        pollTimer = [NSTimer timerWithTimeInterval:10 target:self
                                          selector:@selector(pollTick:)
                                          userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:pollTimer forMode:NSRunLoopCommonModes];
    } else if(!pending && pollTimer){
        [pollTimer invalidate];
        pollTimer = nil;
    }
}

- (void)pollTick:(NSTimer *)t {
    if(subId && subPending && !polling && online.token[0]){
        char p[96];
        NetJob *j;
        snprintf(p, sizeof(p), "/api/v1/submissions/%lld", subId);
        j = [self netNew:NJ_POLL method:"GET" path:p];
        if(j && [self netStart:j]) polling = 1;
    }
}

/* A 401 on any call: API.md says forget the token and ask again.  It
 * happens after a password change, which logs out every client. */
- (void)authLost {
    secure_wipe(online.token, sizeof(online.token));
    online.username[0] = 0;
    online_save(&online);
    [self follow:0 pending:0];
    snprintf(subLine, sizeof(subLine), "You were logged out. Log in again to continue.");
    [self updateOnlineUI];
}

- (void)startList:(int)show {
    NetJob *j;
    if(!online.token[0]) return;
    lastList = [NSDate timeIntervalSinceReferenceDate];
    j = [self netNew:show ? NJ_LIST_SHOW : NJ_LIST method:"GET" path:"/api/v1/submissions"];
    if(j) [self netStart:j];
}

/* Upload one recording: the Submit button's candidate, or the row the
 * Replays window was asked about. */
- (void)startSubmit:(const char *)cand {
    NetJob *j;
    const char *err;
    if(!cand || submitting || !online.token[0]) return;
    j = [self netNew:NJ_SUBMIT method:"POST" path:"/api/v1/submissions"];
    if(!j) return;
    snprintf(j->file, sizeof(j->file), "%s", cand);
    snprintf(j->ctype, sizeof(j->ctype), "application/octet-stream");
    if(!read_recording(cand, &j->data, &j->n, &err)){
        net_free(j);
        alert(NSAlertStyleWarning, @"The recording cannot be sent.", S(err), nil, nil);
        return;
    }
    submitting = 1;
    [self updateOnlineUI];
    if(![self netStart:j]){ submitting = 0; [self updateOnlineUI]; }
}

/* First where the player stands (GET /api/v1/me), so that an upload that
 * would change no board can be asked about (submit_check()). */
- (void)startSubmitCheck:(const char *)cand {
    long long claim[5];
    NetJob *j;
    if(!cand || submitting || !online.token[0]) return;
    if(!read_claims(cand, claim)){ [self startSubmit:cand]; return; }
    j = [self netNew:NJ_ME method:"GET" path:"/api/v1/me"];
    if(!j){ [self startSubmit:cand]; return; }
    snprintf(j->file, sizeof(j->file), "%s", cand);
    submitting = 1;
    [self updateOnlineUI];
    if(![self netStart:j]){ submitting = 0; [self startSubmit:cand]; }
}

- (void)submitAfterCheck:(const NetJob *)j {
    char box[900];
    submitting = 0;
    if(submit_check(j->file, &j->r, box, sizeof(box))){
        [self startSubmit:j->file];
        return;
    }
    [self updateOnlineUI];
    if(alert(NSAlertStyleInformational, @"Submit this recording?", S(box), @"Submit", @"Cancel"))
        [self startSubmit:j->file];
}

/* A page of the website in the player's browser.  Only a web address: the
 * server comes from pfemu-online.cfg, and the system would just as happily
 * open anything else named there. */
- (void)openWeb:(const char *)p {
    char url[400];
    NSURL *u;
    if(strncmp(online.server, "https://", 8) && strncmp(online.server, "http://", 7)){
        alert(NSAlertStyleWarning, @"No web address.",
              @"The leaderboard server in pfemu-online.cfg is not a web address.", nil, nil);
        return;
    }
    snprintf(url, sizeof(url), "%s%s", online.server, p);
    u = [NSURL URLWithString:S(url)];
    if(!u || ![[NSWorkspace sharedWorkspace] openURL:u])
        alert(NSAlertStyleWarning, @"No web browser could be started.", S(url), nil, nil);
}

/* ---------------------------------------------------- the answers arrive */
- (void)netDone:(NetJob *)j {
    const char *s = j->r.body, *e = j->r.body + j->r.len;
    char msg[300];
    int pending = 0;
    switch(j->kind){
    case NJ_LOGOUT:
        net_free(j);
        return;
    case NJ_LOGIN: case NJ_REGISTER:
        /* Only to the window that asked; a cancelled one is gone. */
        if(loginWin && loginOpenGen == j->gen) [self loginDone:j];
        net_free(j);
        [self updateOnlineUI];
        return;
    case NJ_RL_LIST:
        if(rlWin) [self rlNetList:&j->r];
        else rlListing = 0;
        net_free(j);
        return;
    default:
        break;
    }
    if(j->r.status == 401){
        if(j->kind == NJ_SUBMIT || j->kind == NJ_ME) submitting = 0;
        if(j->kind == NJ_POLL) polling = 0;
        /* A stale answer to a token this launcher already dropped says
         * nothing about the current one. */
        if(!strcmp(j->token, online.token)) [self authLost];
        net_free(j);
        if(rlWin) [self rlSync];
        return;
    }
    switch(j->kind){
    case NJ_SUBMIT:
        submitting = 0;
        if(j->r.status == 200 || j->r.status == 202){
            long long ident = 0;
            json_num(s, e, "id", &ident);
            sub_describe(s, e, subLine, sizeof(subLine), &pending);
            [self follow:ident pending:pending];
            if(!strcmp(j->file, lastRec)) lastRec[0] = 0;
            /* 200 is API.md's "this account already sent this exact file":
             * nothing new happened, and the line alone would not say so. */
            if(j->r.status == 200){
                char box[700];
                snprintf(box, sizeof(box),
                         "It is submission #%lld. Sending it again does not verify it"
                         " again. The server verifies every kept recording again by"
                         " itself when it moves to a new pfemu build, and the result"
                         " below updates then.\n\n%s", ident, subLine);
                [self updateOnlineUI];
                alert(NSAlertStyleInformational, @"You already submitted this recording.",
                      S(box), nil, nil);
            }
        } else {
            online_message(&j->r, msg, sizeof(msg));
            alert(NSAlertStyleWarning, @"The recording was not submitted.", S(msg), nil, nil);
        }
        break;
    case NJ_POLL:
        polling = 0;
        if(j->r.status == 200){
            long long ident = 0;
            json_num(s, e, "id", &ident);
            if(ident == subId){
                sub_describe(s, e, subLine, sizeof(subLine), &pending);
                [self follow:ident pending:pending];
                /* The answer arrived while the player looks elsewhere,
                 * possibly at the next game: say so in the Dock. */
                if(!pending && !NSApp.active)
                    [NSApp requestUserAttention:NSInformationalRequest];
            }
        } else if(j->r.status == 404){
            [self follow:0 pending:0];
        }
        /* No answer: keep the line, the timer tries again. */
        break;
    case NJ_LIST:
        if(j->r.status == 200){
            long long ident = 0;
            if(sub_latest(&j->r, &ident, subLine, sizeof(subLine), &pending))
                [self follow:ident pending:pending];
        }
        break;
    case NJ_ME:
        [self submitAfterCheck:j];
        break;
    case NJ_LIST_SHOW:
        if(j->r.status == 200) [self showSubmissionList:&j->r];
        else {
            online_message(&j->r, msg, sizeof(msg));
            alert(NSAlertStyleWarning, @"The submissions could not be listed.", S(msg), nil, nil);
        }
        break;
    default:
        break;
    }
    net_free(j);
    [self updateOnlineUI];
    if(rlWin) [self rlSync];
}

/* ------------------------------------------------------ submissions window
 * One row per upload, newest first (sl_row(), src/launchcore.c).  Copy
 * puts the table on the clipboard tab-separated, which pastes as a table. */
- (void)showSubmissionList:(const HttpResp *)r {
    const char *e = r->body + r->len, *p = NULL, *oe;
    NSMutableString *tsv = [NSMutableString string];
    NSScrollView *sv;
    NSTextField *count;
    NSButton *web, *close;
    NSView *cv;
    int cap = 0, i;
    char line[64];
    slN = 0;
    for(i = 0; i < SL_COLS; i++)
        [tsv appendFormat:@"%@%@", S(sl_titles[i]), i + 1 < SL_COLS ? @"\t" : @"\n"];
    if(!json_arr(r->body, e, "submissions", &p, &e)) p = NULL;
    while(p && (p = json_next_obj(p, e, &oe)) != NULL){
        if(slN == cap){
            int nc = cap ? cap * 2 : 32;
            void *nb = realloc(slCells, (size_t)nc * sizeof(*slCells));
            int *nk = (int*)realloc(slKind, (size_t)nc * sizeof(int));
            if(nb) slCells = nb;
            if(nk) slKind = nk;
            if(!nb || !nk) break;
            cap = nc;
        }
        slKind[slN] = sl_row(p, oe, slCells[slN]);
        for(i = 0; i < SL_COLS; i++)
            [tsv appendFormat:@"%@%@", S(slCells[slN][i]), i + 1 < SL_COLS ? @"\t" : @"\n"];
        slN++;
        p = oe;
    }
    slTsv = tsv;
    slWin = [self panel:@"pfemu - submissions" size:NSMakeSize(900, 420) resizable:1];
    cv = slWin.contentView;
    slTable = [[NSTableView alloc] init];
    slTable.usesAlternatingRowBackgroundColors = YES;
    slTable.gridStyleMask = NSTableViewSolidVerticalGridLineMask;
    for(i = 0; i < SL_COLS; i++){
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"%d", i]];
        static const int w[SL_COLS] = { 50, 150, 90, 110, 220, 150, 110 };
        c.title = S(sl_titles[i]);
        c.width = w[i];
        [slTable addTableColumn:c];
    }
    slTable.dataSource = self;
    slTable.delegate = self;
    sv = [[NSScrollView alloc] init];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.documentView = slTable;
    sv.hasVerticalScroller = YES;
    sv.hasHorizontalScroller = YES;
    sv.borderType = NSBezelBorder;
    if(!slN) snprintf(line, sizeof(line), "No submissions yet.");
    else snprintf(line, sizeof(line), "%d submission%s", slN, slN == 1 ? "" : "s");
    count = [NSTextField labelWithString:S(line)];
    count.translatesAutoresizingMaskIntoConstraints = NO;
    slCopy = [NSButton buttonWithTitle:@"Copy" target:self action:@selector(slCopyTable:)];
    web = [NSButton buttonWithTitle:@"Open on the Website" target:self action:@selector(slOpenWeb:)];
    close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(closeModal:)];
    close.keyEquivalent = @"\r";
    for(NSView *v in @[slCopy, web, close]) v.translatesAutoresizingMaskIntoConstraints = NO;
    for(NSView *v in @[sv, count, slCopy, web, close]) [cv addSubview:v];
    [NSLayoutConstraint activateConstraints:@[
        [sv.topAnchor constraintEqualToAnchor:cv.topAnchor constant:12],
        [sv.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:12],
        [sv.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-12],
        [close.topAnchor constraintEqualToAnchor:sv.bottomAnchor constant:12],
        [close.trailingAnchor constraintEqualToAnchor:sv.trailingAnchor],
        [close.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor constant:-12],
        [web.firstBaselineAnchor constraintEqualToAnchor:close.firstBaselineAnchor],
        [web.trailingAnchor constraintEqualToAnchor:close.leadingAnchor constant:-8],
        [slCopy.firstBaselineAnchor constraintEqualToAnchor:close.firstBaselineAnchor],
        [slCopy.trailingAnchor constraintEqualToAnchor:web.leadingAnchor constant:-8],
        [count.firstBaselineAnchor constraintEqualToAnchor:close.firstBaselineAnchor],
        [count.leadingAnchor constraintEqualToAnchor:sv.leadingAnchor],
    ]];
    [slWin center];
    [NSApp runModalForWindow:slWin];
    [slWin orderOut:nil];
    slTable.dataSource = nil;
    slTable.delegate = nil;
    slWin = nil;
    slTable = nil;
    slCopy = nil;
    slTsv = nil;
    free(slCells); slCells = NULL;
    free(slKind); slKind = NULL;
    slN = 0;
}

- (void)slCopyTable:(id)sender {
    copy_text(slTsv);
    slCopy.title = @"Copied";
}

- (void)slOpenWeb:(id)sender {
    [self openWeb:"/me/submissions"];
}

/* ------------------------------------------------------------ login window */
- (void)loginBusy:(int)busy message:(const char *)msg {
    lgLogin.enabled = !busy;
    lgRegister.enabled = !busy;
    lgMsg.stringValue = S(msg ? msg : "");
}

- (void)loginSend:(int)reg {
    char user[80], pass[300], email[300];
    static char body[1800];
    NetJob *j;
    int ok;
    snprintf(user, sizeof(user), "%s", lgUser.stringValue.UTF8String);
    snprintf(pass, sizeof(pass), "%s", lgPass.stringValue.UTF8String);
    snprintf(email, sizeof(email), "%s", lgEmail.stringValue.UTF8String);
    ok = login_body(reg, user, pass, email, body, sizeof(body));
    secure_wipe(pass, sizeof(pass));
    if(!ok){
        lgMsg.stringValue = @"Enter a username and a password.";
        return;
    }
    j = [self netNew:reg ? NJ_REGISTER : NJ_LOGIN method:"POST"
                path:reg ? "/api/v1/register" : "/api/v1/login"];
    if(!j){ secure_wipe(body, sizeof(body)); return; }
    j->gen = loginOpenGen;
    j->token[0] = 0;
    snprintf(j->ctype, sizeof(j->ctype), "application/json");
    j->n = strlen(body);
    j->data = (char*)malloc(j->n + 1);
    if(j->data) memcpy(j->data, body, j->n + 1);
    secure_wipe(body, sizeof(body));
    if(!j->data){ net_free(j); return; }
    [self loginBusy:1 message:reg ? "Creating the account..." : "Logging in..."];
    if(![self netStart:j]) [self loginBusy:0 message:"Could not start the request."];
}

- (void)lgLoginPressed:(id)sender { [self loginSend:0]; }
- (void)lgRegisterPressed:(id)sender { [self loginSend:1]; }

- (void)loginDone:(NetJob *)j {
    char tok[160] = "", name[64] = "", msg[300];
    const char *s = j->r.body, *e = j->r.body + j->r.len;
    if((j->r.status == 200 || j->r.status == 201) &&
       json_str(s, e, "token", tok, sizeof(tok)) && tok[0]){
        json_str(s, e, "username", name, sizeof(name));
        snprintf(online.token, sizeof(online.token), "%s", tok);
        snprintf(online.username, sizeof(online.username), "%s", name[0] ? name : "?");
        secure_wipe(tok, sizeof(tok));
        /* The Keychain would not keep it: logged in all the same, until the
         * launcher closes. */
        sessionOnly = !online_save(&online);
        loginOk = 1;
        end_modal(NSModalResponseOK);
        return;
    }
    online_message(&j->r, msg, sizeof(msg));
    [self loginBusy:0 message:msg];
}

/* Modal, like the Details window.  1 when it ended logged in. */
- (int)showLogin {
    NSGridView *g;
    NSTextField *note;
    NSButton *cancel;
    NSStackView *buttons;
    NSView *cv, *empty = NSGridCell.emptyContentView;
    loginOk = 0;
    loginOpenGen = ++loginGen;
    loginWin = [self panel:@"pfemu - leaderboard account" size:NSMakeSize(420, 240) resizable:0];
    cv = loginWin.contentView;
    lgUser = [NSTextField textFieldWithString:S(online.username)];
    lgPass = [[NSSecureTextField alloc] init];
    lgEmail = [NSTextField textFieldWithString:@""];
    lgEmail.placeholderString = @"optional";
    [lgUser.widthAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;
    [lgPass.widthAnchor constraintEqualToAnchor:lgUser.widthAnchor].active = YES;
    [lgEmail.widthAnchor constraintEqualToAnchor:lgUser.widthAnchor].active = YES;
    note = [NSTextField wrappingLabelWithString:@"Email is optional and only used by Register."
                                                 " Without one, a forgotten password cannot be"
                                                 " recovered."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    note.preferredMaxLayoutWidth = 260;
    lgMsg = [NSTextField wrappingLabelWithString:@""];
    lgMsg.preferredMaxLayoutWidth = 340;
    g = [NSGridView gridViewWithViews:@[
        @[[NSTextField labelWithString:@"Username:"], lgUser],
        @[[NSTextField labelWithString:@"Password:"], lgPass],
        @[[NSTextField labelWithString:@"Email:"], lgEmail],
        @[empty, note],
        @[lgMsg, empty],
    ]];
    g.rowSpacing = 8;
    g.columnSpacing = 8;
    g.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [g columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [g mergeCellsInHorizontalRange:NSMakeRange(0, 2) verticalRange:NSMakeRange(4, 1)];
    [g cellAtColumnIndex:0 rowIndex:4].xPlacement = NSGridCellPlacementLeading;
    g.translatesAutoresizingMaskIntoConstraints = NO;
    cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(closeModal:)];
    cancel.keyEquivalent = @"\033";
    lgRegister = [NSButton buttonWithTitle:@"Register" target:self action:@selector(lgRegisterPressed:)];
    lgLogin = [NSButton buttonWithTitle:@"Log In" target:self action:@selector(lgLoginPressed:)];
    lgLogin.keyEquivalent = @"\r";
    buttons = [NSStackView stackViewWithViews:@[cancel, lgRegister, lgLogin]];
    buttons.spacing = 8;
    buttons.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:g];
    [cv addSubview:buttons];
    [NSLayoutConstraint activateConstraints:@[
        [g.topAnchor constraintEqualToAnchor:cv.topAnchor constant:20],
        [g.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:20],
        [g.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-20],
        [buttons.topAnchor constraintEqualToAnchor:g.bottomAnchor constant:16],
        [buttons.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-20],
        [buttons.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor constant:-16],
    ]];
    [cv layoutSubtreeIfNeeded];
    [loginWin setContentSize:cv.fittingSize];
    [loginWin center];
    [loginWin makeFirstResponder:online.username[0] ? (NSView *)lgPass : (NSView *)lgUser];
    [NSApp runModalForWindow:loginWin];
    [loginWin orderOut:nil];
    /* The password field's contents go with it. */
    lgPass.stringValue = @"";
    loginWin = nil;
    loginOpenGen = 0;
    lgUser = lgEmail = lgMsg = nil;
    lgPass = nil;
    lgLogin = lgRegister = nil;
    return loginOk;
}

/* A fresh login: show where the last submission stands. */
- (void)afterLogin {
    if(sessionOnly)
        snprintf(subLine, sizeof(subLine), "Logged in for this session only:"
                 " the Keychain would not keep the login.");
    else subLine[0] = 0;
    [self startList:0];
    [self updateOnlineUI];
}

/* ---------------------------------------------------------- replays window
 * Every recording in sessions/, newest first: what it holds, where it
 * stands on the leaderboard, Submit and Delete.  It is where Replay mode
 * picks its file; Other File... is there for one kept elsewhere.
 *
 * Where a recording stands comes from GET /api/v1/submissions, matched on
 * the SHA-256 of the file's bytes, which is what the server keeps an upload
 * under. */
static int rl_ends_pfr(const char *name){
    size_t l = strlen(name);
    return l > 4 && !_stricmp(name + l - 4, ".pfr");
}

static int rl_cmp(const void *a, const void *b){
    time_t x = ((const RlItem*)a)->mtime, y = ((const RlItem*)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

- (void)rlScan {
    DIR *dir = opendir(RL_DIR);
    struct dirent *e;
    int cap = 0;
    free(rlIt);
    rlIt = NULL;
    rlN = 0;
    if(dir){
        while((e = readdir(dir)) != NULL){
            RlItem *x;
            struct stat sb;
            char p[512];
            if(!rl_ends_pfr(e->d_name)) continue;
            snprintf(p, sizeof(p), RL_DIR "/%s", e->d_name);
            if(stat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            if(rlN == cap){
                int nc = cap ? cap * 2 : 32;
                RlItem *ni = (RlItem*)realloc(rlIt, (size_t)nc * sizeof(*ni));
                if(!ni) break;
                rlIt = ni;
                cap = nc;
            }
            x = &rlIt[rlN++];
            memset(x, 0, sizeof(*x));
            snprintf(x->path, sizeof(x->path), "%s", p);
            x->mtime = sb.st_mtime;
            x->size = (long long)sb.st_size;
            x->hd_ok = replay_read_header(x->path, &x->hd) == 0;
            x->have_games = read_claims(x->path, x->best);
            x->sub_s = x->sub_e = -1;
        }
        closedir(dir);
    }
    if(rlN > 1) qsort(rlIt, (size_t)rlN, sizeof(*rlIt), rl_cmp);
    free(rlCellsArr);
    rlCellsArr = (RlCells*)calloc((size_t)(rlN ? rlN : 1), sizeof(RlCells));
}

/* The running game's own recording: still being written, so hands off. */
- (int)rlBusy:(const RlItem *)x {
    return childRunning && childMode == LAUNCH_RECORD && same_file(childPath, x->path);
}

/* A complete ranked recording that the server does not have yet. */
- (int)rlCanSubmit:(const RlItem *)x {
    return x->hd_ok && x->hd.state[0] && x->hd.have_end && x->sub_s < 0 &&
           ![self rlBusy:x];
}

/* Each recording against the submissions answer, by hash. */
- (void)rlMatch {
    const char *e, *p = NULL, *oe;
    int i;
    for(i = 0; i < rlN; i++) rlIt[i].sub_s = rlIt[i].sub_e = -1;
    if(!rlSubs) return;
    e = rlSubs + rlNsubs;
    if(!json_arr(rlSubs, e, "submissions", &p, &e)) return;
    for(i = 0; i < rlN; i++){
        RlItem *x = &rlIt[i];
        if(!x->have_sha && ![self rlBusy:x])
            x->have_sha = release_hash_file(x->path, x->sha, NULL) == 0;
    }
    while((p = json_next_obj(p, e, &oe)) != NULL){
        char hx[80] = "", mine[65];
        json_str(p, oe, "sha256", hx, sizeof(hx));
        /* Newest first, so the first object that matches is the one. */
        for(i = 0; hx[0] && i < rlN; i++){
            RlItem *x = &rlIt[i];
            if(!x->have_sha || x->sub_s >= 0) continue;
            det_hex32(x->sha, mine);
            if(!_stricmp(hx, mine)){
                x->sub_s = (long)(p - rlSubs);
                x->sub_e = (long)(oe - rlSubs);
            }
        }
        p = oe;
    }
}

static void rl_when(time_t t, char *out, size_t n){
    struct tm tm;
    if(!localtime_r(&t, &tm)){ snprintf(out, n, "?"); return; }
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* One row's cells, as in src/launch.c's rl_cells(). */
- (void)rlCells:(RlItem *)x into:(RlCells *)c {
    static char sub[SL_COLS][SL_CELL];
    const char *b = base_name(x->path);
    size_t k = 0;
    int t;
    memset(c, 0, sizeof(*c));
    snprintf(c->file, sizeof(c->file), "%.*s", (int)strlen(b) - 4, b);
    rl_when(x->mtime, c->when, sizeof(c->when));
    if(x->have_games){
        for(t = 1; t <= 4 && k < sizeof(c->scores); t++){
            char sc[32];
            if(x->best[t] < 0) continue;
            fmt_score(x->best[t], sc, sizeof(sc));
            k += (size_t)snprintf(c->scores + k, sizeof(c->scores) - k, "%s%s %s",
                                  k ? ", " : "", sc, table_name(t));
        }
        if(!k) snprintf(c->scores, sizeof(c->scores), "none finished");
    } else if(x->hd_ok && x->hd.have_end && ![self rlBusy:x]){
        snprintf(c->scores, sizeof(c->scores), "not counted yet - replay it once");
    }
    if(x->hd_ok && x->hd.have_end){
        int s = (int)(x->hd.end_emu + 0.5);
        snprintf(c->len, sizeof(c->len), "%d:%02d", s / 60, s % 60);
    }
    x->kind = SL_PLAIN;
    if([self rlBusy:x]) snprintf(c->board, sizeof(c->board), "being recorded");
    else if(!x->hd_ok) snprintf(c->board, sizeof(c->board), "not a readable recording");
    else if(!x->hd.state[0]) snprintf(c->board, sizeof(c->board), "not ranked");
    else if(!x->hd.have_end) snprintf(c->board, sizeof(c->board), "incomplete");
    else if(x->uploading && submitting) snprintf(c->board, sizeof(c->board), "uploading...");
    else if(x->sub_s >= 0){
        long long ident = 0;
        const char *s = rlSubs + x->sub_s, *e = rlSubs + x->sub_e;
        x->kind = sl_row(s, e, sub);
        json_num(s, e, "id", &ident);
        snprintf(c->board, sizeof(c->board), "#%lld %.150s", ident,
                 x->kind == SL_PENDING ? sub[2] : sub[4]);
    }
    else if(!online.token[0]) snprintf(c->board, sizeof(c->board), "log in to submit");
    else if(!rlSubs) snprintf(c->board, sizeof(c->board), "asking the server...");
    else snprintf(c->board, sizeof(c->board), "not submitted");
    /* A verified result's games are the ones that count, so they replace
     * what pfemu counted here; ours stay, in grey, until there is one. */
    x->verified = 0;
    if(x->kind == SL_COUNTS && sub[3][0]){
        snprintf(c->scores, sizeof(c->scores), "%s", sub[3]);
        x->verified = 1;
    }
}

/* New answers, same rows: only the texts change, and reloadData keeps the
 * selection. */
- (void)rlRefresh {
    int i;
    for(i = 0; i < rlN; i++) [self rlCells:&rlIt[i] into:&rlCellsArr[i]];
    [rlTable reloadData];
}

/* All rows again, with one selected: the one named, else the first. */
- (void)rlFill:(const char *)select {
    int i, at = rlN ? 0 : -1;
    char line[96];
    [self rlRefresh];
    for(i = 0; select && select[0] && i < rlN; i++)
        if(same_file(rlIt[i].path, select)){ at = i; break; }
    if(at >= 0){
        [rlTable selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)at]
             byExtendingSelection:NO];
        [rlTable scrollRowToVisible:at];
    } else [rlTable deselectAll:nil];
    if(!rlN) snprintf(line, sizeof(line), "No recordings in " RL_DIR "/ yet.");
    else snprintf(line, sizeof(line), "%d recording%s in " RL_DIR "/", rlN, rlN == 1 ? "" : "s");
    rlCount.stringValue = S(line);
}

/* The one row the details, Replay and Submit are about: the last one
 * clicked when it is selected, else the first selected one. */
- (int)rlSelected {
    NSInteger r = rlTable.selectedRow;
    if(r >= 0 && r < rlN && [rlTable isRowSelected:r]) return (int)r;
    r = (NSInteger)rlTable.selectedRowIndexes.firstIndex;
    return (r != NSNotFound && r >= 0 && r < rlN) ? (int)r : -1;
}

/* Everything known about the selected recording, in the Details idiom. */
- (void)rlDetail {
    static char raw[8192];
    int i = [self rlSelected], anyFree = 0;
    char buf[256], sz[32];
    RlItem *x;
    NSIndexSet *set = rlTable.selectedRowIndexes;
    raw[0] = 0;
    for(NSUInteger k = set.firstIndex; k != NSNotFound; k = [set indexGreaterThanIndex:k])
        if((int)k < rlN && ![self rlBusy:&rlIt[k]]) anyFree = 1;
    rlDel.enabled = anyFree;
    rlReplay.enabled = i >= 0 && rlIt[i].hd_ok && rlIt[i].hd.have_end && ![self rlBusy:&rlIt[i]];
    rlSubmitBtn.enabled = i >= 0 && [self rlCanSubmit:&rlIt[i]] &&
                          !(rlIt[i].uploading && submitting);
    if(i < 0){
        rlDetailView.string = rlN ? @"Pick a recording to see what it holds." : @"";
        return;
    }
    x = &rlIt[i];
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
        if([self rlBusy:x]) det_kv(raw, sizeof(raw), "Session", "being recorded");
        else if(h->have_end)
            det_kv(raw, sizeof(raw), "Session", "%d events, %.1f s", h->nevents, h->end_emu);
        else det_kv(raw, sizeof(raw), "Session", "incomplete - the recording has no end");
        det_kv(raw, sizeof(raw), "Ranked", "%s", h->state[0]
               ? "yes, recorded against the canonical state"
               : "no, recorded with the player's own high-score tables");
    }
    games_report(raw, sizeof(raw), x->path, x->have_games);
    if(x->sub_s >= 0){
        int pending = 0;
        sub_describe(rlSubs + x->sub_s, rlSubs + x->sub_e, buf, sizeof(buf), &pending);
        det_kv(raw, sizeof(raw), "Leaderboard", "%s", buf);
    } else if(x->hd_ok && x->hd.state[0] && x->hd.have_end && ![self rlBusy:x]){
        det_kv(raw, sizeof(raw), "Leaderboard", "%s",
               !online.token[0] ? "Log in to submit it and see its result." :
               !rlSubs ? "Asking the server..." : "Not submitted yet.");
    }
    rlDetailView.string = S(raw);
}

- (void)rlRequest {
    NetJob *j;
    if(rlListing || !online.token[0]) return;
    j = [self netNew:NJ_RL_LIST method:"GET" path:"/api/v1/submissions"];
    if(j && [self netStart:j]) rlListing = 1;
}

/* The submissions list arrived (netDone:). */
- (void)rlNetList:(const HttpResp *)r {
    rlListing = 0;
    if(r->status == 200 && r->body){
        char *copy = (char*)malloc(r->len + 1);
        if(copy){
            memcpy(copy, r->body, r->len);
            copy[r->len] = 0;
            free(rlSubs);
            rlSubs = copy;
            rlNsubs = r->len;
            [self rlMatch];
            [self rlRefresh];
            [self rlDetail];
        }
    }
}

/* The launcher saw an upload, a poll or a list come back. */
- (void)rlSync {
    int i;
    if(!submitting)
        for(i = 0; i < rlN; i++) rlIt[i].uploading = 0;
    [self rlRequest];
    [self rlRefresh];
    [self rlDetail];
}

- (void)rlRescan {
    char keep[600] = "";
    int i = [self rlSelected];
    if(i >= 0) snprintf(keep, sizeof(keep), "%s", rlIt[i].path);
    [self rlScan];
    [self rlMatch];
    [self rlFill:keep];
    [self rlDetail];
}

- (void)rlSubmit:(id)sender {
    int i = [self rlSelected];
    RlItem *x;
    if(i < 0) return;
    x = &rlIt[i];
    if(![self rlCanSubmit:x]) return;
    if(!online.token[0]){
        if(![self showLogin]) return;
        [self afterLogin];
        [self rlRequest];
    }
    if(submitting){
        alert(NSAlertStyleInformational, @"Another upload is still on its way.",
              @"Try again when it is done.", nil, nil);
        return;
    }
    x->uploading = 1;
    [self startSubmitCheck:x->path];
    [self rlRefresh];
    [self rlDetail];
}

/* Every selected row to the Trash, .games file and all. */
- (void)rlDelete:(id)sender {
    NSIndexSet *set = rlTable.selectedRowIndexes;
    int *rows = (int*)malloc(sizeof(int) * (size_t)(rlN ? rlN : 1));
    int nr = 0, unsent = 0, failed = 0, i;
    char box[700], selPath[600] = "";
    NSFileManager *fm = [NSFileManager defaultManager];
    if(!rows) return;
    for(NSUInteger k = set.firstIndex; k != NSNotFound; k = [set indexGreaterThanIndex:k])
        if((int)k < rlN && ![self rlBusy:&rlIt[k]]) rows[nr++] = (int)k;
    if(!nr){ free(rows); return; }
    for(i = 0; i < nr; i++)
        if([self rlCanSubmit:&rlIt[rows[i]]]) unsent++;
    if(nr == 1)
        snprintf(box, sizeof(box), "%s", unsent
                 ? "It is a ranked recording that was never submitted." : "");
    else if(unsent)
        snprintf(box, sizeof(box), "%d of them %s ranked and never submitted.", unsent,
                 unsent == 1 ? "is" : "are");
    else box[0] = 0;
    { char head[400];
      if(nr == 1) snprintf(head, sizeof(head), "Move %s to the Trash?", base_name(rlIt[rows[0]].path));
      else snprintf(head, sizeof(head), "Move %d recordings to the Trash?", nr);
      if(!alert(NSAlertStyleWarning, S(head), S(box), @"Move to Trash", @"Cancel")){
          free(rows);
          return;
      } }
    /* Keep the place in the list: the row after the first deleted one
     * (rows[] is in ascending order). */
    for(i = rows[0]; i < rlN; i++){
        int gone = 0, j;
        for(j = 0; j < nr; j++) if(rows[j] == i) gone = 1;
        if(!gone){ snprintf(selPath, sizeof(selPath), "%s", rlIt[i].path); break; }
    }
    for(i = 0; i < nr; i++){
        const char *p = rlIt[rows[i]].path;
        char games[640];
        NSError *err = nil;
        /* Asked before the file goes: same_file() resolves both paths. */
        int current = same_file(p, replayPath);
        int last = same_file(p, lastRec);
        if(![fm trashItemAtURL:[NSURL fileURLWithPath:S(p)] resultingItemURL:nil error:&err]){
            fprintf(stderr, "[launcher] %s not moved to the Trash: %s\n", p,
                    err ? err.localizedDescription.UTF8String : "?");
            failed++;
            continue;
        }
        snprintf(games, sizeof(games), "%s.games", p);
        if(access(games, F_OK) == 0)
            [fm trashItemAtURL:[NSURL fileURLWithPath:S(games)] resultingItemURL:nil error:nil];
        /* The launcher must not keep pointing at a file that is gone. */
        if(last) lastRec[0] = 0;
        if(current){
            replayPath[0] = 0;
            [self setPathText];
            if(mode == LAUNCH_REPLAY) [self replayLoadFile];
        }
    }
    free(rows);
    if(failed)
        alert(NSAlertStyleWarning, @"Not every recording could be moved to the Trash.",
              @"The files are still there. A folder on a network share may have no Trash.",
              nil, nil);
    [self rlScan];
    [self rlMatch];
    [self rlFill:selPath];
    [self rlDetail];
    [self updateOnlineUI];
}

enum { RL_REPLAY = 1000, RL_OTHER = 1001 };

- (void)rlReplayPressed:(id)sender {
    int i = [self rlSelected];
    if(i < 0 || !rlReplay.enabled) return;
    snprintf(rlPicked, sizeof(rlPicked), "%s", rlIt[i].path);
    end_modal(RL_REPLAY);
}

- (void)rlOtherPressed:(id)sender {
    if([self openRecording:rlPicked size:sizeof(rlPicked)]) end_modal(RL_OTHER);
}

static NSTableColumn *rl_column(NSTableView *t, int ident, NSString *title, CGFloat w){
    NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"%d", ident]];
    c.title = title;
    c.width = w;
    c.minWidth = 40;
    [t addTableColumn:c];
    return c;
}

/* Modal, like Details.  A recording picked for replay lands in the Session
 * group, switched to Replay. */
- (void)showReplays {
    NSScrollView *sv, *sv2;
    NSTextView *tv;
    NSButton *other, *close;
    NSView *cv;
    rlPicked[0] = 0;
    rlSubs = NULL;
    rlNsubs = 0;
    rlListing = 0;
    rlWin = [self panel:@"pfemu - replays" size:NSMakeSize(1000, 560) resizable:1];
    rlWin.minSize = NSMakeSize(640, 360);
    cv = rlWin.contentView;
    rlTable = [[PFTable alloc] init];
    rlTable.deleteTarget = self;
    rlTable.deleteAction = @selector(rlDelete:);
    rlTable.allowsMultipleSelection = YES;
    rlTable.usesAlternatingRowBackgroundColors = YES;
    rlTable.gridStyleMask = NSTableViewSolidVerticalGridLineMask;
    rlTable.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    rl_column(rlTable, RC_FILE, @"Recording", 210);
    rl_column(rlTable, RC_WHEN, @"Recorded", 130);
    rl_column(rlTable, RC_SCORES, @"Best 3-ball games", 260);
    rl_column(rlTable, RC_LENGTH, @"Length", 64);
    rl_column(rlTable, RC_BOARD, @"Leaderboard", 260);
    rlTable.dataSource = self;
    rlTable.delegate = self;
    rlTable.target = self;
    rlTable.doubleAction = @selector(rlReplayPressed:);
    sv = [[NSScrollView alloc] init];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.documentView = rlTable;
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    sv2 = mono_text(&tv);
    rlDetailView = tv;
    rlCount = [NSTextField labelWithString:@""];
    rlCount.lineBreakMode = NSLineBreakByTruncatingTail;
    rlSubmitBtn = [NSButton buttonWithTitle:@"Submit" target:self action:@selector(rlSubmit:)];
    rlSubmitBtn.toolTip = @"Upload the selected ranked recording to the leaderboard";
    rlDel = [NSButton buttonWithTitle:@"Move to Trash" target:self action:@selector(rlDelete:)];
    rlDel.toolTip = @"Move the selected recordings to the Trash (Delete key)";
    other = [NSButton buttonWithTitle:@"Other File…" target:self action:@selector(rlOtherPressed:)];
    close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(closeModal:)];
    close.keyEquivalent = @"\033";
    rlReplay = [NSButton buttonWithTitle:@"Replay" target:self action:@selector(rlReplayPressed:)];
    rlReplay.keyEquivalent = @"\r";
    for(NSView *v in @[rlCount, rlSubmitBtn, rlDel, other, close, rlReplay]){
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [cv addSubview:v];
    }
    [cv addSubview:sv];
    [cv addSubview:sv2];
    [NSLayoutConstraint activateConstraints:@[
        [sv.topAnchor constraintEqualToAnchor:cv.topAnchor constant:12],
        [sv.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:12],
        [sv.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-12],
        [sv2.topAnchor constraintEqualToAnchor:sv.bottomAnchor constant:8],
        [sv2.leadingAnchor constraintEqualToAnchor:sv.leadingAnchor],
        [sv2.trailingAnchor constraintEqualToAnchor:sv.trailingAnchor],
        [sv2.heightAnchor constraintEqualToConstant:170],
        [rlDel.topAnchor constraintEqualToAnchor:sv2.bottomAnchor constant:8],
        [rlDel.trailingAnchor constraintEqualToAnchor:sv.trailingAnchor],
        [rlSubmitBtn.firstBaselineAnchor constraintEqualToAnchor:rlDel.firstBaselineAnchor],
        [rlSubmitBtn.trailingAnchor constraintEqualToAnchor:rlDel.leadingAnchor constant:-8],
        [rlCount.firstBaselineAnchor constraintEqualToAnchor:rlDel.firstBaselineAnchor],
        [rlCount.leadingAnchor constraintEqualToAnchor:sv.leadingAnchor],
        [rlCount.trailingAnchor constraintLessThanOrEqualToAnchor:rlSubmitBtn.leadingAnchor constant:-8],
        [rlReplay.topAnchor constraintEqualToAnchor:rlDel.bottomAnchor constant:16],
        [rlReplay.trailingAnchor constraintEqualToAnchor:sv.trailingAnchor],
        [rlReplay.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor constant:-12],
        [close.firstBaselineAnchor constraintEqualToAnchor:rlReplay.firstBaselineAnchor],
        [close.trailingAnchor constraintEqualToAnchor:rlReplay.leadingAnchor constant:-8],
        [other.firstBaselineAnchor constraintEqualToAnchor:rlReplay.firstBaselineAnchor],
        [other.leadingAnchor constraintEqualToAnchor:sv.leadingAnchor],
    ]];
    [self rlScan];
    [self rlFill:replayPath];
    [self rlDetail];
    [self rlRequest];
    [rlWin center];
    [rlWin makeFirstResponder:rlTable];
    [NSApp runModalForWindow:rlWin];
    [rlWin orderOut:nil];
    rlTable.dataSource = nil;
    rlTable.delegate = nil;
    rlWin = nil;
    rlTable = nil;
    rlDetailView = nil;
    rlCount = nil;
    rlDel = rlSubmitBtn = rlReplay = nil;
    free(rlIt); rlIt = NULL;
    free(rlCellsArr); rlCellsArr = NULL;
    free(rlSubs); rlSubs = NULL;
    rlN = 0;
    if(rlPicked[0]){
        LaunchMode prev = mode;
        snprintf(replayPath, sizeof(replayPath), "%s", rlPicked);
        pathCustom = 1;
        if(mode != LAUNCH_REPLAY){
            mode = LAUNCH_REPLAY;
            [self applyModeUI:prev];   /* keeps the path: pathCustom */
        }
        [self setPathText];
        [self replayLoadFile];
    }
}

/* ---------------------------------------------------------- the two tables */
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t {
    if(t == rlTable) return rlN;
    if(t == slTable) return slN;
    return 0;
}

- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    NSTextField *f = [t makeViewWithIdentifier:@"cell" owner:self];
    int c = col.identifier.intValue;
    const char *text = "";
    NSColor *color = NSColor.labelColor;
    if(!f){
        f = [NSTextField labelWithString:@""];
        f.identifier = @"cell";
        f.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    f.alignment = NSTextAlignmentLeft;
    if(t == rlTable && row < rlN){
        const RlCells *rc = &rlCellsArr[row];
        const RlItem *x = &rlIt[row];
        switch(c){
        case RC_FILE:   text = rc->file; break;
        case RC_WHEN:   text = rc->when; break;
        case RC_SCORES: text = rc->scores;
                        if(!x->verified) color = NSColor.secondaryLabelColor;
                        break;
        case RC_LENGTH: text = rc->len; f.alignment = NSTextAlignmentRight; break;
        case RC_BOARD:  text = rc->board; color = kind_color(x->kind); break;
        }
    } else if(t == slTable && row < slN && c >= 0 && c < SL_COLS){
        text = slCells[row][c];
        color = kind_color(slKind[row]);
    }
    f.stringValue = S(text);
    f.toolTip = text[0] ? S(text) : nil;
    f.textColor = color;
    return f;
}

- (void)tableViewSelectionDidChange:(NSNotification *)n {
    if(n.object == rlTable) [self rlDetail];
}

/* ------------------------------------------------------------- the game */
/* The game ended.  Come back to the front, and if it was a recording that
 * can rank, offer it for submission. */
- (void)childDone {
    ReplayHeader hd;
    launch.title = @"Launch";
    if(childMode == LAUNCH_RECORD && childPath[0]){
        if(replay_read_header(childPath, &hd) == 0 && hd.have_end){
            if(hd.state[0]){
                snprintf(lastRec, sizeof(lastRec), "%s", childPath);
            } else {
                lastRec[0] = 0;
                snprintf(subLine, sizeof(subLine),
                         "Recorded without Ranked: %s cannot be submitted.",
                         base_name(childPath));
            }
        }
        /* The next recording gets a fresh name: a finished session is a
         * playthrough that cannot be reproduced, and aiming the next one
         * at the same file would overwrite it. */
        if(mode == LAUNCH_RECORD){
            pathCustom = 0;
            [self restoreSessionPath:0];
        }
    }
    [self showDetection];
    [self updateOnlineUI];
    if(rlWin) [self rlRescan];
    bring_to_front();
    [win makeKeyAndOrderFront:nil];
    if(lastRec[0] && submitBtn.enabled) [win makeFirstResponder:submitBtn];
}

- (void)childExited:(int)status {
    if(!childRunning) return;
    if(WIFEXITED(status))
        fprintf(stderr, "[launcher] the game ended, exit code %d\n", WEXITSTATUS(status));
    else
        fprintf(stderr, "[launcher] the game ended, wait status %d\n", status);
    if(childWatch){
        dispatch_source_cancel(childWatch);
        childWatch = nil;
    }
    childRunning = 0;
    [self childDone];
}

- (int)spawnGame {
    const RelResult *r = [self cur];
    char exe[1024], table[8];
    const char *argv[16];
    int argc = 0, err, status;
    pid_t pid;
    uint32_t len = GetModuleFileNameA(NULL, exe, (uint32_t)sizeof(exe));
    if(!r || childRunning || len == 0 || len >= sizeof(exe)) return 0;
    argv[argc++] = exe;
    argv[argc++] = "-nolauncher";
    argv[argc++] = "-launched";
    argv[argc++] = "-d";
    argv[argc++] = r->dir;
    if(fullscreen) argv[argc++] = "-fullscreen";
    /* A replay carries its own start; run.c would only ignore this. */
    if(mode != LAUNCH_REPLAY && startTable){
        snprintf(table, sizeof(table), "%d", startTable);
        argv[argc++] = "-table";
        argv[argc++] = table;
    }
    if(mode == LAUNCH_RECORD){
        argv[argc++] = "-record";
        argv[argc++] = replayPath;
        if(online.ranked) argv[argc++] = "-ranked";
    } else if(mode == LAUNCH_REPLAY){
        argv[argc++] = "-replay";
        argv[argc++] = replayPath;
    }
    argv[argc] = NULL;
    /* Same working directory as this process: install directories are
     * stored relative to it. */
    err = posix_spawn(&pid, exe, NULL, NULL, (char *const *)argv, environ);
    if(err != 0){
        alert(NSAlertStyleWarning, @"The game could not be started.",
              S(strerror(err)), nil, nil);
        return 0;
    }
    fprintf(stderr, "[launcher] started %s (%s)\n", r->dir, r->rel ? r->rel->id : "?");
    last_save(r);
    childPid = pid;
    childRunning = 1;
    childMode = mode;
    snprintf(childPath, sizeof(childPath), "%s", mode == LAUNCH_PLAY ? "" : replayPath);
    /* Told on the main queue when it exits.  A game that is gone before the
     * source is armed is caught by the look straight after. */
    childWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid,
                                        DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    dispatch_source_set_event_handler(childWatch, ^{
        int st = 0;
        waitpid(pid, &st, 0);
        [self childExited:st];
    });
    dispatch_resume(childWatch);
    launch.title = @"Running…";
    launch.enabled = NO;
    if(waitpid(pid, &status, WNOHANG) == pid) [self childExited:status];
    return 1;
}

- (void)launchGame:(id)sender {
    const RelResult *r = [self cur];
    const char *dir;
    if(childRunning) return;
    if(mode == LAUNCH_REPLAY){
        /* No config writes in replay mode: the install's files are the
         * session's inputs, and replay promises never to write the real
         * overlay (REPLAY.md 3.3). */
        char why[256];
        if(!replay_check(rhdrOk, &rhdr, replayErr, r, why, sizeof(why))){
            alert(NSAlertStyleWarning, @"This recording cannot be replayed.", S(why), nil, nil);
            return;
        }
        write_session_path([self gameDir], replayPath);
        [self spawnGame];
        return;
    }
    if(!r || !release_runnable(r)) return;
    if(mode == LAUNCH_RECORD && !replayPath[0]){
        alert(NSAlertStyleWarning, @"Nothing to record to.",
              @"Pick a target .pfr file first.", nil, nil);
        return;
    }
    if(mode == LAUNCH_RECORD){
        /* A typed path may lack the extension - normalise so recordings
         * stay findable (the Replays window lists *.pfr). */
        const char *base = base_name(replayPath);
        if(!strchr(base, '.') && strlen(replayPath) + 4 < sizeof(replayPath)){
            strcat(replayPath, ".pfr");
            [self setPathText];
        }
    }
    dir = r->dir;
    write_sound_cfg(dir, sound, quality);
    /* Everything the window owns lands in one file, in one write
     * (src/cfg.c).  Read first so a key this window does not show
     * survives. */
    { PfCfg c;
      cfg_read(dir, &c);
      c.volume = volume;
      c.bass = bass;
      c.treble = treble;
      c.oomph = oomph;
      c.headphone = headphone != 0;
      c.quality = quality;
      memcpy(c.options, cfg, 6);
      /* The trainer is incompatible with recording; main() refuses as a
       * backstop.  The session runs with it off. */
      c.trainer = (mode == LAUNCH_RECORD) ? 0 : (cheatEnable != 0);
      c.fullscreen = fullscreen != 0;
      c.start_table = startTable;
      if(mode == LAUNCH_RECORD && replayPath[0])
          snprintf(c.session, sizeof(c.session), "%s", replayPath);
      cfg_write(dir, &c); }
    [self spawnGame];
}

/* ------------------------------------------------------ control actions */
- (void)toggled:(NSButton *)b {
    int on = b.state == NSControlStateValueOn;
    if(b == wSound) sound = on;
    else if(b == wHeadphone) headphone = on;
    else if(b == wTrainer) cheatEnable = on;
    else if(b == wFullscreen) fullscreen = on;
    else if(b == rankedCb){ online.ranked = on; online_save(&online); }
    else if(b == modePlay || b == modeRecord || b == modeReplay){
        LaunchMode prev = mode;
        mode = (b == modeRecord) ? LAUNCH_RECORD :
               (b == modeReplay) ? LAUNCH_REPLAY : LAUNCH_PLAY;
        if(mode != prev) [self applyModeUI:prev];
        else set_check(b, 1);
    }
}

- (void)picked:(NSPopUpButton *)p {
    int i = (int)p.indexOfSelectedItem, k;
    if(i < 0) return;
    if(p == install){ sel = i; [self reloadForDir]; }
    else if(p == wQuality) quality = i;
    else if(p == wBass) bass = eq_idx_to_db(i);
    else if(p == wTreble) treble = eq_idx_to_db(i);
    else if(p == wOomph) oomph = oomph_idx_to_db(i);
    else if(p == wTable) startTable = i;
    else for(k = 0; k < 6; k++) if(p == wOpt[k]) cfg[k] = (uint8_t)i;
}

- (void)volumeMoved:(NSSlider *)s {
    volume = s.intValue;
    wVolumeText.stringValue = [NSString stringWithFormat:@"%d%%", volume];
}

- (void)controlTextDidChange:(NSNotification *)n {
    if(n.object != path) return;
    snprintf(replayPath, sizeof(replayPath), "%s", path.stringValue.UTF8String);
    pathCustom = 1;
    if(mode == LAUNCH_REPLAY) [self replayLoadFile];
    else [self showDetection];
}

- (void)browse:(id)sender {
    if(mode == LAUNCH_RECORD) [self pickRecordTarget];
    else [self showReplays];
}

/* Log in, or out.  Logging out ends the token on the server (the answer
 * does not matter) and forgets it here straight away. */
- (void)loginPressed:(id)sender {
    if(online.token[0]){
        NetJob *j = [self netNew:NJ_LOGOUT method:"POST" path:"/api/v1/logout"];
        if(j) [self netStart:j];
        secure_wipe(online.token, sizeof(online.token));
        online.username[0] = 0;
        sessionOnly = 0;
        online_save(&online);
        [self follow:0 pending:0];
        subLine[0] = 0;
        [self updateOnlineUI];
    } else if([self showLogin]){
        [self afterLogin];
    }
}

- (void)submitPressed:(id)sender {
    const char *why;
    [self startSubmitCheck:[self submitCandidate:&why]];
}

- (void)sublistPressed:(id)sender {
    [self startList:1];
}

- (void)boardsPressed:(id)sender { [self openWeb:"/boards"]; }
- (void)mePressed:(id)sender { [self openWeb:"/me"]; }

/* The folder the installations live in, in the Finder.  In pfemu.app that
 * is under ~/Library, which the Finder does not show. */
- (void)showFolder:(id)sender {
    char cwd[PATH_MAX];
    if(!getcwd(cwd, sizeof(cwd))) return;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:S(cwd) isDirectory:YES]];
}

- (void)quit:(id)sender {
    [win performClose:sender];
}

/* The game is its own process and keeps running; say so rather than let it
 * look as if closing the launcher had closed it. */
- (BOOL)confirmQuit {
    if(quitConfirmed || !childRunning) return YES;
    if(!alert(NSAlertStyleWarning, @"The game is still running.",
              @"Close the launcher anyway? The game keeps running.",
              @"Close Launcher", @"Cancel"))
        return NO;
    quitConfirmed = 1;
    return YES;
}

- (BOOL)windowShouldClose:(NSWindow *)w {
    if(w != win) return YES;
    return [self confirmQuit];
}

/* A modal window's close box ends its loop like its Close button. */
- (void)windowWillClose:(NSNotification *)n {
    if(n.object != win) end_modal(NSModalResponseCancel);
}

/* A finished submission is not polled, but the server can still change its
 * result: it verifies every kept recording again when its pfemu build
 * changes.  So look again whenever the player comes back to the launcher,
 * at most every 30 s. */
- (void)windowDidBecomeKey:(NSNotification *)n {
    if(n.object != win) return;
    if(online.token[0] && !subPending &&
       [NSDate timeIntervalSinceReferenceDate] - lastList > 30)
        [self startList:0];
}

/* ------------------------------------------------------------- building */
- (NSPopUpButton *)popup:(const char *const *)items count:(int)n sel:(int)s {
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    int i;
    /* Item by item: addItemsWithTitles: drops a title it already has. */
    for(i = 0; i < n; i++) [p.menu addItemWithTitle:S(items[i]) action:NULL keyEquivalent:@""];
    [p selectItemAtIndex:s];
    p.target = self;
    p.action = @selector(picked:);
    return p;
}

- (NSButton *)check:(NSString *)title on:(int)on {
    NSButton *b = [NSButton checkboxWithTitle:title target:self action:@selector(toggled:)];
    set_check(b, on);
    return b;
}

static NSTextField *label(NSString *text){
    return [NSTextField labelWithString:text];
}

/* A button that looks like a link, for the website. */
static NSButton *link_button(NSString *title, id target, SEL action){
    NSButton *b = [NSButton buttonWithTitle:title target:target action:action];
    b.bordered = NO;
    b.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{
        NSForegroundColorAttributeName: NSColor.linkColor,
        NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
        NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize],
    }];
    return b;
}

/* A view that takes the width it is given and no more: a long detection
 * line or installation name ends in an ellipsis rather than widening the
 * window.  Below the fitting-size compression (50), or the window would be
 * sized to the whole text. */
static void squeezable(NSView *v){
    [v setContentCompressionResistancePriority:40
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    [v setContentHuggingPriority:40
                  forOrientation:NSLayoutConstraintOrientationHorizontal];
}

static NSTextField *line_label(void){
    NSTextField *t = label(@"");
    t.lineBreakMode = NSLineBreakByTruncatingTail;
    squeezable(t);
    return t;
}

/* A child of a vertical stack as wide as the stack. */
static void add_full(NSStackView *stack, NSView *v){
    [stack addArrangedSubview:v];
    [v.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* A titled box as wide as the column, around content inset by a margin.
 * fill: the content takes the box's whole width, else only what it needs. */
static NSBox *boxed(NSStackView *column, NSString *title, NSView *content, int fill){
    NSBox *box = [[NSBox alloc] init];
    NSView *cv;
    box.title = title;
    cv = box.contentView;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.topAnchor constraintEqualToAnchor:cv.topAnchor constant:8],
        [content.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:10],
        [content.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor constant:-10],
        fill ? [content.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-10]
             : [content.trailingAnchor constraintLessThanOrEqualToAnchor:cv.trailingAnchor constant:-10],
    ]];
    add_full(column, box);
    return box;
}

static NSGridView *grid(void){
    NSGridView *g = [[NSGridView alloc] initWithFrame:NSZeroRect];
    g.rowSpacing = 8;
    g.columnSpacing = 8;
    g.rowAlignment = NSGridRowAlignmentFirstBaseline;
    return g;
}

/* Labels right-aligned against their controls, the way a Mac form reads. */
static void labels_trailing(NSGridView *g, int col){
    [g columnAtIndex:col].xPlacement = NSGridCellPlacementTrailing;
}

/* One view across columns [from, from + n) of a row just added. */
static void span(NSGridView *g, NSInteger row, NSInteger from, NSInteger n){
    [g mergeCellsInHorizontalRange:NSMakeRange((NSUInteger)from, (NSUInteger)n)
                     verticalRange:NSMakeRange((NSUInteger)row, 1)];
    [g cellAtColumnIndex:from rowIndex:row].xPlacement = NSGridCellPlacementLeading;
}

static NSStackView *vstack(void){
    NSStackView *s = [[NSStackView alloc] init];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 10;
    s.translatesAutoresizingMaskIntoConstraints = NO;
    return s;
}

static NSStackView *hstack(NSArray<NSView *> *views){
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.alignment = NSLayoutAttributeFirstBaseline;
    s.spacing = 8;
    return s;
}

- (void)buildWindow {
    NSView *cv;
    NSStackView *outer, *cols, *left, *right, *row;
    NSGridView *g;
    NSTextField *t;
    NSButton *quitBtn, *folder;
    NSView *empty = nil;
    int i;

    win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 520)
                                      styleMask:NSWindowStyleMaskTitled |
                                                NSWindowStyleMaskClosable |
                                                NSWindowStyleMaskMiniaturizable
                                        backing:NSBackingStoreBuffered
                                          defer:NO];
    win.releasedWhenClosed = NO;
    win.title = @"pfemu";
    win.delegate = self;
    cv = win.contentView;
    outer = vstack();
    outer.spacing = 12;
    [cv addSubview:outer];
    [NSLayoutConstraint activateConstraints:@[
        [outer.topAnchor constraintEqualToAnchor:cv.topAnchor constant:16],
        [outer.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor constant:20],
        [outer.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-20],
        [outer.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor constant:-16],
    ]];

    /* Game, across both columns: the installation (when there is a choice)
     * and what the detector made of it.  The files decide; Details
     * explains. */
    { NSStackView *game = vstack();
      game.spacing = 8;
      if(ninst > 1){
          install = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
          for(i = 0; i < ninst; i++){
              char item[800];
              snprintf(item, sizeof(item), "%s  -  %s", inst[i].dir, inst[i].summary);
              [install.menu addItemWithTitle:S(item) action:NULL keyEquivalent:@""];
          }
          [install selectItemAtIndex:sel];
          install.target = self;
          install.action = @selector(picked:);
          squeezable(install);
          add_full(game, hstack(@[label(@"Installation:"), install]));
      }
      detected = line_label();
      details = [NSButton buttonWithTitle:@"Details" target:self action:@selector(showDetails:)];
      folder = [NSButton buttonWithTitle:@"Show Folder" target:self action:@selector(showFolder:)];
      folder.toolTip = @"Open the folder the installations are in, in the Finder.";
      add_full(game, hstack(@[detected, details, folder]));
      boxed(outer, @"Game", game, 1); }

    /* Two columns below it.  Left: what the game sounds like and the
     * intro's own options.  Right: how the next session starts, what it
     * records, and where the recording goes. */
    left = vstack();
    right = vstack();
    cols = [NSStackView stackViewWithViews:@[left, right]];
    cols.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    cols.alignment = NSLayoutAttributeTop;
    cols.distribution = NSStackViewDistributionFillEqually;
    cols.spacing = 16;
    add_full(outer, cols);

    empty = NSGridCell.emptyContentView;

    g = grid();
    wSound = [self check:@"Sound on (SoundBlaster 220h / IRQ 7)" on:sound];
    [g addRowWithViews:@[wSound, empty]];
    span(g, 0, 0, 2);
    wQuality = [self popup:quality_labels count:5 sel:quality];
    [g addRowWithViews:@[label(@"Quality:"), wQuality]];
    /* Host gain only, applied where the sound leaves pfemu. */
    wVolume = [NSSlider sliderWithValue:volume minValue:0 maxValue:100
                                 target:self action:@selector(volumeMoved:)];
    wVolume.numberOfTickMarks = 5;
    wVolume.continuous = YES;
    [wVolume.widthAnchor constraintGreaterThanOrEqualToConstant:160].active = YES;
    wVolumeText = label(@"100%");
    [wVolumeText.widthAnchor constraintGreaterThanOrEqualToConstant:36].active = YES;
    [g addRowWithViews:@[label(@"Volume:"), hstack(@[wVolume, wVolumeText])]];
    labels_trailing(g, 0);
    boxed(left, @"Sound", g, 0);

    /* Enhancement: host DSP only (src/sound.c), downstream of -wav.  Flat
     * and Off are the original sound; like volume, never recorded. */
    g = grid();
    wBass = [self popup:eq_labels count:9 sel:eq_db_to_idx(bass)];
    wTreble = [self popup:eq_labels count:9 sel:eq_db_to_idx(treble)];
    [g addRowWithViews:@[label(@"Bass:"), wBass, label(@"Treble:"), wTreble]];
    wOomph = [self popup:oomph_labels count:5 sel:oomph_db_to_idx(oomph)];
    wHeadphone = [self check:@"Headphone mode" on:headphone];
    [g addRowWithViews:@[label(@"Oomph:"), wOomph, wHeadphone, empty]];
    labels_trailing(g, 0);
    labels_trailing(g, 2);
    span(g, 1, 2, 2);
    boxed(left, @"Audio enhancement", g, 0);

    /* Game options, two to a row in PINBALL.CFG order: Balls|Angle,
     * Scrolling|Music, Resolution|Color. */
    g = grid();
    for(i = 0; i < 6; i++)
        wOpt[i] = [self popup:launch_opts[i].values count:launch_opts[i].n sel:cfg[i]];
    for(i = 0; i < 6; i += 2)
        [g addRowWithViews:@[label(S(launch_opts[i].label)), wOpt[i],
                             label(S(launch_opts[i + 1].label)), wOpt[i + 1]]];
    labels_trailing(g, 0);
    labels_trailing(g, 2);
    boxed(left, @"Game options", g, 0);

    /* Extras: the trainer, fullscreen, and where the session starts. */
    g = grid();
    wTrainer = [self check:@"Enable trainer" on:cheatEnable];
    wFullscreen = [self check:@"Start in fullscreen" on:fullscreen];
    [g addRowWithViews:@[hstack(@[wTrainer, wFullscreen]), empty]];
    span(g, 0, 0, 2);
    /* Start at: skip the intro and the menu and boot straight into a
     * table (docs/EMULATOR.md).  Greyed out in replay mode: the .pfr
     * records how its session began and that has to win. */
    wTable = [self popup:table_labels count:5 sel:startTable];
    [g addRowWithViews:@[label(@"Start at:"), wTable]];
    t = [NSTextField wrappingLabelWithString:
            @"Trainer: 1-3 toggle, arrows / Z move the ball. Option+Return or"
             " Ctrl+Cmd+F toggles fullscreen, Cmd+Q quits the game."];
    t.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    t.textColor = NSColor.secondaryLabelColor;
    t.preferredMaxLayoutWidth = 330;
    [g addRowWithViews:@[t, empty]];
    span(g, 2, 0, 2);
    labels_trailing(g, 0);
    [g cellAtColumnIndex:0 rowIndex:0].xPlacement = NSGridCellPlacementLeading;
    [g cellAtColumnIndex:0 rowIndex:2].xPlacement = NSGridCellPlacementLeading;
    boxed(right, @"Extras", g, 0);

    /* Session record / replay (docs/REPLAY.md section 4).  Ranked: record
     * against the canonical state (docs/REPLAY.md, Ranked recordings),
     * which is what the leaderboard accepts.  Off keeps the install's own
     * high-score tables. */
    g = grid();
    modePlay = [NSButton radioButtonWithTitle:@"Play" target:self action:@selector(toggled:)];
    modeRecord = [NSButton radioButtonWithTitle:@"Record" target:self action:@selector(toggled:)];
    modeReplay = [NSButton radioButtonWithTitle:@"Replay" target:self action:@selector(toggled:)];
    rankedCb = [self check:@"Ranked" on:online.ranked];
    row = hstack(@[modePlay, modeRecord, modeReplay, rankedCb]);
    [row setCustomSpacing:20 afterView:modeReplay];
    [g addRowWithViews:@[row, empty, empty]];
    span(g, 0, 0, 3);
    pathLabel = label(@"File:");
    path = [NSTextField textFieldWithString:S(replayPath)];
    path.delegate = self;
    [path.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
    browse = [NSButton buttonWithTitle:@"Replays…" target:self action:@selector(browse:)];
    [g addRowWithViews:@[pathLabel, path, browse]];
    labels_trailing(g, 0);
    [g cellAtColumnIndex:0 rowIndex:0].xPlacement = NSGridCellPlacementLeading;
    boxed(right, @"Session", g, 0);

    /* Leaderboard (pfemu-web/docs/API.md): the account, the recording
     * Submit would send, the last submission's status, and the website for
     * what the launcher does not do - the boards themselves, the password
     * and email, the whole history.  The website's front page is the
     * boards; named for what it shows, not the server's name, which is not
     * meant to stay.  "My account" appears once there is one. */
    { NSStackView *lb = vstack();
      lb.spacing = 8;
      account = line_label();
      loginBtn = [NSButton buttonWithTitle:@"Log In…" target:self action:@selector(loginPressed:)];
      sublistBtn = [NSButton buttonWithTitle:@"Submissions" target:self action:@selector(sublistPressed:)];
      add_full(lb, hstack(@[account, loginBtn, sublistBtn]));
      subfile = line_label();
      submitBtn = [NSButton buttonWithTitle:@"Submit" target:self action:@selector(submitPressed:)];
      add_full(lb, hstack(@[subfile, submitBtn]));
      substate = line_label();
      add_full(lb, substate);
      meLink = link_button(@"My account", self, @selector(mePressed:));
      [lb addArrangedSubview:hstack(@[link_button(@"Leaderboards", self, @selector(boardsPressed:)),
                                      meLink])];
      boxed(right, @"Leaderboard", lb, 1); }

    /* Bottom line: which pfemu this is (the id -verify carries), then Quit
     * and Launch, the default button, where a Mac puts it. */
    t = label(@"Build " PFEMU_BUILD);
    t.textColor = NSColor.secondaryLabelColor;
    t.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    squeezable(t);
    quitBtn = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quit:)];
    launch = [NSButton buttonWithTitle:@"Launch" target:self action:@selector(launchGame:)];
    launch.keyEquivalent = @"\r";
    [launch.widthAnchor constraintGreaterThanOrEqualToConstant:90].active = YES;
    row = hstack(@[t, quitBtn, launch]);
    [row setHuggingPriority:NSLayoutPriorityDefaultLow - 2
             forOrientation:NSLayoutConstraintOrientationHorizontal];
    add_full(outer, row);

    [cv layoutSubtreeIfNeeded];
    [win setContentSize:cv.fittingSize];
    [win center];
    /* AppKit keeps the position from here on, in the app's defaults. */
    [win setFrameAutosaveName:@"pfemu launcher"];
}

/* ------------------------------------------------------------ app menu */
static void build_menu(void){
    NSMenu *bar = [[NSMenu alloc] init], *m;
    NSMenuItem *top, *it;

    top = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
    m = [[NSMenu alloc] initWithTitle:@"pfemu"];
    [m addItemWithTitle:@"Hide pfemu" action:@selector(hide:) keyEquivalent:@"h"];
    it = [m addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:)
               keyEquivalent:@"h"];
    it.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [m addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Quit pfemu" action:@selector(terminate:) keyEquivalent:@"q"];
    top.submenu = m;

    /* Without an Edit menu, Cmd+C and Cmd+V do nothing in a text field. */
    top = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
    m = [[NSMenu alloc] initWithTitle:@"Edit"];
    [m addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    it = [m addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    it.keyEquivalentModifierMask = NSEventModifierFlagShift | NSEventModifierFlagCommand;
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [m addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [m addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [m addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    top.submenu = m;

    top = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
    m = [[NSMenu alloc] initWithTitle:@"Window"];
    [m addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [m addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    top.submenu = m;
    NSApp.windowsMenu = m;

    NSApp.mainMenu = bar;
}

/* ---------------------------------------------------------------- start */
/* GOG.com's Deluxe: src/gog.c finds game.gog and decides whether to offer
 * the import; asking is this window's.  1 when GOG/ was just filled. */
static int gog_offer(const RelResult *found, int n){
    char image[1024], msg[1400], err[800];
    if(!gog_candidate(found, n, image, sizeof(image))) return 0;
    gog_offer_text(msg, sizeof(msg), image);
    if(!alert(NSAlertStyleInformational, @"GOG version found", S(msg), @"Yes", @"No")){
        gog_decline(image);
        return 0;
    }
    if(cdimage_import(image, GOG_DIR, err, sizeof(err)) != 0){
        fprintf(stderr, "[gog] The GOG version could not be imported: %s\n", err);
        alert(NSAlertStyleWarning, @"The GOG version could not be imported.", S(err), nil, nil);
        return 0;
    }
    fprintf(stderr, "[gog] imported %s into %s: %s\n", image, GOG_DIR, err);
    return 1;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    int imported, i;
    /* Run from a source tree the program has no bundle, and so no icon of
     * its own: take the one beside it. */
    if(!NSBundle.mainBundle.bundleIdentifier){
        char icon[1100];
        NSImage *img;
        beside_exe(icon, sizeof(icon), "res/pfemu.png");
        img = [[NSImage alloc] initWithContentsOfFile:S(icon)];
        if(img) NSApp.applicationIconImage = img;
    }
    bring_to_front();
    online_load(&online);
    /* Every directory that holds an INTRO.PRG, identified by content.  GAME
     * comes first when it exists, then the rest alphabetically. */
    ninst = release_scan(inst, MAX_INSTALLS);
    /* Before the window exists: its layout depends on how many there are. */
    imported = gog_offer(inst, ninst);
    if(imported) ninst = release_scan(inst, MAX_INSTALLS);
    /* One that can run, else the first; the one launched last wins when it
     * is still there, and a copy just imported wins over both. */
    for(i = 0; i < ninst; i++)
        if(release_runnable(&inst[i])){ sel = i; break; }
    i = last_pick(inst, ninst);
    if(i >= 0) sel = i;
    if(imported)
        for(i = 0; i < ninst; i++)
            if(!_stricmp(inst[i].dir, GOG_DIR)){ sel = i; break; }

    [self buildWindow];
    [self reloadForDir];
    [self applyModeUI:LAUNCH_PLAY];
    [self updateOnlineUI];
    [win makeKeyAndOrderFront:nil];
    /* Logged in from an earlier run: show where the last submission stands,
     * and follow it if the service is still on it. */
    [self startList:0];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    return YES;
}

/* Cmd+Q, or the last window closed.  The run loop is stopped rather than
 * the process ended, so run_launcher() returns like its GTK twin. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    if(![self confirmQuit]) return NSTerminateCancel;
    [NSApp stop:nil];
    /* stop: takes effect after the next event; make sure there is one. */
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint modifierFlags:0
                                       timestamp:0 windowNumber:0 context:nil
                                         subtype:0 data1:0 data2:0]
             atStart:NO];
    return NSTerminateCancel;
}

@end

/* The launcher, for as long as it is open.  Launch starts the game as a
 * child process and the window stays; this returns when it is closed. */
int run_launcher(void){
    static PFLauncher *delegate;   /* NSApp holds its delegate weakly */
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [[PFLauncher alloc] init];
        NSApp.delegate = delegate;
        build_menu();
        [NSApp run];
    }
    return 0;
}

#endif /* __APPLE__ */
