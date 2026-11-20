#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_OUT="/Applications/RtlWifiMenuBar.app"

echo "=== RTL8188FU Ultimate macOS Installer (V29.4) ==="
mkdir -p "$APP_OUT/Contents/MacOS"
swiftc -O -target x86_64-apple-macos11.0 "$REPO_ROOT/app/RtlWifiMenuBar.swift" \
    -framework Cocoa -framework IOKit -framework Security \
    -o "$APP_OUT/Contents/MacOS/RtlWifiMenuBar"

cat > "$APP_OUT/Contents/Info.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>RtlWifiMenuBar</string>
    <key>CFBundleIdentifier</key><string>io.github.azizmaliz3.RtlWifiMenuBar</string>
    <key>CFBundleName</key><string>RTL8188FU Wi-Fi</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.5.0</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
PLIST

chmod +x "$APP_OUT/Contents/MacOS/RtlWifiMenuBar"
codesign --force --deep --sign - "$APP_OUT"

# Install CLI tool
swiftc -O -target x86_64-apple-macos11.0 "$REPO_ROOT/tools/rtl8188ctl.swift" -framework IOKit -o /tmp/rtl8188ctl
sudo mv /tmp/rtl8188ctl /usr/local/bin/rtl8188ctl

# Add Login LaunchAgent so menu bar starts automatically like Windows Wi-Fi tray
mkdir -p "$HOME/Library/LaunchAgents"
cat > "$HOME/Library/LaunchAgents/io.github.azizmaliz3.RtlWifiMenuBar.plist" << 'AGENT'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>io.github.azizmaliz3.RtlWifiMenuBar</string>
    <key>ProgramArguments</key><array><string>/Applications/RtlWifiMenuBar.app/Contents/MacOS/RtlWifiMenuBar</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
</dict>
</plist>
AGENT
launchctl load -w "$HOME/Library/LaunchAgents/io.github.azizmaliz3.RtlWifiMenuBar.plist" 2>/dev/null || open "$APP_OUT"
echo "Done! Click the Wi-Fi icon in your menu bar."
