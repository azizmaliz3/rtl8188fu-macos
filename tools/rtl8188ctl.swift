import Foundation
import IOKit

let args = CommandLine.arguments
guard args.count >= 2 else {
    print("Usage: rtl8188ctl <status|scan|disconnect>")
    exit(1)
}

guard let matching = IOServiceMatching("RtlWifiDriver") else { exit(1) }
let service = IOServiceGetMatchingService(kIOMasterPortDefault, matching)
if service == 0 {
    print("Error: RtlWifiDriver kext not loaded or RTL8188FU dongle not attached.")
    exit(1)
}
var conn: io_connect_t = 0
guard IOServiceOpen(service, mach_task_self_, 0, &conn) == KERN_SUCCESS else {
    print("Error: Could not open RtlWifiUserClient.")
    exit(1)
}
defer { IOServiceClose(conn); IOObjectRelease(service) }

switch args[1] {
case "status":
    var buf = [UInt8](repeating: 0, count: 88)
    var outSize = buf.count
    let kr = IOConnectCallStructMethod(conn, 0, nil, 0, &buf, &outSize)
    if kr == KERN_SUCCESS {
        buf.withUnsafeBytes { p in
            let state = p.load(fromByteOffset: 4, as: UInt32.self)
            let ch    = p.load(fromByteOffset: 8, as: UInt32.self)
            let rssi  = p.load(fromByteOffset: 12, as: Int32.self)
            let speed = p.load(fromByteOffset: 16, as: UInt32.self)
            let tx    = p.load(fromByteOffset: 20, as: UInt32.self)
            let rx    = p.load(fromByteOffset: 28, as: UInt32.self)
            print("State: \(state) | Ch: \(ch) | RSSI: \(rssi) dBm | Link: \(speed) Mbps | TX: \(tx) | RX: \(rx)")
        }
    }
case "scan":
    _ = IOConnectCallStructMethod(conn, 1, nil, 0, nil, nil)
    print("Scan triggered.")
case "disconnect":
    _ = IOConnectCallStructMethod(conn, 4, nil, 0, nil, nil)
    print("Disconnected.")
default:
    print("Unknown command: \(args[1])")
    exit(1)
}
