#!/bin/sh
# Builds the Mac download: build/release/VirtuaFighter3Recomp-mac.zip with the app (universal,
# macOS 11+, ad-hoc signed: players right-click > Open the first time), README.txt and LICENSES.txt.
# Nothing from the ROMs goes in it: the player chooses their own vf3.zip at the first launch. No
# pictures are shipped (the widescreen sides are the game's own, carried on).
set -e
cd "$(dirname "$0")/../.."
tools/mac/build_release.sh
APP="build/release/Virtua Fighter 3 Recomp.app"
rm -rf build/release && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/assets"
cp build/rel/vf3 "$APP/Contents/MacOS/Virtua Fighter 3 Recomp"
strip -S -x "$APP/Contents/MacOS/Virtua Fighter 3 Recomp"   # (no debug info: it holds the build's own paths)
cp -R assets/ui assets/fonts "$APP/Contents/Resources/assets/"
cp assets/icon/VF3.icns "$APP/Contents/Resources/VF3.icns"
VERSION=${VF3_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}   # (VF3_VERSION=v0.1.0 for a release)
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Virtua Fighter 3 Recomp</string>
	<key>CFBundleDisplayName</key><string>Virtua Fighter 3 Recomp</string>
	<key>CFBundleExecutable</key><string>Virtua Fighter 3 Recomp</string>
	<key>CFBundleIdentifier</key><string>recomp.vf3.unofficial</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleIconFile</key><string>VF3</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
	<key>GCSupportsControllerUserInteraction</key><true/>
	<key>NSLocalNetworkUsageDescription</key><string>Online play: to host a match or join one on your network.</string>
</dict>
</plist>
PLIST
codesign --force --deep -s - "$APP"
cp tools/release/README.txt build/release/README.txt
tools/release/licenses.sh build/release/LICENSES.txt
# one zip holding the app folder and the two text files
(cd build/release && rm -f VirtuaFighter3Recomp-mac.zip && mkdir -p pkg/"Virtua Fighter 3 Recomp" && \
 cp -R "Virtua Fighter 3 Recomp.app" README.txt LICENSES.txt pkg/"Virtua Fighter 3 Recomp"/ && \
 ditto -c -k --norsrc --keepParent "pkg/Virtua Fighter 3 Recomp" VirtuaFighter3Recomp-mac.zip && rm -rf pkg)
ls -la build/release
