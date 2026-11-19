#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/RtlWifiShared.h"
#include "../src/RtlWifiDataPlane.hpp"
#include "../src/RtlWifiStability.hpp"
#include "../src/RtlWifiCrypto.h"

int main() {
    // 1. ABI v3 packed struct sizes
    assert(sizeof(RtlWifiStatus) == 88);
    assert(sizeof(RtlWifiDiagnostics) == 48);
    assert(sizeof(RtlWifiScanEntry) == 48);
    assert(sizeof(RtlWifiConnectReq) == 112);

    // 2. Control-Plane Fast Lane (ARP/DHCP/DNS) + WMM DSCP routing
    uint8_t arp[42]{}; arp[12] = 0x08; arp[13] = 0x06;
    auto cArp = rtl8188f::classifyEthHeader(arp, sizeof(arp));
    assert(cArp.highPriorityPipe && cArp.isControlFastLane && cArp.tid == 6);

    uint8_t dns[42]{};
    dns[12] = 0x08; dns[13] = 0x00; dns[14] = 0x45; dns[23] = 17; // UDP
    dns[36] = 0x00; dns[37] = 53; // dport 53
    auto cDns = rtl8188f::classifyEthHeader(dns, sizeof(dns));
    assert(cDns.highPriorityPipe && cDns.isControlFastLane);

    // 3. USB 512-byte ZLP boundary guard
    uint8_t zbuf[600]{};
    assert(rtl8188f::safeUsbBulkOutLen(512, zbuf, sizeof(zbuf)) == 520);
    assert(rtl8188f::safeUsbBulkOutLen(516, zbuf, sizeof(zbuf)) == 516);

    // 4. Zero-copy QoS header builder (WPA2 CCMP vs Open Wi-Fi)
    uint8_t bssid[6] = {1,2,3,4,5,6}, sta[6] = {7,8,9,10,11,12}, out[64]{};
    assert(rtl8188f::writeQosDataHeader(out, arp, bssid, sta, true, 99, 6) == 42);
    assert(out[0] == 0x88 && out[1] == 0x41 && out[24] == 6 && out[29] == 0x20);
    assert(rtl8188f::writeQosDataHeader(out, arp, bssid, sta, false, 99, 0) == 34);
    assert(out[0] == 0x88 && out[1] == 0x01);

    // 5. A-MSDU splitter + Ring-0 Fuzz tests (truncated, zero-len, oversized)
    uint8_t amsdu[128]{};
    amsdu[12] = 0; amsdu[13] = 12; // msduLen = 12 (8 LLC + 4 payload)
    amsdu[14] = 0xAA; amsdu[15] = 0xAA; amsdu[16] = 0x03; amsdu[20] = 0x08; amsdu[21] = 0x00;
    rtl8188f::AmsduSubframe subs[4];
    assert(rtl8188f::splitAmsdu(amsdu, 28, subs, 4) == 1 && subs[0].payloadLen == 4 && subs[0].etherType == 0x0800);
    // Fuzz malformed/truncated buffers: must never crash or overrun!
    uint8_t junk[64]; memset(junk, 0xFF, sizeof(junk));
    assert(rtl8188f::splitAmsdu(junk, sizeof(junk), subs, 4) == 0);
    assert(rtl8188f::splitAmsdu(amsdu, 5, subs, 4) == 0);

    // 6. 64-slot RX A-MPDU Reordering Window (in-order, out-of-order, duplicate, gap flush)
    rtl8188f::RtlRxReorderWindow<int> rw;
    rw.reset(0);
    std::vector<int> delivered;
    auto emit = [&](int v) { delivered.push_back(v); };
    assert(rw.insert(10, 10, emit));  // init -> delivers 10
    assert(rw.insert(12, 12, emit));  // buffered (gap at 11)
    assert(!rw.insert(12, 12, emit)); // duplicate rejected
    assert(delivered.size() == 1);
    assert(rw.insert(11, 11, emit));  // fills gap -> delivers 11 AND 12 in order!
    assert(delivered.size() == 3 && delivered[1] == 11 && delivered[2] == 12);
    assert(rw.insert(14, 14, emit));  // gap at 13
    rw.flushAll(emit);                // timeout flush -> delivers 14
    assert(delivered.size() == 4 && delivered[3] == 14 && rw.flushCount() == 1);

    // 7. PMK Cache + Mesh Roam Trigger + PHY RSSI + HT40 Op IE
    rtl8188f::RtlPmkCache cache;
    cache.clear();
    uint8_t ph[20]{1}, pmkIn[32]{7}, pmkOut[32]{};
    cache.store("MeshNet", 7, ph, pmkIn);
    assert(cache.lookup("MeshNet", 7, ph, pmkOut) && rtlcrypto::ct_equal(pmkIn, pmkOut, 32) && cache.hits() == 1);
    assert(rtl8188f::shouldRoamToSibling(-82, -68));
    assert(!rtl8188f::shouldRoamToSibling(-65, -50));

    uint8_t drvInfo[32]{}; drvInfo[0] = 55;
    int32_t rssi = rtl8188f::extractPhyRssiDbm(drvInfo, 32, 12);
    assert(rssi == -55);

    uint8_t htOpIe[] = { 61, 22, 6, 0x05, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }; // HT40+
    assert(rtl8188f::parseHtOpSecondaryOffset(htOpIe, sizeof(htOpIe)) == +1);

    // 8. Hardened RtlWifiCrypto.h KATs
    uint8_t pmk[32];
    rtlcrypto::pbkdf2_pmk("password", reinterpret_cast<const uint8_t *>("IEEE"), 4, pmk);
    static const uint8_t kExpectedPmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
    };
    assert(rtlcrypto::ct_equal(pmk, kExpectedPmk, 32));
    rtlcrypto::secure_zero(pmk, sizeof(pmk));
    assert(pmk[0] == 0 && pmk[31] == 0);

    std::printf("ALL V28 ULTIMATE + RING-0 FUZZ HOSTTESTS PASSED!\n");
    return 0;
}
