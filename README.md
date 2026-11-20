# rtl8188fu-macos (V28 Ultimate)

Production-grade macOS Wi-Fi driver (`RtlWifiDriver.kext`), native menu-bar app (`RtlWifiMenuBar.app`), CLI (`rtl8188ctl`), and `.pkg` installer for **Realtek RTL8188FU / RTL8188FTV (`0bda:f179`)**.

## V28 Ultimate Capabilities
- **Zero-Copy Dual-Pipe WMM TX + Control-Plane Fast Lane:** 32-slot BE + 16-slot VO/VI async USB rings, single-copy `mbuf_copydata()` straight into DMA buffers, USB 512-byte ZLP guard, and ARP/DHCP/DNS fast-lane prioritization.
- **Full 802.11n Throughput:** H2C firmware rate adaptation (MCS0–MCS7 + SGI), real HT40+/HT40- channel programming from AP HT Operation IEs, QoS A-MPDU + ADDBA, 16-URB RXDMA aggregation, A-MSDU de-aggregation, and a **64-slot RX A-MPDU sequence reorder window**.
- **24/7 Stability & Security:** Live hardware PHY RSSI decoding + DIG, silicon BSSID filtering (`REG_BSSID` + `RCR`), full post-association GTK rekeying, PMK caching, Open + WPA2-PSK support, `read_random()` SNonce, `IOCommandGate` serialization, `setPowerState()` sleep/wake, and `IONetworkStats` Activity Monitor telemetry.
- **Human-Friendly macOS UX:** `RtlWifiMenuBar.app` with macOS Keychain auto-join on boot/wake, captive portal detection, `rtl8188ctl` CLI, `.pkg` builder (`scripts/build_pkg.sh`), and sanitized `scripts/collect_logs.sh`.
