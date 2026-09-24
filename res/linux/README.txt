pfemu for Linux
===============

pfemu plays Pinball Fantasies from the original DOS game files. This is
the Linux build: the same emulator as the Windows one, in an SDL2 window.
Recordings made with it are verified by the leaderboard like any other.

Needs SDL2, which most desktops already have:

    sudo apt install libsdl2-2.0-0        # Debian, Ubuntu
    sudo dnf install SDL2                 # Fedora
    sudo pacman -S sdl2                   # Arch

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

pfemu starts the installation played last, or the first one it finds.

    ./install-desktop-entry.sh

adds pfemu to the application menu.

Keys
----

The game's own keys, plus: Scroll Lock quits, Alt+Enter toggles
fullscreen, F11 saves a screenshot, - and + set the volume, keypad *
mutes.

Settings
--------

There is no launcher on Linux yet. Each installation's settings are in
PFEMU-STATE/pfemu.cfg, a text file. Recording and replaying work from the
command line:

    ./pfemu -nolauncher -d GOG -ranked -record game.pfr
    ./pfemu -nolauncher -d GOG -replay game.pfr

Uploading to the leaderboard needs the Windows launcher for now.

Everything else: https://github.com/mindphluxnet/pfemu
