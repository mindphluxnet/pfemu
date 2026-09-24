# Handoff

State as of 2026-09-23, `main` at `e50929c` plus the ranked golden vector.

Read this with `VERIFY.md`, which is the document this work serves. This file
is the short version plus what to do next.

## Where things stand

**The leaderboard is three repositories.** They sit side by side in
`source/`, and each has its own handoff:

- **pfemu** (this one): the emulator, and `-verify`.
- **pfemu-service**: the validator. It runs on the Mac Mini, is reachable
  only from the web server, and turns a `.pfr` into facts.
- **pfemu-web**: accounts, launcher uploads, the kept recordings, the
  ranking policy and the boards. It runs on another server in the LAN and
  is the only public part.

This repository meets the other two only at the `-verify` object and the
exit code, and the validator pins which pfemu build it runs. This file
covers the emulator side.

The work done before it is finished:

- **Determinism holds.** Two compilers (MSVC, gcc 14.2), two operating
  systems (Windows, Debian), two optimisation levels (`-O2`, `-O1` + ubsan).
  All three golden vectors reproduce a human-played Windows session byte for
  byte: capture hash, every frame and the footer cycle count.
- **The score holds across platforms.** Two complete Party Land games
  recorded on Windows replay under WSL/Debian to the same score, and the
  suite pins both: `deluxe-table1-partyon-295s` at 20,652,570, and the
  ranked `deluxe-table1-ranked-644s` at 33,415,380.
- **A ranked recording is independent of `PFEMU-STATE/`.** `-ranked` plays
  against a fixed canonical state, and `-strict` accepts nothing else
  (REPLAY.md, Ranked recordings).
- **Eligibility is decided and enforced.** An attempt counts only if it ends
  in attract mode on its own, and it must meet the other five conditions in
  `sc_close()` as well. A run that stops mid-table does not count. The vector
  pins `rankable`, not just the number.
- **The verifier's output exists.** `-verify FILE` writes one JSON object:
  a status (`verified` / `mismatch` / `refused`), the recorded-vs-actual
  comparison, every attempt with a reason token, and `best`, the highest
  *rankable* score. It sets the exit code to 0 / 2 / 1 to match. This is
  what the service reads. Nothing else the emulator prints is an interface.
  See the verdict section of VERIFY.md. Since `c24cf6a` it also carries
  `build` (`git describe --dirty` at compile time, via a generated
  `src/build.h`), so each stored verdict names the binary that produced it.
  Since 2026-09-23 every attempt also carries `ball_scores`, its points per
  ball (VERIFY.md, "The verdict"), for the entry pages on pfemu-web. The
  live verdicts only get it after the validator builds this commit and
  `pfweb reverify` runs.
- **The input side is hardened.** The `.pfr` parser bounds every field,
  refuses events stamped past the footer (that used to be an endless run),
  and `-strict` refuses what a verifier should not accept. 27 regression cases
  pass, and 1M mutation cases produced no finding.
- **Replays run 3.3-3.7x real time** with `-unthrottle`, and pacing is
  proven guest-invisible on two hosts.
- **ubsan is clean** on both vectors. It found 32 unaligned guest-RAM
  accesses in `dos.c`/`bios.c` on the first pass, all fixed since. It found
  nothing in `cpu.c`, where the shift-count UB was predicted.

All of it has a test, and CI runs on every push the tests that do not need
game files: build, parser suite, 50k fuzz cases, verdict suite, ubsan
compile, and since 2026-09-24 the SDL2 build.

**A playable Linux build exists (2026-09-24), played under WSLg and on
one real Ubuntu desktop** (see next steps, item 7). `make gui` builds `pfemu` from the headless build's objects plus
`src/host_sdl.c`: an SDL2 window, the keyboard mapped back to PC scan codes,
and waveOut implemented on an SDL audio callback so `sound.c` is unchanged.
**The launcher is GTK 3 (2026-09-24, tried by hand on the Ubuntu desktop,
everything works; the user found the game running even better there than
on Windows):**
`src/launch_gtk.c`, the Win32 window with the Leaderboard group (step 2,
also tried by hand and working), on rules
moved out of `launch.c` into `src/launchcore.c` so both launchers share
them (replay refusal, Details report, record path, labels, `.games`
report). Differences on purpose, listed at the top of `launch_gtk.c`: no
saved position (Wayland), and Delete is a button and uses the Trash.
Two **Win32 launcher bugs** found on the way are fixed in both (tried by
hand on Windows too, 2026-09-24, both work): leaving Replay mode kept the
replay's settings in the window, so a Play launch wrote them to the
install (now `shown_mode` reloads the install's on the way out), and an
install switch kept the previous install's six options and Start at
(`reload_for_dir()` now reads them). Ranked is kept in
`pfemu-online.cfg`, touching only its `ranked=` line. `make gui NOGTK=1`
keeps the old starter in `host_sdl.c` (no launcher; GOG offer, last
install, exec). What moved to make the starter possible:
`write_sound_cfg()`, `pfemu-last.cfg` and `pfemu-winpos.cfg` from `launch.c`
to `cfg.c`, and the GOG search to `src/gog.c`, which covers both hosts. The
Linux search is described in RELEASES.md, "The Linux edition". GOG's Linux
installer ships the same `game.gog` as the Windows one. New flag on both
platforms: `-import IMAGE DIR`. Releases carry `pfemu-linux-x86_64.tar.gz`
(`release.yml`, built inside Ubuntu 22.04 for its older glibc and SDL
2.0.20; CI builds the same on every push) with `install-desktop-entry.sh`
for the application menu. The tarball now needs `libgtk-3-0`,
`libcurl3-gnutls` and `libsecret-1-0` besides SDL2; uploads from Linux are
built (next steps, item 7).

## Verified, and not

| Claim | Status |
| --- | --- |
| MSVC and gcc 14.2 agree byte-for-byte | **Verified**, on all three vectors |
| Two platforms agree on a **score** | **Verified**, on two vectors. `deluxe-table1-ranked-644s`: recorded on Windows/MSVC, replayed under WSL/Debian gcc, 33,415,380 at 3,866,538,359 cycles, same capture hash. `deluxe-table1-partyon-295s`: recorded on Windows/MSVC, replayed on Debian/gcc 14.2, same capture hash, all 15 frames, same 1,773,389,028 cycles, same 20,652,570. This is the claim the whole service rests on |
| A recording **made on Linux** verifies on both | **Verified, 2026-09-24, one game.** Recorded with the SDL2 build under WSLg, `-ranked`, Party Land, 422.5 s, 478 events, 27,531,690 `rankable`. The host fell behind (`fell_behind=26`, 6.8 s over 429 s of wall time) and the sound was poor, which the guest never sees. `-strict -unthrottle -verify` came back `verified` with the same cycles, wav hash and score from `pfemu-headless` (gcc, WSL) and from `pfemu.exe` (MSVC). Pinned as the golden vector `deluxe-table1-linux-ranked-422s`, the first recorded on Linux. A second, from the **Ubuntu desktop** (2026-09-24, launched from the GTK launcher): Party Land, 614.0 s, 605 events, 39,282,110 `rankable`, four launches. It reproduces the footer and wav hash under `pfemu-headless` and is pinned as `deluxe-table1-ubuntu-ranked-614s` |
| The suite catches a wrong score **and a wrong eligibility verdict** | **Verified**, on two vectors. The attempt lines pin `20652570 ... attract yes` and `33415380 ... attract yes`, and `run.sh` runs the verdict replay of a ranked vector under `-strict`. `run.sh` fails the vector if a replay disagrees, or if the ball-counter watchdog fires |
| The verdict object is stable | **Verified** for its own logic. `tests/verify/` has 18 cases against stubs under ASan+UBSan. Every emitted object was also parsed with a real JSON parser, and two deliberate mutations of `verify.c` were caught |
| The verdict matches the report | **Verified** on both hosts. `run.sh` cross-checks the JSON `best` against the `[RANKABLE]` lines the same replay printed: 20,652,570 on both. The ranked vector agrees at 33,415,380 under WSL, under `-strict` |
| Determinism across optimisation levels | **Verified**, incidentally. Both vectors reproduce their capture hash under `-O1` + ubsan |
| Host pacing is guest-invisible | **Verified**, on one vector, on two hosts (`tests/golden/speed-ab.sh`) |
| A replay can run faster than real time | **Verified**: 3.7x on the server, 3.3x under WSL |
| A different **CPU architecture** agrees | **Verified, 2026-09-24, one vector.** `deluxe-table1-ranked-644s` (recorded on Windows/MSVC, x86-64) replayed on a **Raspberry Pi 4** (aarch64, Raspberry Pi OS, gcc, build `c77828d1b4c0`) with the service's invocation, `-strict -unthrottle -verify`: `verified`, same 3,866,538,359 cycles, same wav hash `acbce80e9929def1`, 33,415,380 `rankable`, same ball scores. The Makefile needed nothing for ARM |
| A Raspberry Pi 4 is fast enough to verify | **Yes, measured once.** The same run: 644.4 s emulated in 289 s wall (`time`), 2.2x, against 3.8x on the Mac Mini. `user` was 249 s, so about 40 s went to something else on the Pi; an idle Pi may reach about 2.6x (not measured). One core per replay, so four in parallel is possible in principle; thermal throttling under that load is not measured |
| The `.pfr` parser refuses hostile input | **Verified** for the 27 cases in `tests/fuzz`. 14 of the first 22 were accepted by the previous parser; the other 5 cover the `state:` line |
| The `.pfr` parser is memory-safe | **No finding**, which is weaker than verified. 1,000,004 mutation cases under ASan+UBSan, no crash. A hand-rolled gcc mutator is not a coverage-guided campaign |
| Shift counts >= width in `cpu.c` | **Looked for properly, not found**, in the code the two vectors execute. `TABLE2`, `TABLE4`, the intro and three other releases are unexercised |
| `-strict -unthrottle -verify` together | **Verified** through the service, on the Mac Mini under bwrap: the Party Land vector came back `verified` and `rankable` at 20,652,570, build `3f9b265d674c` from a clean clone, 78.2 s wall for 295.6 s emulated (3.8x). The install was mounted read-only, so the replay needs no write access to it |
| `-ffp-contract=off` is *necessary* | **Not demonstrated.** A justified precaution: `-ffp-contract=fast` emitted 50 FMAs and changed nothing |
| Big-endian correctness | **Untested.** The memory helpers are host-endian, like the puns they replaced |
| A replay is independent of the verifier's `PFEMU-STATE/` | **False, measured 2026-09-23.** Party Land vector, Deluxe: with `table1.hi` as recorded it verifies at 20,652,570. With every entry at 99,999,999 it mismatches, and the attempt ends at 175.3 s with 17,128,800. With the file missing it mismatches at 7,125,000. Same inputs, a different game. `replay.c` only warns when the overlay hash differs. **Not the name entry at game end:** with the high table the `-scoredbg` trace is identical up to t=67 s, and the first difference is a score event 1 ms early at t=72.58 s, on ball 1 at 4.55M, far from any table entry. With the file missing it diverges at t=6.6 s. The table reaches the game *during* play. A guess, not checked in the game code: a compare against the table whose cycle cost depends on its contents |
| A **ranked** replay is independent of the verifier's `PFEMU-STATE/` | **Verified, 2026-09-23, on a played game.** `deluxe-table1-ranked-644s`: a complete Party Land game, 644 s, recorded on Windows with `-ranked`. It was replayed under WSL/Debian with `-strict` against five verifier states: the installed one, every `table1.hi` entry at 99,999,999, `table1.hi` missing, a high `TABLE1.HI` in the install directory itself, and `SOUND.CFG` set to no sound. All five came back `verified`, with the same capture hash and 33,415,380 `rankable`. The control is a headless 40 s boot through the intro, recorded without `-ranked` and replayed against the same states. It mismatched on all three table changes and was refused on the sound change, so these states do change the game. The same boot recorded with `-ranked` verified in all five |
| `-keys` combined with `-record` replays | **No, found 2026-09-23.** A headless recording driven by `-keys` mismatches on replay (capture hash), ranked or not, with every event injected and the footer matching. The same recording without `-keys` verifies. Human recordings are unaffected: both golden vectors verify. Not investigated. Suspect that the keyscript and the logger see different cycles, or that something the keyscript does is not logged |

## What to do next, in order

0. **Move the service to ranked recordings.** The blocker is solved in
   this repository. A ranked recording no longer depends on anyone's
   `PFEMU-STATE/`. The design decision (2026-09-23) was the **canonical
   state**, not the state embedded in the `.pfr`. Embedding would have let
   the player pick a high-score table, and the table changes the game
   during play. See [Ranked recordings](REPLAY.md#ranked-recordings):
   `-record FILE -ranked` plays against a fixed overlay that holds only a
   `SOUND.CFG` derived from the header, and writes `state: canonical-1`.
   The replay rebuilds the same overlay without reading the operator's
   `PFEMU-STATE/`. The DOS layer hides the install's `SOUND.CFG`,
   `PINBALL.CFG` and `*.HI`. `-strict` refuses anything else with
   `state_not_canonical`. The golden vector `deluxe-table1-ranked-644s`
   is a played game, and it verifies against five different verifier
   states (table above). What is left is outside this repository or on the
   launcher:
   - **Replace the service's smoke vector** with
     `deluxe-table1-ranked-644s`. The Party Land vector has no `state:`
     line, so the Mac Mini's `-strict` run refuses it by design as soon as
     it builds a commit from after `e50929c`. Push first.
   - **`pfemu-web/docs/API.md`** needs the new refusal `state_not_canonical`
     and what to tell the player ("record it as a ranked run").
   - **The validator has to build `e50929c` or later before a ranked
     file can verify.** An older build ignores the `state:` line, replays
     the file against its own `PFEMU-STATE/`, and so mismatches wherever
     the high-score tables matter. **Done:** the Mac Mini runs `e50929c`
     (user, 2026-09-24). The one verdict change since is `5a40ee6`
     (`ball_scores`); moving `PFEMU_REF` to v1.6 (`6456cd0`) would add it
     and changes nothing a replay does.

1. **The service** lives in `pfemu-service` and `pfemu-web`; their HANDOFFs
   have the order. From this repository they need two things:
   - **Leave the `-verify` object alone.** It is an interface: add fields,
     never rename or remove them, and bump `pfemu_verify` if that ever has
     to happen. Push before the Mac Mini builds, because it pins a commit
     that has to exist on `origin`.
   - **The launcher upload is built, and a person has not tried it
     yet.** `src/online.c` is the client for `pfemu-web/docs/API.md`
     over WinHTTP: register, log in, the token encrypted with DPAPI in
     `pfemu-online.cfg`, upload, poll every 10 s, the list, and the
     `reason` table. `launch.c` has the Leaderboard group, a login
     window and the **Ranked** checkbox (default on). A console test
     against the client passed 22 checks, including live calls against
     `https://pf.dark-secrets.eu`: GET, 401 on a bad token, a POST with a
     body, and an unreachable host. The windows themselves, and a real
     upload, are still to be tried by hand.
   - **Submit asks when an upload would change nothing** (2026-09-23,
     not tried by hand yet). A recording session turns the score hooks on
     and writes `<file>.pfr.games` at exit; Submit holds it against
     `standings` from `/api/v1/me` and asks when no three-ball game beats
     the player's best on its table (REPLAY.md, Launcher). The status line
     lists every table a submission ranked on (pfemu-web ranks each table
     of a session since `179c5b2`) and the queue position while it waits.
     Record mode now logs the `[score]` lines too.
   - **The launcher is two columns now** (2026-09-23, not tried by hand
     yet): Game across the top, Sound / Audio enhancement / Game options
     on the left, Extras / Session / Leaderboard on the right, the window
     sized from the layout in `WM_CREATE`. Submissions is a list view
     (`sublist_proc`) with Copy and a website button; the Leaderboard group
     has links to the site and to `/me`, opened only when the server is an
     http(s) address.
   - **The Replays window** (2026-09-23, not tried by hand yet) replaced
     Browse in play/replay mode: `show_replays()` lists `sessions\*.pfr`
     with per-row Submit/Delete cells, matches uploads by SHA-256
     (`release_hash_file()`), and deletes to the Recycle Bin. Submitting
     now takes a path (`start_submit_check(h, st, path)`), and message
     boxes about requests go to `ui_owner()` so they never re-enable the
     launcher under a modal window. The scores column prefers the
     verified result's games; a launcher replay that completes writes a
     missing `.games` (`run.c`, `replay_completed()`), so old recordings
     fill in after one replay.
   - **The launcher no longer closes.** Launch starts the game as a
     child process (`-nolauncher -launched ...`), the launcher waits and
     comes back to the front when the game ends, with the fresh
     recording ready to submit. The in-process `goto relaunch` path in
     `run.c` is gone: a refusal now shows its box and ends the child,
     and the launcher is still there. One process per session is also
     what keeps state from one session out of the next recording.

2. **A real fuzzing campaign, before the service takes uploads from
   strangers.** This is the only item on the list with a deadline attached.
   `make fuzz-clang` on a host with clang, left running for hours, against a
   corpus of more than one vector. The current "no finding" comes from a blind
   mutator, and that kind of mutator rediscovers shallow structure and stops.
   It does not need to block building the service, only opening it.

3. **More vectors, on the tables and releases nothing covers.** This moves
   several things at once: ubsan coverage (`TABLE2`, `TABLE4`, the intro, the
   floppy/A releases), the fuzz corpus for item 2, and the cross-platform
   score evidence, which rests on one complete game. Recording one is cheap:
   the capture hash is automatic, and `mkexpected.sh` does the rest.
   **A match-fires vector is the one to keep if it turns up**, but it is no
   longer worth hunting for. The PARTY ON return in the Party Land vector
   already covers that shape of test: an exact equality on the score that
   changes the rest of the game.

4. **CI: the golden suite needs a decision.** Everything that runs without
   game files already runs on every push. `run.sh` and `speed-ab.sh` need an
   installation, which is deliberately not in the repository. The options
   are a self-hosted runner that already has an install, or a small
   synthetic guest program committed as a fixture. *Smaller:* there is no
   Windows job on push, so a POSIX-side change can break the MSVC build
   unnoticed until a release tag.

5. **A vector that reaches the PIT and VGA phase math hard**, to turn
   `-ffp-contract=off` from a precaution into a demonstrated necessity, or
   to show it is unneeded. The flag stays either way.

6. **Optional:** make the memory helpers explicitly little-endian. Correct
   in principle, but unobservable on any host we build for.

7. **The Linux build: plays cleanly on a real desktop (2026-09-24).**
   Under WSLg it was playable but the sound crackled, stalled and jumped:
   `SDL_RenderPresent` blocked up to 70 ms and WSLg's sink asked with gaps
   up to 97 ms, which sound.c's twelve buffers (about 144 ms at 21 kHz)
   plus the 64 ms lead turned into underruns and `audio_drops`. On real
   hardware (Ubuntu, GNOME on Wayland, GTX 960) a 92 s run had one 2 ms
   underrun, `audio_drops=0`, gaps of at most 26.5 ms and `fell_behind=0`,
   and the user heard it as perfect. The same machine showed that SDL2
   takes X11 even in a Wayland session and then dies in Xlib when GLX is
   broken (`glxinfo` failed there too), software renderer included;
   `plat_early_init()` now picks Wayland in a Wayland session, and `./pfemu`
   starts there with no errors and no environment variables. A ring of
   about 250 ms in `waveOutWrite()` stays the idea for a machine that
   needs more slack. v1.6 (2026-09-24) is the first release with the
   Linux tarball; unpacked on that Ubuntu machine, set up and launched
   fine, and Alt+Enter fullscreen and F11 screenshots work there too.
   A `-ranked` recording made there (Party Land, 299.5 s, 417 events,
   17,386,520) came back `verified` and `rankable` under `-strict` from
   both `pfemu-headless` and `pfemu.exe`, with the same cycles and wav hash.
   **The launcher is GTK 3** (the user's choice, 2026-09-24), in two
   steps. Step 1, done: everything but the Leaderboard group (see the
   status paragraph above), tried by hand on the Ubuntu desktop on
   2026-09-24 and everything worked (user). The user recorded a session
   with it there for step 2 (`sessions/GOG_20260924_104612.pfr`, Party
   Land, 614 s, 39,282,110): it verifies under `-strict` from
   `pfemu-headless` with the recorded cycles and wav hash, and is the
   golden vector `deluxe-table1-ubuntu-ranked-614s`.
   **Step 2, done, tried by hand on 2026-09-24 and everything worked
   (user):** the Leaderboard group, the
   login window, the Submissions window, the submit check against
   `/api/v1/me`, polling, and the Replays window's Leaderboard column and
   Submit button. `online.c` now has a POSIX half: libcurl (GnuTLS build)
   for HTTP and libsecret for the token. The user chose the keyring over a
   0600 file (2026-09-24). One keyring item per server holds
   "username\ntoken"; `pfemu-online.cfg` keeps only `server=` and
   `ranked=` there and leaves a Windows build's lines alone. Without a
   keyring (WSLg has none) the login lasts until the launcher closes, and
   the status line says so. What an answer means moved to `launchcore.c`
   too (`sub_describe`, `sl_row`, `submit_check`, `login_body`,
   `read_recording`); `online_save()` returns whether the login was kept.
   A console test of the libcurl client passed 7 checks, among them live
   calls against `https://pf.dark-secrets.eu` (401 on a bad token, a POST
   with a body), an unreachable host and an unknown name. The Windows
   build was relinked after the move; the user tried the Win32 launcher
   after it, and the two Win32 fixes above, by hand (2026-09-24): both
   work.
8. **Videos of replays (started 2026-09-24).** `-video` and
   `tools/render-video.sh` exist in this repository ([Videos](REPLAY.md#videos));
   the plan the user agreed to is: first pfemu and measurements, then
   render jobs in pfemu-service (`POST /v1/renders` with the `.pfr`, queued
   behind every verification, run in the same sandbox), then pfemu-web's
   `dispatch` pulling the file (`GET /v1/renders/<id>/video`, size and hash
   checked) and keeping it beside the recordings. The validator never
   pushes and holds no credentials. The render run can carry `-verify` as
   well, and pfemu-web keeps the video only when that verdict matches the
   stored one. Measured so far, all on the Windows PC under WSL
   (`deluxe-table1-linux-ranked-422s`):
   - `-verify` with and without `-video` is identical apart from the wall
     time, and so is `-wav`. Two runs wrote the same frame stream and
     soundtrack (sha256).
   - Emulation with `-video` to `/dev/null` at 640x480: 133.1 s against
     126.2 s without, 8 s of it in `src/video.c`.
   - x264 `veryfast` CRF 20 at 640x480: 5.0x real time on one thread, 8.4x
     on two, so one extra core outruns the emulator. 52 MB for 422 s,
     about 7 MB a minute.
   - A frame from the MP4 and a lossless `-shot` at the same moment look
     the same, the DMD included.
   **Not measured: the Mac Mini.** Its line would be
   `tools/render-video.sh tests/golden/deluxe-table1-ranked-644s.pfr
   /tmp/party.mp4 -d <install>` with ffmpeg installed, and
   `NOVIDEO=1` for the baseline. The Pi 4 is 2.2x on emulation alone and
   x264 is slow there; not a video machine without measuring.
   **Separate finding, not acted on:** `wsplit` puts 47 s of the 126 s
   baseline in `other`. The outer loop ends every round with
   `plat_sleep_ms(1)`, `-unthrottle` included, and the present-phase
   break makes that one round per guest frame: 25,000 sleeps. Skipping
   the sleep under `-unthrottle` should make every verification
   noticeably faster; it is host pacing, so guest-invisible by the same
   argument as `-unthrottle` itself, but it needs the speed A/B to show it.

## Running the gate

The headless build may be run directly - it opens no window and no audio
device. From the repo root under Git Bash:

    wsl make && wsl sh tests/golden/run.sh          # the gate, ~50 min: real time, each vector twice
    wsl sh tests/golden/speed-ab.sh                 # pacing A/B, ~4 minutes
    wsl make fuzz && wsl ./pfemu-fuzz-pfr -selftest # parser, milliseconds
    wsl make verify-test && wsl ./pfemu-verify-test # verdict, milliseconds
    cmd //c ".\build.bat"                            # MSVC; note //c, Git Bash eats /c

The ubsan pass has its own script, because it must **not** go through
`run.sh`: that deletes its work directory on exit and would take the stderr
you want with it.

    wsl sh tests/golden/ubsan.sh                    # builds, runs every vector

It replays every `.pfr` in `tests/golden/` into `/tmp/pfemu-ubsan/<vector>/`,
keeps the logs, and prints the unique `runtime error` lines across all of
them. It also says whether each vector still reproduced its capture hash
under `-O1` + instrumentation, which is a small extra piece of evidence on
top of the bug hunt.

This used to be a multi-line snippet to paste. Do not go back to that: in
`cmd.exe` it is split line by line, the first fragment is an unterminated
bash command, and the result is that nothing happens and nothing explains
why. The snippet also carried the repo root in `$OLDPWD`, which a loop's own
`cd` overwrites after the first vector.

`make ubsan` builds `pfemu-headless-ubsan` and deletes its object files on the
way out, so the two configurations no longer contaminate each other and no
follow-up `make clean` is needed.

## Traps, all of them paid for once already

- **A blank Linux window under WSLg titled "WARNING: COPY MODE" is WSLg,
  not pfemu.** It happens when WSLg could not open its shared memory at
  start (`rdp_allocate_shared_memory: Failed` in `/mnt/wslg/weston.log`).
  Frames and keys never cross over, although SDL reports the window shown
  and focused. Check that log before debugging `host_sdl.c`;
  `wsl --shutdown` starts WSLg again. WSLg sound goes over RDP and is known
  to crackle (microsoft/wslg#1429). pfemu's own side of it is in the
  `[sdl] audio:` exit line.
- **Never delete `<install>/PFEMU-STATE/`.** It holds `SOUND.CFG`; without it
  the guest prints a DOS error and terminates about 1.3 emulated seconds in,
  which then surfaces as some confusing downstream failure.
- **Always pass `-freezetime`** for any run compared byte-for-byte.
- **`tests/golden/*.pfr` are `-text` in `.gitattributes`.** A `.pfr` is hashed
  over its own raw disk bytes, so end-of-line conversion on checkout refuses
  the file. The first commit of the vector was normalised on the way in and
  was already broken.
- **Line endings are mixed in this repo** and `core.autocrlf` is `true`, and
  not along the line you would guess. The C sources are CRLF. `Makefile`,
  `docs/VERIFY.md`, `docs/REPLAY.md` and this file are LF. `README.md` and
  `docs/EMULATOR.md` are **CRLF** - so "the docs are LF" is wrong, and a
  patch script written on that assumption fails to match anything, which is
  the good outcome. Check each file with `file` before editing it; a
  scripted edit that gets this wrong turns the whole file into the diff.
- **Running the golden suite needs the game files.** `run.sh` takes an
  installation path, or reads `PFEMU_INSTALL`. Its default, `FANTASYDX`, is
  gone from this checkout: the Deluxe here is `GOG/`. **Give it the binary
  as an absolute path.** It changes directory before each run, so
  `./pfemu-headless` finds nothing, and every check fails with empty output
  that looks like a total divergence.
- **A wall of undefined `__ubsan_handle_*` at link time is a stale-object
  problem, not a source problem.** Instrumented `.o` files relinked without
  `-fsanitize=undefined` produce it, and it reads like the code is broken when
  it is not. `make clean && make` clears it. The Makefile now keeps the two
  builds apart so it should not recur; if it does, check
  `nm -u src/run.o | grep ubsan` before suspecting the source.
- **A remote checkout is only as new as `origin`.** The Mac Mini once ran
  the gate green on a stale checkout, because the commits under test had
  not been pushed. If a vector with a score shows no `verdict` line, the
  checkout is out of date, and the run is a pass for an older commit, not
  this one. Push, pull, `make`, then run.
- **Updating the validator is `PFEMU_REF`, a rebuild and a reverify.**
  The container builds pfemu itself from `PFEMU_REF` in
  `pfemu-service/docker/.env`. A `git pull` and `make` in a checkout on
  the Mac Mini, and a container restart, change nothing it runs. So: set
  `PFEMU_REF` to a pushed commit, `docker compose up -d --build`, add the
  new build to `builds = [...]` in `pfweb.toml` if that list is set, then
  `docker compose run --rm dispatch reverify` on the web server. Nothing
  triggers the reverify by itself (decided 2026-09-23: the validator is
  rarely rebuilt). Until then a kept recording keeps the old build's
  result, and resubmitting the same file does not change that.
- **An `.expected` is generated, never typed.** `mkexpected.sh` refuses to
  overwrite a differing file and leaves `.expected.new` beside it, because
  an expected value that changed on its own is a finding. Read the diff
  and move the file into place by hand only when the change is intended.
- **A WSL build of this checkout says `-dirty`.** `core.autocrlf` is
  `true` on the Windows side, so WSL-git sees every CRLF file as modified,
  and `src/build.h` gets `<hash>-dirty` even on a clean tree. The service
  withholds ranking from a dirty build, correctly. A clean Linux clone,
  which is what the Mac Mini builds from, does not have this problem.
- **`sed -i` under Git Bash turns a CRLF file into LF**, the whole file.
  `grep -c $'\r$'` under Git Bash still counts every line as CRLF
  afterwards, so it cannot catch this. `file` and git's "LF will be
  replaced by CRLF" warning do. `unix2dos` puts it back. Use the editor
  tool or WSL for C sources instead.
- **Write patch scripts to files, not heredocs**, when an agent edits this
  repo through a shell tool. A `\n` inside a heredoc came out as a literal
  newline or a bare `n` more than once. That produced a broken C string
  once, and once a shell line that `sh -n` accepted but that would have
  handed `n` to grep as a filename.
