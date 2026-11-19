import Cocoa
import IOKit
import Security

let kRtlAbiVersion: UInt32           = 3
let kRtlMethodGetStatus: UInt32      = 0
let kRtlMethodStartScan: UInt32      = 1
let kRtlMethodGetScanResults: UInt32 = 2
let kRtlMethodConnect: UInt32        = 3
let kRtlMethodDisconnect: UInt32     = 4
let kRtlMethodGetDiagnostics: UInt32 = 5

struct RtlWifiStatusRaw {
    var abiVersion: UInt32 = 0
    var state: UInt32 = 0
    var channel: UInt32 = 0
    var rssiDbm: Int32 = -100
    var linkSpeedMbps: UInt32 = 0
    var txPackets: UInt32 = 0
    var txErrors: UInt32 = 0
    var rxPackets: UInt32 = 0
    var rxErrors: UInt32 = 0
    var macAddr: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) = (0,0,0,0,0,0)
    var bssid:   (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) = (0,0,0,0,0,0)
    var ssidLen: UInt8 = 0
    var ht40Active: UInt8 = 0
    var linkQualityPct: UInt8 = 0
    var digGain: UInt8 = 0
    var ssid: (CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,
               CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,
               CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,
               CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar) =
              (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    var pad: (UInt8, UInt8, UInt8) = (0,0,0)
}

struct RtlWifiScanEntryRaw {
    var bssid: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) = (0,0,0,0,0,0)
    var channel: UInt8 = 0
    var security: UInt8 = 0
    var rssiDbm: Int8 = -100
    var ssidLen: UInt8 = 0
    var htCapable: UInt8 = 0
    var reserved: UInt8 = 0
    var ssid: (CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,
               CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,
               CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,
               CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar,CChar) =
              (0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    var pad: (UInt8, UInt8, UInt8) = (0,0,0)
}

struct NetworkItem {
    let ssid: String
    let channel: Int
    let rssi: Int
    let secure: Bool
}

final class KeychainStore {
    private static let service = "io.github.azizmaliz3.RtlWifiMenuBar"
    private static let lastSsidKey = "LastConnectedSSID"

    static var lastSSID: String? {
        get { UserDefaults.standard.string(forKey: lastSsidKey) }
        set { UserDefaults.standard.set(newValue, forKey: lastSsidKey) }
    }

    static func save(password: String, for ssid: String) {
        let data = Data(password.utf8)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid
        ]
        SecItemDelete(query as CFDictionary)
        var item = query
        item[kSecValueData as String] = data
        SecItemAdd(item as CFDictionary, nil)
    }

    static func loadPassword(for ssid: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func forget(ssid: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid
        ]
        SecItemDelete(query as CFDictionary)
        if lastSSID == ssid { lastSSID = nil }
    }
}

final class RtlWifiClient {
    private var conn: io_connect_t = 0

    func open() -> Bool {
        if conn != 0 { return true }
        guard let matching = IOServiceMatching("RtlWifiDriver") else { return false }
        let service = IOServiceGetMatchingService(kIOMasterPortDefault, matching)
        if service == 0 { return false }
        defer { IOObjectRelease(service) }
        return IOServiceOpen(service, mach_task_self_, 0, &conn) == KERN_SUCCESS
    }

    func getStatus() -> RtlWifiStatusRaw? {
        guard open() else { return nil }
        var out = RtlWifiStatusRaw()
        var outSize = MemoryLayout<RtlWifiStatusRaw>.size
        return IOConnectCallStructMethod(conn, kRtlMethodGetStatus, nil, 0, &out, &outSize) == KERN_SUCCESS ? out : nil
    }

    func startScan() {
        guard open() else { return }
        _ = IOConnectCallStructMethod(conn, kRtlMethodStartScan, nil, 0, nil, nil)
    }

    func getScanResults() -> [NetworkItem] {
        guard open() else { return [] }
        let entrySize = MemoryLayout<RtlWifiScanEntryRaw>.size
        var raw = [UInt8](repeating: 0, count: 8 + 32 * entrySize)
        var outSize = raw.count
        guard IOConnectCallStructMethod(conn, kRtlMethodGetScanResults, nil, 0, &raw, &outSize) == KERN_SUCCESS else { return [] }
        return raw.withUnsafeBytes { ptr -> [NetworkItem] in
            let count = min(Int(ptr.load(fromByteOffset: 4, as: UInt32.self)), 32)
            var items: [NetworkItem] = []
            for i in 0..<count {
                let e = ptr.load(fromByteOffset: 8 + i * entrySize, as: RtlWifiScanEntryRaw.self)
                let ssid = withUnsafeBytes(of: e.ssid) { String(decoding: $0.prefix(min(Int(e.ssidLen), 32)), as: UTF8.self) }
                if !ssid.isEmpty {
                    items.append(NetworkItem(ssid: ssid, channel: Int(e.channel), rssi: Int(e.rssiDbm), secure: e.security != 0))
                }
            }
            return items.sorted { $0.rssi > $1.rssi }
        }
    }

    func connect(ssid: String, password: String, secure: Bool) -> Bool {
        guard open() else { return false }
        var buf = [UInt8](repeating: 0, count: 112)
        buf.withUnsafeMutableBytes { ptr in
            ptr.storeBytes(of: kRtlAbiVersion, toByteOffset: 0, as: UInt32.self)
            ptr.storeBytes(of: UInt32(secure ? 1 : 0), toByteOffset: 4, as: UInt32.self)
            let sBytes = Array(ssid.utf8.prefix(32))
            let pBytes = Array(password.utf8.prefix(64))
            ptr.storeBytes(of: UInt8(sBytes.count), toByteOffset: 8, as: UInt8.self)
            ptr.storeBytes(of: UInt8(pBytes.count), toByteOffset: 9, as: UInt8.self)
            ptr.storeBytes(of: UInt8(1), toByteOffset: 10, as: UInt8.self)
            for (i, b) in sBytes.enumerated() { ptr.storeBytes(of: b, toByteOffset: 12 + i, as: UInt8.self) }
            for (i, b) in pBytes.enumerated() { ptr.storeBytes(of: b, toByteOffset: 45 + i, as: UInt8.self) }
        }
        return IOConnectCallStructMethod(conn, kRtlMethodConnect, buf, buf.count, nil, nil) == KERN_SUCCESS
    }

    func disconnect() {
        guard open() else { return }
        _ = IOConnectCallStructMethod(conn, kRtlMethodDisconnect, nil, 0, nil, nil)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private let client = RtlWifiClient()
    private var networks: [NetworkItem] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu

        // Auto-reconnect on wake from macOS sleep
        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(onSystemWake),
            name: NSWorkspace.didWakeNotification, object: nil
        )

        refreshIcon()
        tryAutoConnect()

        Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            self?.refreshIcon()
        }
    }

    @objc private func onSystemWake() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            self?.tryAutoConnect()
        }
    }

    private func tryAutoConnect() {
        guard let st = client.getStatus(), st.state != 3 else { return }
        guard let last = KeychainStore.lastSSID else { return }
        let pass = KeychainStore.loadPassword(for: last) ?? ""
        _ = client.connect(ssid: last, password: pass, secure: !pass.isEmpty)
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
            self?.refreshIcon()
            self?.checkCaptivePortal()
        }
    }

    private func checkCaptivePortal() {
        guard let url = URL(string: "http://captive.apple.com/hotspot-detect.html") else { return }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            if let data = data, let body = String(data: data, encoding: .utf8), !body.contains("Success") {
                DispatchQueue.main.async { NSWorkspace.shared.open(url) }
            }
        }.resume()
    }

    private func refreshIcon() {
        let st = client.getStatus()
        let symbol = (st?.state == 3) ? "wifi" : (st == nil ? "wifi.exclamationmark" : "wifi.slash")
        statusItem.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "RTL8188FU Wi-Fi")
    }

    func menuWillOpen(_ menu: NSMenu) {
        menu.removeAllItems()
        let st = client.getStatus()
        if let st = st {
            let ssid = withUnsafeBytes(of: st.ssid) { String(decoding: $0.prefix(min(Int(st.ssidLen), 32)), as: UTF8.self) }
            if st.state == 3 && !ssid.isEmpty {
                let bw = st.ht40Active != 0 ? "HT40" : "HT20"
                menu.addItem(withTitle: "Connected: \(ssid) (\(st.linkSpeedMbps) Mbps \(bw) · \(st.linkQualityPct)%)", action: nil, keyEquivalent: "")
                menu.addItem(withTitle: "Signal: \(st.rssiDbm) dBm · Ch \(st.channel) · TX \(st.txPackets) / RX \(st.rxPackets)", action: nil, keyEquivalent: "")
                menu.addItem(withTitle: "Disconnect", action: #selector(onDisconnect), keyEquivalent: "")
                menu.addItem(withTitle: "Forget \"\(ssid)\"", action: #selector(onForgetCurrent), keyEquivalent: "")
            } else {
                menu.addItem(withTitle: "RTL8188FU Ready — Not Connected", action: nil, keyEquivalent: "")
            }
        } else {
            menu.addItem(withTitle: "USB Wi-Fi Adapter Not Detected", action: nil, keyEquivalent: "")
        }

        menu.addItem(.separator())
        menu.addItem(withTitle: "Scan for Networks", action: #selector(onRescan), keyEquivalent: "r")
        menu.addItem(.separator())

        networks = client.getScanResults()
        if networks.isEmpty {
            menu.addItem(withTitle: "No networks found yet — click Scan", action: nil, keyEquivalent: "")
        } else {
            for (idx, net) in networks.enumerated() {
                let lock = net.secure ? " [WPA2]" : " [Open]"
                let item = NSMenuItem(title: "\(net.ssid)\(lock)  (\(net.rssi) dBm · Ch \(net.channel))", action: #selector(onSelectNetwork(_:)), keyEquivalent: "")
                item.tag = idx
                menu.addItem(item)
            }
        }

        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit RTL8188FU Wi-Fi", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    }

    @objc private func onRescan() {
        client.startScan()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { [weak self] in self?.refreshIcon() }
    }

    @objc private func onDisconnect() {
        client.disconnect()
        refreshIcon()
    }

    @objc private func onForgetCurrent() {
        if let last = KeychainStore.lastSSID { KeychainStore.forget(ssid: last) }
        client.disconnect()
        refreshIcon()
    }

    @objc private func onSelectNetwork(_ sender: NSMenuItem) {
        guard sender.tag >= 0 && sender.tag < networks.count else { return }
        let net = networks[sender.tag]
        if !net.secure {
            KeychainStore.lastSSID = net.ssid
            _ = client.connect(ssid: net.ssid, password: "", secure: false)
            refreshIcon()
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in self?.checkCaptivePortal() }
            return
        }
        if let saved = KeychainStore.loadPassword(for: net.ssid) {
            KeychainStore.lastSSID = net.ssid
            _ = client.connect(ssid: net.ssid, password: saved, secure: true)
            refreshIcon()
            return
        }
        let alert = NSAlert()
        alert.messageText = "Join \"\(net.ssid)\""
        alert.informativeText = "Enter the WPA2 password (saved securely in macOS Keychain):"
        let input = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        alert.accessoryView = input
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            let pass = input.stringValue
            KeychainStore.save(password: pass, for: net.ssid)
            KeychainStore.lastSSID = net.ssid
            _ = client.connect(ssid: net.ssid, password: pass, secure: true)
            refreshIcon()
        }
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
