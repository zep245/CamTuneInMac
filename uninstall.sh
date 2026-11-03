#!/bin/bash
# uninstall.sh - stops and removes camtune (the camera keeps its last values until it is unplugged or restarted)
A="$HOME/Library/LaunchAgents/local.camtune.plist"
launchctl bootout "gui/$(id -u)" "$A" 2>/dev/null || true
rm -f "$A" "$HOME/Library/Application Support/CamTune/camtune"
echo "camtune removed (settings file kept in ~/Library/Application Support/CamTune)"
