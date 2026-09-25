/* AppKit launcher: the Mac face of the launcher.
 *
 * The same window as src/launch_gtk.c, on AppKit instead of GTK, and the
 * same rules, because both call src/launchcore.c for every decision that
 * matters: which replay may run against which installation, what Details
 * says, where a recording goes.  AppKit is part of every Mac and this file
 * is plain Objective-C, so the Command Line Tools' clang builds it; there
 * is no Xcode project, no nib and no storyboard.
 *
 * This is the first of two steps.  Here: the installation, sound, audio
 * enhancement, the game options, the trainer, fullscreen, Start at, and
 * Play, Record and Replay, with the Details report.  Not yet: the
 * Leaderboard group, the login and the Replays window.  src/online.c is
 * already linked (src/launchcore.c reads its answers with it), with the
 * Keychain as its token store, and the Ranked checkbox is saved through
 * it.
 *
 * Differences from the GTK launcher, each on purpose:
 *
 *   - pfemu.app works in ~/Library/Application Support/pfemu, not beside
 *     the program (mac_app_home(), src/posix.c), so Show Folder opens that
 *     folder in the Finder: it is hidden there otherwise.
 *   - Replay picks its file with the system's Open panel, in sessions/.
 *   - The window position is AppKit's own (setFrameAutosaveName:).
 *   - Setting a control from code sends no action in AppKit, so there is
 *     no "updating" guard: every action here is the user's.
 *
 * The game still runs as a child process, this program again with
 * -nolauncher -launched, for the reason src/launch.c gives: nothing from one
 * session may reach the next recording.  The child never touches AppKit
 * itself; SDL does, in the child.
 */
#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
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

@interface PFLauncher : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate>
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
    OnlineCfg online;        /* only Ranked, until the Leaderboard group */
    int quitConfirmed;
    NSWindow *win, *detailsWin;
    NSString *detailsText;
    NSButton *detailsCopy;
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

- (void)setPathText {
    path.stringValue = S(replayPath);
}

/* The detection line, with the whole text as its tooltip: a record target
 * is long, and the label ends in an ellipsis where the window does. */
- (void)setDetected:(const char *)text {
    detected.stringValue = S(text);
    detected.toolTip = S(text);
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
 * either mode with it on as a backstop. */
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
    /* Record picks a target file; otherwise the button picks a recording
     * and switches to Replay for it. */
    browse.title = mode == LAUNCH_RECORD ? @"Browse…" : @"Open…";
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
    detailsWin = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 780, 540)
                                            styleMask:NSWindowStyleMaskTitled |
                                                      NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
    detailsWin.releasedWhenClosed = NO;
    detailsWin.title = @"pfemu - details";
    detailsWin.delegate = self;
    cv = detailsWin.contentView;
    sv = [NSTextView scrollableTextView];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.borderType = NSBezelBorder;
    tv = (NSTextView *)sv.documentView;
    tv.editable = NO;
    tv.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    tv.textContainerInset = NSMakeSize(4, 4);
    tv.string = detailsText;
    detailsCopy = [NSButton buttonWithTitle:@"Copy" target:self action:@selector(copyDetails:)];
    detailsCopy.translatesAutoresizingMaskIntoConstraints = NO;
    close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(closeDetails:)];
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
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:detailsText forType:NSPasteboardTypeString];
    detailsCopy.title = @"Copied";
}

- (void)closeDetails:(id)sender {
    [NSApp stopModal];
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

/* Replay: a recording, from sessions/ unless the current one is elsewhere.
 * Picking one in Play mode switches to Replay. */
- (void)pickReplay {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    UTType *t = pfr_type();
    char dir[1100], abs[PATH_MAX];
    const char *b = base_name(replayPath);
    LaunchMode prev = mode;
    p.title = @"Replay";
    p.canChooseFiles = YES;
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    if(t) p.allowedContentTypes = @[t];
    snprintf(dir, sizeof(dir), "%.*s", (int)(b - replayPath), replayPath);
    if(!dir[0] || mode != LAUNCH_REPLAY) snprintf(dir, sizeof(dir), RL_DIR);
    if(realpath(dir, abs) || realpath(".", abs))
        p.directoryURL = [NSURL fileURLWithPath:S(abs) isDirectory:YES];
    if([p runModal] != NSModalResponseOK || !p.URL) return;
    snprintf(replayPath, sizeof(replayPath), "%s", p.URL.fileSystemRepresentation);
    pathCustom = 1;
    [self setPathText];
    if(mode != LAUNCH_REPLAY){
        mode = LAUNCH_REPLAY;
        [self applyModeUI:prev];
    }
    [self replayLoadFile];
    [self reloadForDir];
}

/* ------------------------------------------------------------- the game */
/* The game ended.  Come back to the front. */
- (void)childDone {
    launch.title = @"Launch";
    if(childMode == LAUNCH_RECORD && childPath[0] && mode == LAUNCH_RECORD){
        /* The next recording gets a fresh name: a finished session is a
         * playthrough that cannot be reproduced, and aiming the next one
         * at the same file would overwrite it. */
        pathCustom = 0;
        [self restoreSessionPath:0];
    }
    [self showDetection];
    bring_to_front();
    [win makeKeyAndOrderFront:nil];
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
         * stay findable (Open... lists *.pfr). */
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
    else [self pickReplay];
}

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
    if(w == detailsWin) return YES;
    return [self confirmQuit];
}

- (void)windowWillClose:(NSNotification *)n {
    if(n.object == detailsWin) [NSApp stopModal];
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
      detected = label(@"");
      detected.lineBreakMode = NSLineBreakByTruncatingTail;
      squeezable(detected);
      details = [NSButton buttonWithTitle:@"Details" target:self action:@selector(showDetails:)];
      folder = [NSButton buttonWithTitle:@"Show Folder" target:self action:@selector(showFolder:)];
      folder.toolTip = @"Open the folder the installations are in, in the Finder.";
      add_full(game, hstack(@[detected, details, folder]));
      boxed(outer, @"Game", game, 1); }

    /* Two columns below it.  Left: what the game sounds like and the
     * intro's own options.  Right: how the next session starts and what it
     * records. */
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
    browse = [NSButton buttonWithTitle:@"Browse…" target:self action:@selector(browse:)];
    [g addRowWithViews:@[pathLabel, path, browse]];
    labels_trailing(g, 0);
    [g cellAtColumnIndex:0 rowIndex:0].xPlacement = NSGridCellPlacementLeading;
    boxed(right, @"Session", g, 0);

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

    /* Without an Edit menu, Cmd+C and Cmd+V do nothing in the File field. */
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
    [win makeKeyAndOrderFront:nil];
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
