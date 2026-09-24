#!/bin/sh
# Put pfemu in the desktop's application menu, for the folder this script
# is in.  pfemu looks for game installations and keeps its settings in the
# folder it is started from, so the entry starts it there.
#
#   ./install-desktop-entry.sh            add or update the entry
#   ./install-desktop-entry.sh --remove   take it out again
#
# Run it again after moving the folder.  Nothing outside
# ~/.local/share/applications is touched.
set -eu

dir=$(cd "$(dirname "$0")" && pwd)
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
file=$apps/pfemu.desktop

if [ "${1:-}" = "--remove" ]; then
    rm -f "$file"
    echo "removed $file"
    exit 0
fi

[ -x "$dir/pfemu" ] || { echo "no pfemu next to this script, in $dir" >&2; exit 1; }

mkdir -p "$apps"
# StartupWMClass matches the window SDL opens, so the dock shows this icon.
cat > "$file" <<EOF
[Desktop Entry]
Type=Application
Name=pfemu
GenericName=Pinball Fantasies
Comment=Play Pinball Fantasies from the original DOS files
Exec="$dir/pfemu"
Path=$dir
Icon=$dir/pfemu.png
Terminal=false
Categories=Game;ArcadeGame;
StartupWMClass=pfemu
EOF
echo "added $file"
