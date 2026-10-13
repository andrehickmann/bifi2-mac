#!/bin/zsh
# Build "Bi-Fi 2.app": binary, bundled libraries, game data and icon.
# Usage: ./bundle.sh <installed game dir containing BIFI/> [output dir]
set -euo pipefail
cd "$(dirname "$0")"
GAME_ROOT=${1:?installed game directory (containing BIFI/)}
OUT=${2:-build}
APP="$OUT/Bi-Fi 2.app"

make
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/game"

cp build/bifi2 "$APP/Contents/MacOS/bifi2"
# The icon is game artwork and therefore not in the repository; drop your own in assets/.
[[ -f assets/BiFi2.icns ]] && cp assets/BiFi2.icns "$APP/Contents/Resources/"

UNICORN=$(brew --prefix unicorn)/lib/libunicorn.2.dylib
SDL2=$(brew --prefix sdl2-compat)/lib/libSDL2-2.0.0.dylib
SDL3=$(brew --prefix sdl3)/lib/libSDL3.0.dylib
ADPLUG=$(otool -L build/bifi2 | awk '/libadplug/{print $1}')
BINIO=$(otool -L "$ADPLUG" | awk '/libbinio/{print $1}')
cp "$UNICORN" "$APP/Contents/Frameworks/libunicorn.2.dylib"
cp "$SDL2" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
cp "$SDL3" "$APP/Contents/Frameworks/libSDL3.dylib"
cp "$ADPLUG" "$APP/Contents/Frameworks/$(basename "$ADPLUG")"
cp "$BINIO" "$APP/Contents/Frameworks/$(basename "$BINIO")"
chmod u+w "$APP"/Contents/Frameworks/*.dylib

install_name_tool -id @rpath/libunicorn.2.dylib "$APP/Contents/Frameworks/libunicorn.2.dylib"
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -id @rpath/libSDL3.dylib "$APP/Contents/Frameworks/libSDL3.dylib"
FW="$APP/Contents/Frameworks"
install_name_tool -id "@rpath/$(basename "$ADPLUG")" "$FW/$(basename "$ADPLUG")"
install_name_tool -id "@rpath/$(basename "$BINIO")" "$FW/$(basename "$BINIO")"
install_name_tool -change "$BINIO" "@rpath/$(basename "$BINIO")" "$FW/$(basename "$ADPLUG")"
install_name_tool -add_rpath @loader_path "$FW/$(basename "$ADPLUG")" 2>/dev/null || true
BIN="$APP/Contents/MacOS/bifi2"
install_name_tool -change "$(otool -L "$BIN" | awk '/libunicorn/{print $1}')" @rpath/libunicorn.2.dylib "$BIN"
install_name_tool -change "$(otool -L "$BIN" | awk '/libSDL2/{print $1}')" @rpath/libSDL2-2.0.0.dylib "$BIN"
install_name_tool -change "$ADPLUG" "@rpath/$(basename "$ADPLUG")" "$BIN"
install_name_tool -change "$BINIO" "@rpath/$(basename "$BINIO")" "$BIN"
install_name_tool -add_rpath @executable_path/../Frameworks "$BIN"

# game data: installed files, sound configured for Sound Blaster 2.0
cp -R "$GAME_ROOT/BIFI" "$APP/Contents/Resources/game/"
rm -f "$APP/Contents/Resources/game/BIFI/.DS_Store"
cp assets/PSMCFG4.DAT "$APP/Contents/Resources/game/BIFI/PSMCFG4.DAT"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Bi-Fi 2</string>
  <key>CFBundleDisplayName</key><string>Bi-Fi 2 – Action in Hollywood</string>
  <key>CFBundleIdentifier</key><string>local.bifi2.host</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleExecutable</key><string>bifi2</string>
  <key>CFBundleIconFile</key><string>BiFi2</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.adventure-games</string>
</dict>
</plist>
PLIST

codesign --force --sign - "$APP/Contents/Frameworks/"*.dylib
codesign --force --sign - "$APP"
echo "built $APP"
