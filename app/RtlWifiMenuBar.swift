import Cocoa
import IOKit
import Security

let kRtlAbiVersion: UInt32         = 3
let kRtlMaxSsidLen: Int            = 32
let kRtlMaxPassLen: Int            = 64
let kRtlMaxScanCount: Int          = 32

let kRtlMethodGetStatus: UInt32      = 0
let kRtlMethodStartScan: UInt32      = 1
let kRtlMethodGetScanResults: UInt32 = 2
let kRtlMethodConnect: UInt32        = 3
let kRtlMethodDisconnect: UInt32     = 4
let kRtlMethodGetDiagnostics: UInt32 = 5

let kRtlStateDisconnected: UInt32  = 0
let kRtlStateScanning: UInt32      = 1
let kRtlStateConnecting: UInt32    = 2
let kRtlStateConnected: UInt32     = 3
let kRtlStateRecovering: UInt32    = 4

let kRtlSecOpen: UInt32            = 0
let kRtlSecWpa2Psk: UInt32         = 1

private func readUInt32LE(_ buf: [UInt8], at offset: Int) -> UInt32 {
    guard offset + 4 <= buf.count else { return 0 }
    return UInt32(buf[offset])
        | (UInt32(buf[offset + 1]) << 8)
        | (UInt32(buf[offset + 2]) << 16)
        | (UInt32(buf[offset + 3]) << 24)
}
private func readInt32LE(_ buf: [UInt8], at offset: Int) -> Int32 {
    return Int32(bitPattern: readUInt32LE(buf, at: offset))
}

private func writeUInt32LE(_ value: UInt32, into buf: inout [UInt8], at offset: Int) {
    guard offset + 4 <= buf.count else { return }
    buf[offset]     = UInt8(value & 0xFF)
    buf[offset + 1] = UInt8((value >> 8) & 0xFF)
    buf[offset + 2] = UInt8((value >> 16) & 0xFF)
    buf[offset + 3] = UInt8((value >> 24) & 0xFF)
}

private func readString(_ buf: [UInt8], offset: Int, maxLen: Int, declaredLen: Int) -> String {
    guard offset < buf.count else { return "" }
    let available = min(maxLen, buf.count - offset)
    let limit = declaredLen > 0 ? min(declaredLen, available) : available
    var bytes: [UInt8] = []
    bytes.reserveCapacity(limit)
    for i in 0..<limit {
        let b = buf[offset + i]
        if b == 0 { break }
        bytes.append(b)
    }
    return String(bytes: bytes, encoding: .utf8)
        ?? String(bytes: bytes, encoding: .isoLatin1)
        ?? ""
}

private func formatMac(_ buf: [UInt8], offset: Int) -> String {
    guard offset + 6 <= buf.count else { return "00:00:00:00:00:00" }
    return (0..<6).map { String(format: "%02X", buf[offset + $0]) }.joined(separator: ":")
}

struct RtlWifiStatus {
    let state: UInt32
    let channel: UInt32
    let rssiDbm: Int32
    let linkSpeedMbps: UInt32
    let txPackets: UInt32
    let txErrors: UInt32
    let rxPackets: UInt32
    let rxErrors: UInt32
    let macAddr: String
    let bssid: String
    let ssid: String
    let ht40Active: Bool
    let linkQualityPct: UInt8
    let digGain: UInt8
}

struct RtlWifiScanEntry {
    let bssid: String
    let channel: UInt8
    let security: UInt32
    let rssiDbm: Int8
    let ssid: String
    let htCapable: Bool
    let ht40SecOffset: Int8
}

struct RtlWifiDiagnostics {
    let txRingInflightBe: UInt32
    let txRingInflightVo: UInt32
    let rxRingInflight: UInt32
    let watchdogResets: UInt32
    let gtkRekeyCount: UInt32
    let usbStallRecoveries: UInt32
    let thermalDelta: Int32
    let amsduSubframes: UInt32
    let reorderFlushes: UInt32
    let pmkCacheHits: UInt32
    let roamCount: UInt32
}

final class RtlWifiClient {
    private let lock = NSLock()
    private var connectPort: io_connect_t = IO_OBJECT_NULL

    deinit {
        lock.lock()
        closeLocked()
        lock.unlock()
    }

    private func openLocked() -> io_connect_t {
        if connectPort != IO_OBJECT_NULL {
            return connectPort
        }
        guard let matching = IOServiceMatching("RtlWifiDriver") else {
            return IO_OBJECT_NULL
        }
        let service = IOServiceGetMatchingService(kIOMasterPortDefault, matching)
        guard service != IO_OBJECT_NULL else {
            return IO_OBJECT_NULL
        }
        defer { IOObjectRelease(service) }

        var port: io_connect_t = IO_OBJECT_NULL
        let kr = IOServiceOpen(service, mach_task_self_, 0, &port)
        if kr == KERN_SUCCESS && port != IO_OBJECT_NULL {
            connectPort = port
            return port
        }
        return IO_OBJECT_NULL
    }

    private func closeLocked() {
        if connectPort != IO_OBJECT_NULL {
            IOServiceClose(connectPort)
            connectPort = IO_OBJECT_NULL
        }
    }

    private func invokeMethod(
        _ port: io_connect_t,
        selector: UInt32,
        input: [UInt8]?,
        output: inout [UInt8],
        outputSize: inout Int
    ) -> kern_return_t {
        if let inp = input {
            if outputSize > 0 {
                return inp.withUnsafeBytes { inPtr in
                    output.withUnsafeMutableBytes { outPtr in
                        IOConnectCallStructMethod(
                            port, selector,
                            inPtr.baseAddress, inp.count,
                            outPtr.baseAddress, &outputSize
                        )
                    }
                }
            } else {
                return inp.withUnsafeBytes { inPtr in
                    IOConnectCallStructMethod(
                        port, selector,
                        inPtr.baseAddress, inp.count,
                        nil, nil
                    )
                }
            }
        } else {
            if outputSize > 0 {
                return output.withUnsafeMutableBytes { outPtr in
                    IOConnectCallStructMethod(
                        port, selector,
                        nil, 0,
                        outPtr.baseAddress, &outputSize
                    )
                }
            } else {
                return IOConnectCallStructMethod(
                    port, selector,
                    nil, 0,
                    nil, nil
                )
            }
        }
    }

    func callStructMethod(
        selector: UInt32,
        input: [UInt8]? = nil,
        outputSize: Int = 0
    ) -> (kern_return_t, [UInt8]) {
        lock.lock()
        defer { lock.unlock() }

        let port = openLocked()
        guard port != IO_OBJECT_NULL else {
            return (KERN_FAILURE, [])
        }

        var outBuf = [UInt8](repeating: 0, count: max(outputSize, 0))
        var outLen = outputSize
        var kr = invokeMethod(port, selector: selector, input: input, output: &outBuf, outputSize: &outLen)

        if kr != KERN_SUCCESS {
            closeLocked()
            let reopened = openLocked()
            if reopened != IO_OBJECT_NULL {
                outBuf = [UInt8](repeating: 0, count: max(outputSize, 0))
                outLen = outputSize
                kr = invokeMethod(reopened, selector: selector, input: input, output: &outBuf, outputSize: &outLen)
                if kr != KERN_SUCCESS {
                    closeLocked()
                }
            }
        }

        return (kr, outBuf)
    }

    var isDriverPresent: Bool {
        lock.lock()
        defer { lock.unlock() }
        return openLocked() != IO_OBJECT_NULL
    }

    func getStatus() -> RtlWifiStatus? {
        let (kr, buf) = callStructMethod(selector: kRtlMethodGetStatus, outputSize: 88)
        guard kr == KERN_SUCCESS, buf.count >= 88 else { return nil }
        guard readUInt32LE(buf, at: 0) == kRtlAbiVersion else { return nil }

        let ssidLen = Int(buf[48])
        return RtlWifiStatus(
            state: readUInt32LE(buf, at: 4),
            channel: readUInt32LE(buf, at: 8),
            rssiDbm: readInt32LE(buf, at: 12),
            linkSpeedMbps: readUInt32LE(buf, at: 16),
            txPackets: readUInt32LE(buf, at: 20),
            txErrors: readUInt32LE(buf, at: 24),
            rxPackets: readUInt32LE(buf, at: 28),
            rxErrors: readUInt32LE(buf, at: 32),
            macAddr: formatMac(buf, offset: 36),
            bssid: formatMac(buf, offset: 42),
            ssid: readString(buf, offset: 52, maxLen: kRtlMaxSsidLen, declaredLen: ssidLen),
            ht40Active: buf[49] != 0,
            linkQualityPct: buf[50],
            digGain: buf[51]
        )
    }

    func startScan() -> kern_return_t {
        return callStructMethod(selector: kRtlMethodStartScan).0
    }

    func getScanResults() -> [RtlWifiScanEntry] {
        let totalSize = 8 + kRtlMaxScanCount * 48
        let (kr, buf) = callStructMethod(selector: kRtlMethodGetScanResults, outputSize: totalSize)
        guard kr == KERN_SUCCESS, buf.count >= 8 else { return [] }
        guard readUInt32LE(buf, at: 0) == kRtlAbiVersion else { return [] }

        let count = min(Int(readUInt32LE(buf, at: 4)), kRtlMaxScanCount)
        var bySsid: [String: RtlWifiScanEntry] = [:]

        for i in 0..<count {
            let base = 8 + i * 48
            guard base + 48 <= buf.count else { break }
            let ssidLen = Int(buf[base + 9])
            let ssid = readString(buf, offset: base + 12, maxLen: kRtlMaxSsidLen, declaredLen: ssidLen)
            guard !ssid.isEmpty else { continue }

            let entry = RtlWifiScanEntry(
                bssid: formatMac(buf, offset: base),
                channel: buf[base + 6],
                security: UInt32(buf[base + 7]),
                rssiDbm: Int8(bitPattern: buf[base + 8]),
                ssid: ssid,
                htCapable: buf[base + 10] != 0,
                ht40SecOffset: Int8(bitPattern: buf[base + 11])
            )

            if let existing = bySsid[ssid] {
                if entry.rssiDbm > existing.rssiDbm {
                    bySsid[ssid] = entry
                }
            } else {
                bySsid[ssid] = entry
            }
        }

        return bySsid.values.sorted { $0.rssiDbm > $1.rssiDbm }
    }

    func connect(ssid: String, passphrase: String?, security: UInt32, preferHt40: Bool = true) -> kern_return_t {
        var req = [UInt8](repeating: 0, count: 112)
        let ssidBytes = Array(ssid.utf8.prefix(kRtlMaxSsidLen))
        let passBytes = Array((passphrase ?? "").utf8.prefix(kRtlMaxPassLen))

        writeUInt32LE(kRtlAbiVersion, into: &req, at: 0)
        writeUInt32LE(security, into: &req, at: 4)
        req[8] = UInt8(ssidBytes.count)
        req[9] = UInt8(passBytes.count)
        req[10] = preferHt40 ? 1 : 0
        req[11] = 0

        for (idx, b) in ssidBytes.enumerated() {
            req[12 + idx] = b
        }
        for (idx, b) in passBytes.enumerated() {
            req[45 + idx] = b
        }

        return callStructMethod(selector: kRtlMethodConnect, input: req).0
    }

    func disconnect() -> kern_return_t {
        return callStructMethod(selector: kRtlMethodDisconnect).0
    }

    func getDiagnostics() -> RtlWifiDiagnostics? {
        let (kr, buf) = callStructMethod(selector: kRtlMethodGetDiagnostics, outputSize: 48)
        guard kr == KERN_SUCCESS, buf.count >= 48 else { return nil }
        guard readUInt32LE(buf, at: 0) == kRtlAbiVersion else { return nil }

        return RtlWifiDiagnostics(
            txRingInflightBe: readUInt32LE(buf, at: 4),
            txRingInflightVo: readUInt32LE(buf, at: 8),
            rxRingInflight: readUInt32LE(buf, at: 12),
            watchdogResets: readUInt32LE(buf, at: 16),
            gtkRekeyCount: readUInt32LE(buf, at: 20),
            usbStallRecoveries: readUInt32LE(buf, at: 24),
            thermalDelta: readInt32LE(buf, at: 28),
            amsduSubframes: readUInt32LE(buf, at: 32),
            reorderFlushes: readUInt32LE(buf, at: 36),
            pmkCacheHits: readUInt32LE(buf, at: 40),
            roamCount: readUInt32LE(buf, at: 44)
        )
    }
}

enum KeychainHelper {
    static let service = "io.github.azizmaliz3.RtlWifiMenuBar"

    static func save(ssid: String, password: String) {
        guard let data = password.data(using: .utf8) else { return }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid
        ]
        let attrs: [String: Any] = [
            kSecValueData as String: data
        ]
        let status = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if status == errSecItemNotFound {
            var addQuery = query
            addQuery[kSecValueData as String] = data
            SecItemAdd(addQuery as CFDictionary, nil)
        }
    }

    static func load(ssid: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess, let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func delete(ssid: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid
        ]
        SecItemDelete(query as CFDictionary)
    }

    static func savedSsids() -> [String] {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecReturnAttributes as String: true,
            kSecMatchLimit as String: kSecMatchLimitAll
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess, let list = item as? [[String: Any]] else { return [] }
        return list.compactMap { $0[kSecAttrAccount as String] as? String }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private let menu = NSMenu()
    private let client = RtlWifiClient()

    private var refreshTimer: Timer?
    private var currentStatus: RtlWifiStatus?
    private var currentDiagnostics: RtlWifiDiagnostics?
    private var scanResults: [RtlWifiScanEntry] = []

    private var isScanning = false
    private var isConnecting = false
    private var connectingSsid: String?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        menu.delegate = self
        statusItem.menu = menu

        setStatusIcon(symbolName: "wifi.slash", fallbackText: "Wi-Fi ✕")
        rebuildMenu()

        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(handleSystemWake(_:)),
            name: NSWorkspace.didWakeNotification,
            object: nil
        )

        refreshTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            self?.refreshStatusAsync()
        }

        refreshStatusAsync()
        autoConnectIfNeeded(delay: 0.5)
    }

    func applicationWillTerminate(_ notification: Notification) {
        refreshTimer?.invalidate()
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }

    @objc private func handleSystemWake(_ notification: Notification) {
        autoConnectIfNeeded(delay: 2.0)
    }

    func menuWillOpen(_ menu: NSMenu) {
        refreshStatusAsync()
        rebuildMenu()
    }

    private func setStatusIcon(symbolName: String, fallbackText: String) {
        guard let button = statusItem.button else { return }
        if let image = NSImage(systemSymbolName: symbolName, accessibilityDescription: "RTL8188FU Wi-Fi") {
            image.isTemplate = true
            button.image = image
            button.title = ""
        } else {
            button.image = nil
            button.title = fallbackText
        }
    }

    private func updateStatusIcon() {
        if isConnecting || isScanning {
            setStatusIcon(symbolName: "wifi.exclamationmark", fallbackText: "Wi-Fi …")
            return
        }
        guard let st = currentStatus else {
            setStatusIcon(symbolName: "wifi.slash", fallbackText: "Wi-Fi ✕")
            return
        }
        switch st.state {
        case kRtlStateConnected:
            setStatusIcon(symbolName: "wifi", fallbackText: "Wi-Fi")
        case kRtlStateScanning, kRtlStateConnecting, kRtlStateRecovering:
            setStatusIcon(symbolName: "wifi.exclamationmark", fallbackText: "Wi-Fi !")
        default:
            setStatusIcon(symbolName: "wifi.slash", fallbackText: "Wi-Fi ✕")
        }
    }

    private func refreshStatusAsync() {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let st = self.client.getStatus()
            let diag = (st != nil) ? self.client.getDiagnostics() : nil
            DispatchQueue.main.async {
                self.currentStatus = st
                self.currentDiagnostics = diag
                self.updateStatusIcon()
                self.rebuildMenu()
            }
        }
    }

    @objc private func refreshClicked(_ sender: Any?) {
        refreshStatusAsync()
        startScan(autoConnectAfter: false)
    }

    func startScan(autoConnectAfter: Bool = false) {
        guard !isScanning else { return }
        isScanning = true
        updateStatusIcon()
        rebuildMenu()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let kr = self.client.startScan()
            if kr == KERN_SUCCESS {
                Thread.sleep(forTimeInterval: 1.5)
            }
            let results = self.client.getScanResults()
            let st = self.client.getStatus()
            DispatchQueue.main.async {
                self.isScanning = false
                self.scanResults = results
                self.currentStatus = st
                self.updateStatusIcon()
                self.rebuildMenu()
                if autoConnectAfter {
                    self.tryAutoConnectFromScanResults()
                }
            }
        }
    }

    private func autoConnectIfNeeded(delay: TimeInterval) {
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self = self else { return }
            let st = self.client.getStatus()
            DispatchQueue.main.async {
                self.currentStatus = st
                self.updateStatusIcon()
                if let st = st, st.state == kRtlStateDisconnected, !self.isConnecting {
                    let saved = Set(KeychainHelper.savedSsids())
                    if !saved.isEmpty {
                        self.startScan(autoConnectAfter: true)
                    }
                }
            }
        }
    }

    private func tryAutoConnectFromScanResults() {
        guard !isConnecting else { return }
        if let st = currentStatus, st.state == kRtlStateConnected { return }
        let saved = Set(KeychainHelper.savedSsids())
        guard !saved.isEmpty else { return }

        if let best = scanResults.first(where: { saved.contains($0.ssid) }) {
            let pass = KeychainHelper.load(ssid: best.ssid)
            connectToNetwork(
                ssid: best.ssid,
                passphrase: pass,
                security: best.security,
                saveToKeychain: false,
                showFailureAlert: false
            )
        }
    }

    private func connectToNetwork(
        ssid: String,
        passphrase: String?,
        security: UInt32,
        saveToKeychain: Bool,
        showFailureAlert: Bool = true
    ) {
        guard !isConnecting else { return }
        isConnecting = true
        connectingSsid = ssid
        updateStatusIcon()
        rebuildMenu()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let kr = self.client.connect(ssid: ssid, passphrase: passphrase, security: security, preferHt40: true)
            let st = self.client.getStatus()
            DispatchQueue.main.async {
                self.isConnecting = false
                self.connectingSsid = nil
                self.currentStatus = st
                self.updateStatusIcon()
                self.rebuildMenu()

                if kr != KERN_SUCCESS {
                    if showFailureAlert {
                        self.showAlert(
                            title: "Connection Failed",
                            message: "Could not connect to \"\(ssid)\" (kern_return_t: \(kr))."
                        )
                    }
                } else if saveToKeychain {
                    KeychainHelper.save(ssid: ssid, password: passphrase ?? "")
                }
            }
        }
    }

    @objc private func networkItemClicked(_ sender: NSMenuItem) {
        guard let entry = sender.representedObject as? RtlWifiScanEntry else { return }
        if entry.security == kRtlSecOpen {
            connectToNetwork(ssid: entry.ssid, passphrase: nil, security: kRtlSecOpen, saveToKeychain: true)
            return
        }
        if let savedPass = KeychainHelper.load(ssid: entry.ssid), !savedPass.isEmpty {
            connectToNetwork(ssid: entry.ssid, passphrase: savedPass, security: entry.security, saveToKeychain: false)
            return
        }
        promptForPassword(entry: entry)
    }

    private func promptForPassword(entry: RtlWifiScanEntry) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Join \"\(entry.ssid)\""
        alert.informativeText = "Enter the WPA2 password for this network."
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")

        let container = NSView(frame: NSRect(x: 0, y: 0, width: 280, height: 54))
        let passField = NSSecureTextField(frame: NSRect(x: 0, y: 28, width: 280, height: 24))
        passField.placeholderString = "Password"
        let rememberCheck = NSButton(checkboxWithTitle: "Remember this network", target: nil, action: nil)
        rememberCheck.frame = NSRect(x: 0, y: 0, width: 280, height: 20)
        rememberCheck.state = .on

        container.addSubview(passField)
        container.addSubview(rememberCheck)
        alert.accessoryView = container
        alert.window.initialFirstResponder = passField

        if alert.runModal() == .alertFirstButtonReturn {
            let pass = passField.stringValue
            let save = (rememberCheck.state == .on)
            connectToNetwork(ssid: entry.ssid, passphrase: pass, security: entry.security, saveToKeychain: save)
        }
    }

    @objc private func disconnectClicked(_ sender: Any?) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            _ = self.client.disconnect()
            let st = self.client.getStatus()
            DispatchQueue.main.async {
                self.currentStatus = st
                self.updateStatusIcon()
                self.rebuildMenu()
            }
        }
    }

    @objc private func forgetNetworkClicked(_ sender: NSMenuItem) {
        guard let ssid = sender.representedObject as? String else { return }
        KeychainHelper.delete(ssid: ssid)
        if let st = currentStatus, st.state == kRtlStateConnected, st.ssid == ssid {
            disconnectClicked(nil)
        } else {
            rebuildMenu()
        }
    }

    @objc private func checkCaptivePortalClicked(_ sender: Any?) {
        guard let url = URL(string: "http://captive.apple.com/hotspot-detect.html") else { return }
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 6.0
        config.timeoutIntervalForResource = 6.0
        let session = URLSession(configuration: config)

        session.dataTask(with: url) { [weak self] data, _, error in
            DispatchQueue.main.async {
                guard let self = self else { return }
                if let error = error {
                    self.showAlert(
                        title: "Captive Portal Check",
                        message: "Network check failed: \(error.localizedDescription)"
                    )
                    return
                }
                let body = data.flatMap { String(data: $0, encoding: .utf8) } ?? ""
                if body.contains("Success") {
                    self.showAlert(
                        title: "Internet Connected",
                        message: "Captive portal check succeeded (no captive portal detected)."
                    )
                } else {
                    NSApp.activate(ignoringOtherApps: true)
                    let alert = NSAlert()
                    alert.messageText = "Captive Portal Detected"
                    alert.informativeText = "This network may require signing in via a web portal."
                    alert.addButton(withTitle: "Open Login Page")
                    alert.addButton(withTitle: "Close")
                    if alert.runModal() == .alertFirstButtonReturn {
                        NSWorkspace.shared.open(url)
                    }
                }
            }
        }.resume()
    }

    @objc private func openNetworkSettingsClicked(_ sender: Any?) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Network-Settings.extension"),
           NSWorkspace.shared.open(url) {
            return
        }
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Library/PreferencePanes/Network.prefPane"))
    }

    @objc private func quitClicked(_ sender: Any?) {
        NSApp.terminate(nil)
    }

    private func showAlert(title: String, message: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    private func signalBars(_ rssi: Int32) -> String {
        if rssi >= -55 { return "▂▄▆█" }
        if rssi >= -67 { return "▂▄▆" }
        if rssi >= -78 { return "▂▄" }
        return "▂"
    }

    private func rebuildMenu() {
        menu.removeAllItems()

        if isConnecting {
            let label = connectingSsid.map { "Connecting to \($0)..." } ?? "Connecting..."
            let item = NSMenuItem(title: label, action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
        } else if isScanning {
            let item = NSMenuItem(title: "Scanning...", action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
        } else if let st = currentStatus {
            switch st.state {
            case kRtlStateConnected:
                let title = "Connected: \(st.ssid.isEmpty ? st.bssid : st.ssid)"
                let connItem = NSMenuItem(title: title, action: nil, keyEquivalent: "")
                connItem.isEnabled = false
                menu.addItem(connItem)

                let info = String(
                    format: "Ch %u • %d dBm • %u Mbps%@",
                    st.channel, st.rssiDbm, st.linkSpeedMbps,
                    st.ht40Active ? " (HT40)" : ""
                )
                let infoItem = NSMenuItem(title: info, action: nil, keyEquivalent: "")
                infoItem.isEnabled = false
                menu.addItem(infoItem)

                if let diag = currentDiagnostics {
                    let diagStr = "TX \(st.txPackets) (err \(st.txErrors)) • RX \(st.rxPackets) • StallRec \(diag.usbStallRecoveries)"
                    let diagItem = NSMenuItem(title: diagStr, action: nil, keyEquivalent: "")
                    diagItem.isEnabled = false
                    menu.addItem(diagItem)
                }

                let discItem = NSMenuItem(title: "Disconnect", action: #selector(disconnectClicked(_:)), keyEquivalent: "")
                discItem.target = self
                menu.addItem(discItem)

            case kRtlStateConnecting:
                let item = NSMenuItem(title: "Connecting...", action: nil, keyEquivalent: "")
                item.isEnabled = false
                menu.addItem(item)

            case kRtlStateScanning:
                let item = NSMenuItem(title: "Scanning...", action: nil, keyEquivalent: "")
                item.isEnabled = false
                menu.addItem(item)

            case kRtlStateRecovering:
                let item = NSMenuItem(title: "Recovering adapter...", action: nil, keyEquivalent: "")
                item.isEnabled = false
                menu.addItem(item)

            default:
                let item = NSMenuItem(title: "Disconnected", action: nil, keyEquivalent: "")
                item.isEnabled = false
                menu.addItem(item)
            }
        } else {
            let item = NSMenuItem(title: "RTL8188FU Adapter Not Detected", action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
        }

        menu.addItem(NSMenuItem.separator())

        let header = NSMenuItem(title: isScanning ? "Scanning for Networks..." : "Available Networks", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)

        if scanResults.isEmpty {
            let emptyItem = NSMenuItem(title: isScanning ? "Please wait..." : "No networks found (click Refresh)", action: nil, keyEquivalent: "")
            emptyItem.isEnabled = false
            menu.addItem(emptyItem)
        } else {
            for entry in scanResults {
                let lockBadge = (entry.security == kRtlSecOpen) ? "Open" : "WPA2"
                let activeMark = (currentStatus?.state == kRtlStateConnected && currentStatus?.ssid == entry.ssid) ? "✓ " : ""
                let title = "\(activeMark)\(entry.ssid)  [\(lockBadge)]  \(signalBars(Int32(entry.rssiDbm))) \(entry.rssiDbm) dBm"
                let item = NSMenuItem(title: title, action: #selector(networkItemClicked(_:)), keyEquivalent: "")
                item.target = self
                item.representedObject = entry
                item.isEnabled = !isConnecting
                menu.addItem(item)
            }
        }

        menu.addItem(NSMenuItem.separator())

        let refreshItem = NSMenuItem(title: "Refresh", action: #selector(refreshClicked(_:)), keyEquivalent: "r")
        refreshItem.target = self
        menu.addItem(refreshItem)

        let savedSsids = KeychainHelper.savedSsids().sorted()
        if !savedSsids.isEmpty {
            let forgetRoot = NSMenuItem(title: "Forget Network", action: nil, keyEquivalent: "")
            let forgetMenu = NSMenu()
            for ssid in savedSsids {
                let fItem = NSMenuItem(title: ssid, action: #selector(forgetNetworkClicked(_:)), keyEquivalent: "")
                fItem.target = self
                fItem.representedObject = ssid
                forgetMenu.addItem(fItem)
            }
            forgetRoot.submenu = forgetMenu
            menu.addItem(forgetRoot)
        }

        let captiveItem = NSMenuItem(title: "Check Captive Portal", action: #selector(checkCaptivePortalClicked(_:)), keyEquivalent: "")
        captiveItem.target = self
        menu.addItem(captiveItem)

        let netPrefsItem = NSMenuItem(title: "Open Network Settings...", action: #selector(openNetworkSettingsClicked(_:)), keyEquivalent: "")
        netPrefsItem.target = self
        menu.addItem(netPrefsItem)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(title: "Quit", action: #selector(quitClicked(_:)), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
