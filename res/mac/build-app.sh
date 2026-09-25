#!/bin/sh
# make app: pfemu.app, the Mac download.
#
# One program for Intel and Apple Silicon (lipo), built once for each with
# the Command Line Tools' clang for macOS 11 and newer - the first macOS that
# runs on Apple Silicon - with SDL2.framework inside the bundle, and the
# whole bundle signed ad hoc (codesign -s -).  An ad-hoc signature has no
# developer behind it and costs nothing; Apple Silicon runs nothing
# unsigned.  Gatekeeper still stops a downloaded copy that Apple has not
# notarized, and notarizing needs the paid developer programme, so the first
# start of a download is System Settings > Privacy & Security > Open Anyway
# (res/mac/README.txt).
#
# SDL2.framework is SDL's own, from its release .dmg, in ~/Library/Frameworks
# or /Library/Frameworks - where `make gui` takes it from.  It holds both
# architectures.  Homebrew's SDL is a dylib for one, so it cannot go in.
#
# Each architecture is built from clean, this Mac's own last, so src/ and
# ./pfemu end up as a plain `make gui` leaves them.
set -eu
cd "$(dirname "$0")/../.."

fw=
for d in "$HOME/Library/Frameworks" /Library/Frameworks; do
    if [ -d "$d/SDL2.framework" ]; then fw=$d/SDL2.framework; break; fi
done
if [ -z "$fw" ]; then
    echo "make app: SDL2.framework is not in ~/Library/Frameworks or /Library/Frameworks." >&2
    echo "Copy it there from SDL2-2.x.dmg (https://github.com/libsdl-org/SDL/releases)." >&2
    exit 1
fi
fwdir=$(dirname "$fw")

here=$(uname -m)
if [ "$here" = arm64 ]; then other=x86_64; else other=arm64; fi
archs=${ARCHS:-"$other $here"}
for a in $archs; do
    if ! lipo "$fw/SDL2" -verify_arch "$a" 2>/dev/null; then
        echo "make app: $fw has no $a code." >&2
        exit 1
    fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for a in $archs; do
    echo "== $a"
    make clean >/dev/null
    make gui CC="cc -arch $a -mmacosx-version-min=11.0"
    cp pfemu "$tmp/pfemu-$a"
done

app=pfemu.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
lipo -create -output "$app/Contents/MacOS/pfemu" "$tmp"/pfemu-*
# The bundle's own SDL only: the folder this Mac keeps it in means nothing
# on another.
install_name_tool -delete_rpath "$fwdir" "$app/Contents/MacOS/pfemu" 2>/dev/null || true
ditto "$fw" "$app/Contents/Frameworks/SDL2.framework"

# The icon, from the same 256-pixel picture as the Windows and Linux ones.
iconset="$tmp/pfemu.iconset"
mkdir "$iconset"
for s in 16 32 128; do
    sips -z $s $s res/pfemu.png --out "$iconset/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z $d $d res/pfemu.png --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
done
cp res/pfemu.png "$iconset/icon_256x256.png"
iconutil -c icns -o "$app/Contents/Resources/pfemu.icns" "$iconset"

# Versions: the release's for people (PFEMU_VERSION, which the release job
# takes from its tag, else the newest tag here), the build id -verify
# reports for everything else.
short=${PFEMU_VERSION:-}
[ -n "$short" ] || short=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//') || short=
[ -n "$short" ] || short=0
build=$(git describe --always --dirty --abbrev=12 2>/dev/null) || build=unknown
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                  <string>pfemu</string>
    <key>CFBundleDisplayName</key>           <string>pfemu</string>
    <key>CFBundleIdentifier</key>            <string>org.pfemu.pfemu</string>
    <key>CFBundleExecutable</key>            <string>pfemu</string>
    <key>CFBundleIconFile</key>              <string>pfemu</string>
    <key>CFBundlePackageType</key>           <string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>CFBundleShortVersionString</key>    <string>$short</string>
    <key>CFBundleVersion</key>               <string>$build</string>
    <key>LSMinimumSystemVersion</key>        <string>11.0</string>
    <key>LSApplicationCategoryType</key>     <string>public.app-category.arcade-games</string>
    <key>NSHighResolutionCapable</key>       <true/>
    <key>NSPrincipalClass</key>              <string>NSApplication</string>
</dict>
</plist>
EOF

# SDL signs its framework; one that is not signed gets an ad-hoc signature
# of its own, since a bundle can only be signed over signed parts.
fwin="$app/Contents/Frameworks/SDL2.framework"
codesign --verify "$fwin" 2>/dev/null || codesign --force --sign - "$fwin"
codesign --force --sign - "$app"
codesign --verify --strict "$app"

echo "built $app ($(lipo -archs "$app/Contents/MacOS/pfemu"), $build)"
