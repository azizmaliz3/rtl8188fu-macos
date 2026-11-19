#!/bin/bash
set -euo pipefail
# Builds a double-clickable macOS .pkg installer for non-technical users
STAGE="/tmp/rtl8188fu_pkg_stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/Applications" "$STAGE/usr/local/bin"
cp -R /Applications/RtlWifiMenuBar.app "$STAGE/Applications/"
cp /usr/local/bin/rtl8188ctl "$STAGE/usr/local/bin/"
pkgbuild --root "$STAGE" \
    --identifier io.github.azizmaliz3.rtl8188fu.pkg \
    --version 0.4.0 \
    --install-location / \
    RTL8188FU-WiFi-Installer-0.4.0.pkg
echo "Created RTL8188FU-WiFi-Installer-0.4.0.pkg"
