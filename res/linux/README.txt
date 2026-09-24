pfemu for Linux
===============

pfemu plays Pinball Fantasies from the original DOS game files. This is
the Linux build: the same emulator as the Windows one, in an SDL2 window,
with a GTK 3 launcher. Recordings made with it are verified by the
leaderboard like any other.

Needs SDL2, GTK 3, libcurl and libsecret, which most desktops already have:

    sudo apt install libsdl2-2.0-0 libgtk-3-0 libcurl3-gnutls libsecret-1-0   # Debian, Ubuntu
    sudo dnf install SDL2 gtk3 libcurl libsecret                             # Fedora
    sudo pacman -S sdl2 gtk3 curl libsecret                                  # Arch

The game files are not included.

Starting
--------

    ./pfemu

pfemu works in the folder it is started from.

- GOG.com: install Pinball Fantasies Deluxe with GOG's installer, Heroic or
  Minigalaxy. On the first start pfemu offers to copy the game into a
  folder named GOG here. The GOG installation is not changed.
- Other releases: put the files directly in a folder here, GAME for
  example (GAME/INTRO.PRG, not GAME/FANTASY/INTRO.PRG).
- A Deluxe CD image pfemu did not find by itself:
      ./pfemu -import path/to/game.gog GOG

The launcher opens on the installation played last, or the first one it
finds. Launch starts the game; the launcher stays open behind it.

    ./install-desktop-entry.sh

adds pfemu to the application menu.

Keys
----

The game's own keys, plus: Scroll Lock quits, Alt+Enter toggles
fullscreen, F11 saves a screenshot, - and + set the volume, keypad *
mutes.

Settings
--------

The launcher sets sound, audio enhancement, the game options, the trainer,
fullscreen and where the game starts, for each installation. They are kept
in the installation's PFEMU-STATE/pfemu.cfg, a text file.

Session: Play, Record (Ranked records what the leaderboard accepts) or
Replay. Recordings go to sessions/ here; Replays... lists them, shows what
each one holds, replays one, and moves the ones you delete to the Trash.
The command line does the same without the launcher:

    ./pfemu -nolauncher -d GOG -ranked -record game.pfr
    ./pfemu -nolauncher -d GOG -replay game.pfr

Leaderboard: log in (or register) in the launcher, then Submit sends the
last ranked recording, or the one picked in Replays. The launcher follows
it until the server has verified it. The login is kept in the desktop's
keyring (GNOME Keyring, KWallet); without one it lasts until the launcher
closes.

Everything else: https://github.com/mindphluxnet/pfemu
