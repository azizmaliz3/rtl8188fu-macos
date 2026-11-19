#!/bin/bash
set -euo pipefail
launchctl unload "$HOME/Library/LaunchAgents/io.github.azizmaliz3.RtlWifiMenuBar.plist" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/io.github.azizmaliz3.RtlWifiMenuBar.plist"
pkill -x RtlWifiMenuBar 2>/dev/null || true
rm -rf /Applications/RtlWifiMenuBar.app /usr/local/bin/rtl8188ctl
if [ -d /Library/Extensions/RtlWifiDriver.kext ]; then
    sudo kmutil unload -b io.github.azizmaliz3.RtlWifiDriver 2>/dev/null || true
    sudo rm -rf /Library/Extensions/RtlWifiDriver.kext
fi
echo "Uninstalled cleanly."
