#!/bin/sh
# Builds "~/Applications/mpv Native.app" from build/mpv/mpv and the library snapshot in this folder.
# Re-run after every rebuild. Its own bundle id and log file, so it lives next to /Applications/mpv.app
# without clashing; both read ~/.config/mpv. Signed ad hoc only (no Apple or work identity).
set -e
D="$(cd "$(dirname "$0")" && pwd)"
APP="$HOME/Applications/mpv Native.app"
TMP="$APP.building"
VER=$(sed -n 's/^#define VERSION "v\(.*\)"$/\1/p' "$D/build/mpv/common/version.h")

rm -rf "$TMP"
cp -R "$D/../TOOLS/osxbundle/mpv.app" "$TMP"
find "$TMP" -name .gitkeep -delete
C="$TMP/Contents"

# Binary and its @executable_path/lib/ libraries (the static libdovi is linked in already)
cp "$D/build/mpv/mpv" "$C/MacOS/mpv"
find "$D/lib" -name '*.dylib' -type f ! -name 'libMoltenVK.dylib' -exec cp {} "$C/MacOS/lib/" \;

# MoltenVK where the Vulkan loader looks inside an app bundle
mkdir -p "$C/Frameworks" "$C/Resources/vulkan/icd.d"
cp "$D/lib/libMoltenVK.dylib" "$C/Frameworks/"
cat > "$C/Resources/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../Frameworks/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
EOF

P="$C/Info.plist"
plutil -replace CFBundleIdentifier -string io.mpv.native "$P"
plutil -replace CFBundleName -string "mpv Native" "$P"
plutil -replace CFBundleShortVersionString -string "$VER" "$P"
plutil -replace LSApplicationCategoryType -string public.app-category.video "$P"
sed -i '' 's|^log-file=.*|log-file=~/Library/Logs/mpv-native.log|' "$C/Resources/mpv.conf"

find "$C/MacOS/lib" "$C/Frameworks" -name '*.dylib' -exec codesign --force -s - {} \; 2>/dev/null
codesign --force -s - "$C/MacOS/mpv"
codesign --force -s - "$TMP"

rm -rf "$APP"
mv "$TMP" "$APP"
echo "built: $APP ($VER)"
