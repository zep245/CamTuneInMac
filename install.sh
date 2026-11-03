#!/bin/bash
# install.sh - installs camtune for this user and starts it (no sudo needed). Run from the folder with camtune in it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
D="$HOME/Library/Application Support/CamTune"
A="$HOME/Library/LaunchAgents/local.camtune.plist"
[ -f "$HERE/camtune" ] || { echo "STOP: no camtune next to install.sh"; exit 1; }
file -b "$HERE/camtune" | grep -q x86_64 || { echo "STOP: camtune is not an x86_64 program"; exit 1; }
mkdir -p "$D" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
launchctl bootout "gui/$(id -u)" "$A" 2>/dev/null || true
cp "$HERE/camtune" "$D/camtune.new" && mv -f "$D/camtune.new" "$D/camtune"
chmod 755 "$D/camtune"; xattr -d com.apple.quarantine "$D/camtune" 2>/dev/null || true
cat > "$A" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>local.camtune</string>
	<key>ProgramArguments</key><array><string>$D/camtune</string></array>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
	<key>ProcessType</key><string>Background</string>
	<key>StandardErrorPath</key><string>$HOME/Library/Logs/camtune.log</string>
	<key>StandardOutPath</key><string>$HOME/Library/Logs/camtune.log</string>
</dict>
</plist>
PL
launchctl bootstrap "gui/$(id -u)" "$A"
sleep 2
echo "installed and running. Settings: $D/camtune.conf"
"$D/camtune" --list || true
