#!/bin/sh
# Installs ~/Applications/SDL PDF Viewer.app so PDFs can be opened from Finder's
# "Open With" menu. Re-run after rebuilding: the app holds a copy of the binary.
#
# The viewer takes its file from argv, but Finder delivers files through an Apple
# Event, so the bundle is an AppleScript applet that forwards them to the binary.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
binary="$repo/build/sdl-pdf-viewer"
app="$HOME/Applications/SDL PDF Viewer.app"
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

if [ ! -x "$binary" ]; then
    echo "error: $binary not found; build the project first" >&2
    exit 1
fi

mkdir -p "$HOME/Applications"
rm -rf "$app"

osacompile -o "$app" <<'EOF'
on launchViewer(theFiles)
	set viewer to quoted form of (POSIX path of (path to me) & "Contents/Resources/sdl-pdf-viewer")
	repeat with f in theFiles
		do shell script viewer & " " & quoted form of POSIX path of f & " >/dev/null 2>&1 &"
	end repeat
end launchViewer

on open theFiles
	launchViewer(theFiles)
end open

on run
	launchViewer(choose file of type {"com.adobe.pdf"} with multiple selections allowed)
end run
EOF

cp "$binary" "$app/Contents/Resources/sdl-pdf-viewer"

icon_tmp=$(mktemp -d)
swift "$repo/tools/make_iconset.swift" "$repo/assets/sdl-pdf-viewer.svg" "$icon_tmp/AppIcon.iconset"
iconutil -c icns -o "$app/Contents/Resources/AppIcon.icns" "$icon_tmp/AppIcon.iconset"
rm -rf "$icon_tmp"
# The applet's stock icon comes from both of these files.
rm -f "$app/Contents/Resources/droplet.icns" "$app/Contents/Resources/Assets.car"

plist="$app/Contents/Info.plist"
plutil -replace CFBundleIconFile -string "AppIcon" "$plist"
plutil -remove CFBundleIconName "$plist"
plutil -replace CFBundleIdentifier -string "local.sdl-pdf-viewer" "$plist"
plutil -replace CFBundleName -string "SDL PDF Viewer" "$plist"
# Alternate rank: listed under Open With without competing to be the default.
plutil -replace CFBundleDocumentTypes -json '[{
  "CFBundleTypeName": "PDF document",
  "CFBundleTypeRole": "Viewer",
  "LSHandlerRank": "Alternate",
  "LSItemContentTypes": ["com.adobe.pdf"]
}]' "$plist"

# Editing the bundle invalidates the applet's signature; arm64 requires a valid one.
codesign --force --deep --sign - "$app"
touch "$app" # prompts Finder to refresh its cached icon
"$lsregister" -f "$app"

echo "Installed $app"
