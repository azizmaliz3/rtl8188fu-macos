import Cocoa
import IOKit
import Security

let kRtlAbiVersion: UInt32           = 3
let kRtlMethodGetStatus: UInt32      = 0
let kRtlMethodStartScan: UInt32      = 1
let kRtlMethodGetScanResults: UInt32 = 2
let kRtlMethodConnect: UInt32        = 3
let kRtlMethodDisconnect: UInt32     = 4

struct RtlWifiStatus {
    let state: UInt32
    let channel: UInt32
    let rssiDbm: Int32
    let linkSpeedMbps: UInt32
    let txPackets: UInt32
    let txErrors: UInt32
    let rxPackets: UInt32
    let rxErrors: UInt32
    let ht40Active: Bool
    let linkQualityPct: UInt8
    let ssid: String
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
    private let lock = NSLock()
    private var conn: io_connect_t = 0

    private func closeLocked() {
        if conn != 0 {
            IOServiceClose(conn)
            conn = 0
        }
    }

    private func openLocked() -> Bool {
        if conn != 0 { return true }
        guard let matching = IOServiceMatching("RtlWifiDriver") else { return false }
        let service = IOServiceGetMatchingService(0, matching)
        if service == 0 { return false }
        defer { IOObjectRelease(service) }
        return IOServiceOpen(service, mach_task_self_, 0, &conn) == KERN_SUCCESS
    }

    private func callLocked(selector: UInt32,
                            input: [UInt8]? = nil,
                            outputSize: Int = 0) -> [UInt8]? {
        for attempt in 0..<2 {
            guard openLocked() else { return nil }
            var outBuf = [UInt8](repeating: 0, count: max(outputSize, 0))
            var outCount = outBuf.count
            let kr: kern_return_t
            if let inp = input {
                kr = inp.withUnsafeBytes { inPtr in
                    if outputSize > 0 {
                        return IOConnectCallStructMethod(conn, selector, inPtr.baseAddress, inp.count, &outBuf, &outCount)
                    } else {
                        return IOConnectCallStructMethod(conn, selector, inPtr.baseAddress, inp.count, nil, nil)
                    }
                }
            } else {
                if outputSize > 0 {
                    kr = IOConnectCallStructMethod(conn, selector, nil, 0, &outBuf, &outCount)
                } else {
                    kr = IOConnectCallStructMethod(conn, selector, nil, 0, nil, nil)
                }
            }
            if kr == KERN_SUCCESS {
                return outBuf
            }
            // Stale handle after USB unplug/replug: close and retry once
            closeLocked()
            if attempt == 1 { return nil }
        }
        return nil
    }

    func getStatus() -> RtlWifiStatus? {
        lock.lock(); defer { lock.unlock() }
        guard let raw = callLocked(selector: kRtlMethodGetStatus, outputSize: 88), raw.count == 88 else { return nil }
        return raw.withUnsafeBytes { p -> RtlWifiStatus? in
            let abi = p.load(fromByteOffset: 0, as: UInt32.self)
            guard abi == kRtlAbiVersion else { return nil }
            let state   = p.load(fromByteOffset: 4, as: UInt32.self)
            let channel = p.load(fromByteOffset: 8, as: UInt32.self)
            let rssi    = p.load(fromByteOffset: 12, as: Int32.self)
            let speed   = p.load(fromByteOffset: 16, as: UInt32.self)
            let txPkts  = p.load(fromByteOffset: 20, as: UInt32.self)
            let txErrs  = p.load(fromByteOffset: 24, as: UInt32.self)
            let rxPkts  = p.load(fromByteOffset: 28, as: UInt32.self)
            let rxErrs  = p.load(fromByteOffset: 32, as: UInt32.self)
            let ssidLen = min(Int(p.load(fromByteOffset: 48, as: UInt8.self)), 32)
            let ht40    = p.load(fromByteOffset: 49, as: UInt8.self) != 0
            let qual    = p.load(fromByteOffset: 50, as: UInt8.self)
            let ssidBytes = raw[52..<(52 + ssidLen)]
            let ssid = String(decoding: ssidBytes, as: UTF8.self)
            return RtlWifiStatus(state: state, channel: channel, rssiDbm: rssi,
                                 linkSpeedMbps: speed, txPackets: txPkts, txErrors: txErrs,
                                 rxPackets: rxPkts, rxErrors: rxErrs, ht40Active: ht40,
                                 linkQualityPct: qual, ssid: ssid)
        }
    }

    func startScan() {
        lock.lock(); defer { lock.unlock() }
        _ = callLocked(selector: kRtlMethodStartScan)
    }

    func getScanResults() -> [NetworkItem] {
        lock.lock(); defer { lock.unlock() }
        let entrySize = 48
        let totalSize = 8 + 32 * entrySize
        guard let raw = callLocked(selector: kRtlMethodGetScanResults, outputSize: totalSize), raw.count == totalSize else {
            return []
        }
        return raw.withUnsafeBytes { p -> [NetworkItem] in
            let abi = p.load(fromByteOffset: 0, as: UInt32.self)
            guard abi == kRtlAbiVersion else { return [] }
            let count = min(Int(p.load(fromByteOffset: 4, as: UInt32.self)), 32)
            var items: [NetworkItem] = []
            for i in 0..<count {
                let base = 8 + i * entrySize
                let ch   = Int(p.load(fromByteOffset: base + 6, as: UInt8.self))
                let sec  = p.load(fromByteOffset: base + 7, as: UInt8.self) != 0
                let rssi = Int(p.load(fromByteOffset: base + 8, as: Int8.self))
                let slen = min(Int(p.load(fromByteOffset: base + 9, as: UInt8.self)), 32)
                let ssid = String(decoding: raw[(base + 12)..<(base + 12 + slen)], as: UTF8.self)
                if !ssid.isEmpty {
                    items.append(NetworkItem(ssid: ssid, channel: ch, rssi: rssi, secure: sec))
                }
            }
            return items.sorted { $0.rssi > $1.rssi }
        }
    }

    func connect(ssid: String, password: String, secure: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        var buf = [UInt8](repeating: 0, count: 112)
        buf.withUnsafeMutableBytes { ptr in
            ptr.storeBytes(of: kRtlAbiVersion, toByteOffset: 0, as: UInt32.self)
            ptr.storeBytes(of: UInt32(secure ? 1 : 0), toByteOffset: 4, as: UInt32.self)
            let sBytes = Array(ssid.utf8.prefix(32))
            let pBytes = Array(password.utf8.prefix(64))
            ptr.storeBytes(of: UInt8(sBytes.count), toByteOffset: 8, as: UInt8.self)
            ptr.storeBytes(of: UInt8(pBytes.count), toByteOffset: 9, as: UInt8.self)
            ptr.storeBytes(of: UInt8(0), toByteOffset: 10, as: UInt8.self)
            for (i, b) in sBytes.enumerated() { ptr.storeBytes(of: b, toByteOffset: 12 + i, as: UInt8.self) }
            for (i, b) in pBytes.enumerated() { ptr.storeBytes(of: b, toByteOffset: 45 + i, as: UInt8.self) }
        }
        return callLocked(selector: kRtlMethodConnect, input: buf) != nil
    }

    func disconnect() {
        lock.lock(); defer { lock.unlock() }
        _ = callLocked(selector: kRtlMethodDisconnect)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private let client = RtlWifiClient()
    private let bgQueue = DispatchQueue(label: "io.github.azizmaliz3.rtlwifi.bg", qos: .userInitiated)
    private var networks: [NetworkItem] = []
    private var isScanning = false
    private var isConnecting = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu

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
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { [weak self] in
            self?.tryAutoConnect()
        }
    }

    private func tryAutoConnect() {
        bgQueue.async { [weak self] in
            guard let self = self else { return }
            guard let st = self.client.getStatus(), st.state != 3 else { return }
            guard let last = KeychainStore.lastSSID else { return }
            let pass = KeychainStore.loadPassword(for: last) ?? ""
            _ = self.client.connect(ssid: last, password: pass, secure: !pass.isEmpty)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                self.refreshIcon()
                self.checkCaptivePortal()
            }
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
        bgQueue.async { [weak self] in
            guard let self = self else { return }
            let st = self.client.getStatus()
            DispatchQueue.main.async {
                let symbol = (st?.state == 3) ? "wifi" : (st == nil ? "wifi.exclamationmark" : "wifi.slash")
                self.statusItem.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "RTL8188FU Wi-Fi")
            }
        }
    }

    func menuWillOpen(_ menu: NSMenu) {
        menu.removeAllItems()
        let st = client.getStatus()
        if let st = st {
            if st.state == 3 && !st.ssid.isEmpty {
                let bw = st.ht40Active ? "HT40" : "HT20"
                menu.addItem(withTitle: "Connected: \(st.ssid) (\(st.linkSpeedMbps) Mbps \(bw) · \(st.linkQualityPct)%)", action: nil, keyEquivalent: "")
                menu.addItem(withTitle: "Signal: \(st.rssiDbm) dBm · Ch \(st.channel) · TX \(st.txPackets) / RX \(st.rxPackets)", action: nil, keyEquivalent: "")
                menu.addItem(withTitle: "Disconnect", action: #selector(onDisconnect), keyEquivalent: "")
                menu.addItem(withTitle: "Forget \"\(st.ssid)\"", action: #selector(onForgetCurrent), keyEquivalent: "")
            } else if isConnecting || st.state == 2 {
                menu.addItem(withTitle: "Connecting...", action: nil, keyEquivalent: "")
            } else if st.state == 4 {
                menu.addItem(withTitle: "Reconnecting automatically...", action: nil, keyEquivalent: "")
            } else {
                menu.addItem(withTitle: "RTL8188FU Ready — Not Connected", action: nil, keyEquivalent: "")
            }
        } else {
            menu.addItem(withTitle: "USB Wi-Fi Adapter Not Detected", action: nil, keyEquivalent: "")
        }

        menu.addItem(.separator())
        let scanTitle = isScanning ? "Scanning..." : "Scan for Networks"
        let scanItem = NSMenuItem(title: scanTitle, action: isScanning ? nil : #selector(onRescan), keyEquivalent: "r")
        menu.addItem(scanItem)
        menu.addItem(withTitle: "Join Hidden / Other Network...", action: #selector(onJoinOther), keyEquivalent: "")
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
        guard !isScanning else { return }
        isScanning = true
        bgQueue.async { [weak self] in
            guard let self = self else { return }
            self.client.startScan()
            let results = self.client.getScanResults()
            DispatchQueue.main.async {
                self.networks = results
                self.isScanning = false
                self.refreshIcon()
            }
        }
    }

    @objc private func onDisconnect() {
        bgQueue.async { [weak self] in
            self?.client.disconnect()
            self?.refreshIcon()
        }
    }

    @objc private func onForgetCurrent() {
        if let last = KeychainStore.lastSSID { KeychainStore.forget(ssid: last) }
        onDisconnect()
    }

    @objc private func onJoinOther() {
        let alert = NSAlert()
        alert.messageText = "Join Hidden / Other Network"
        alert.informativeText = "Enter the network name (SSID) and optional WPA2 password:"
        let view = NSView(frame: NSRect(x: 0, y: 0, width: 260, height: 58))
        let ssidField = NSTextField(frame: NSRect(x: 0, y: 32, width: 260, height: 24))
        ssidField.placeholderString = "Network Name (SSID)"
        let passField = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        passField.placeholderString = "Password (leave blank for Open)"
        view.addSubview(ssidField)
        view.addSubview(passField)
        alert.accessoryView = view
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            let ssid = ssidField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !ssid.isEmpty else { return }
            let pass = passField.stringValue
            connectAsync(ssid: ssid, password: pass, secure: !pass.isEmpty, saveOnSuccess: true)
        }
    }

    private func connectAsync(ssid: String, password: String, secure: Bool, saveOnSuccess: Bool) {
        isConnecting = true
        bgQueue.async { [weak self] in
            guard let self = self else { return }
            let ok = self.client.connect(ssid: ssid, password: password, secure: secure)
            DispatchQueue.main.async {
                self.isConnecting = false
                if ok {
                    KeychainStore.lastSSID = ssid
                    if saveOnSuccess && secure && !password.isEmpty {
                        KeychainStore.save(password: password, for: ssid)
                    }
                    self.refreshIcon()
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { self.checkCaptivePortal() }
                } else {
                    if !saveOnSuccess && secure {
                        KeychainStore.forget(ssid: ssid)
                    }
                    self.refreshIcon()
                    let err = NSAlert()
                    err.messageText = "Could not connect to \"\(ssid)\""
                    err.informativeText = "Check that the password is correct and the router is within range."
                    err.runModal()
                }
            }
        }
    }

    @objc private func onSelectNetwork(_ sender: NSMenuItem) {
        guard sender.tag >= 0 && sender.tag < networks.count else { return }
        let net = networks[sender.tag]
        if !net.secure {
            connectAsync(ssid: net.ssid, password: "", secure: false, saveOnSuccess: false)
            return
        }
        if let saved = KeychainStore.loadPassword(for: net.ssid), !saved.isEmpty {
            connectAsync(ssid: net.ssid, password: saved, secure: true, saveOnSuccess: false)
            return
        }
        let alert = NSAlert()
        alert.messageText = "Join \"\(net.ssid)\""
        alert.informativeText = "Enter the WPA2 password (saved in macOS Keychain after connecting):"
        let input = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        alert.accessoryView = input
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            connectAsync(ssid: net.ssid, password: input.stringValue, secure: true, saveOnSuccess: true)
        }
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
