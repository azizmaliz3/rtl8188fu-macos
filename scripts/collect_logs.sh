#!/bin/bash
set -euo pipefail
OUT="rtl8188fu-logs-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
sw_vers > "$OUT/system.txt"
ioreg -p IOUSB -l -w 0 | grep -A 25 -i "f179\|RtlWifi" > "$OUT/usb_ioreg.txt" || true
log show --last 15m --predicate 'eventMessage CONTAINS[c] "RtlWifi"' > "$OUT/kernel_log.txt" 2>/dev/null || true
command -v rtl8188ctl >/dev/null && rtl8188ctl status > "$OUT/status.txt" 2>&1 || true
zip -rq "${OUT}.zip" "$OUT"
rm -rf "$OUT"
echo "Saved sanitized diagnostics to ${OUT}.zip (contains no passwords)."
