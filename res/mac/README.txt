pfemu for macOS
===============

pfemu plays Pinball Fantasies from the original DOS game files. This is
the Mac build: the same emulator as the Windows and Linux ones, for Intel
and Apple Silicon Macs with macOS 11 or newer. Recordings made with it
replay the same everywhere.

The game files are not included.

The first start
---------------

pfemu is free and not registered with Apple, so macOS refuses to open it
the first time ("Apple could not verify ..."). To open it anyway:

1. Move pfemu.app to Applications (or wherever you keep programs).
2. Open it once. macOS says it cannot; click Done (or OK).
3. Open System Settings > Privacy & Security, scroll down to
   "pfemu was blocked", and click Open Anyway.
4. Confirm with Open Anyway and your password.

From then on it opens like any other program.

Where the game goes
-------------------

pfemu keeps everything in

    ~/Library/Application Support/pfemu

The Finder hides the Library folder; the launcher's Show Folder button
opens this one.

- GOG.com: if Pinball Fantasies Deluxe is installed with Heroic, pfemu
  offers on its first start to copy the game into a folder named GOG
  there. The GOG installation is not changed.
- Other releases: put the files directly in a folder there, GAME for
  example (GAME/INTRO.PRG, not GAME/FANTASY/INTRO.PRG), and open pfemu
  again.

The launcher opens on the installation played last. Launch starts the
game; the launcher stays open behind it.

Keys
----

The game's own keys, plus:

    Cmd+Q                 quit the game
    Option+Return         fullscreen on and off (Ctrl+Cmd+F too)
    Cmd+P                 screenshot
    Cmd+S / Cmd+L         save / load the snapshot slot
    - and +               volume
    Cmd+0                 mute and back
    Cmd+E                 audio enhancement off and back
    Cmd+1 ... Cmd+4       F1-F4, for keyboards without them
    Cmd+,                 F5, the game's options menu

macOS uses Ctrl+Up and Ctrl+Down for Mission Control. Flip with Shift or
Option, or turn those shortcuts off in System Settings > Keyboard >
Keyboard Shortcuts > Mission Control.

Settings
--------

The launcher sets sound, audio enhancement, the game options, the trainer,
fullscreen and where the game starts, for each installation. They are kept
in the installation's PFEMU-STATE/pfemu.cfg, a text file.

Session: Play, Record (Ranked records what the leaderboard accepts) or
Replay. Recordings go to the sessions folder; Replays... lists them, shows
what each one holds, replays one, and moves the ones you delete to the
Trash.

Leaderboard: log in (or register) in the launcher, then Submit sends the
last ranked recording, or the one picked in Replays. The launcher follows
it until the server has verified it. The login is kept in your Keychain;
after an update to pfemu, macOS may ask once whether pfemu may use it
(Always Allow).

Everything else: https://github.com/mindphluxnet/pfemu
