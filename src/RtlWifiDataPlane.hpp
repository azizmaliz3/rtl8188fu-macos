#ifndef RTL_WIFI_DATA_PLANE_HPP
#define RTL_WIFI_DATA_PLANE_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace rtl8188f {

// RTL8188FU / RTL8723B-gen Registers (Linux rtl8xxxu_regs.h)
static constexpr uint16_t kRegTrxdmaCtrl     = 0x010C;
static constexpr uint16_t kRegHmetfr         = 0x01CF;
static constexpr uint16_t kRegHmebox0        = 0x01D0;
static constexpr uint16_t kRegHmeboxExt0     = 0x01F0;
static constexpr uint16_t kRegRxdmaAggPgTh   = 0x0280;
static constexpr uint16_t kRegRxdmaModeCtrl  = 0x0290;
static constexpr uint16_t kRegRcr            = 0x0608;
static constexpr uint16_t kRegBssid          = 0x0618;
static constexpr uint16_t kRegOfdm0XaAgc1    = 0x0C50;

static constexpr uint32_t kRcrCbssidData     = (1u << 6);
static constexpr uint32_t kRcrCbssidBcn      = (1u << 7);

static constexpr uint8_t  kH2cMediaStatusRpt = 0x01;
static constexpr uint8_t  kH2cKeepAlive      = 0x03;
static constexpr uint8_t  kH2cMacidCfg       = 0x40;

// Async USB Ring Sizing
static constexpr uint32_t kTxBeRingSize      = 32;
static constexpr uint32_t kTxHiRingSize      = 16;
static constexpr uint32_t kTxBufSize         = 2048;
static constexpr uint32_t kRxRingSize        = 16;
static constexpr uint32_t kRxBufSize         = 16384;

// TX Descriptor 40-byte layout constants (rtl8xxxu_txdesc40)
static constexpr uint32_t kTxDescSize        = 40;
static constexpr uint32_t kTxDw1SecCcmp      = 0x00C00000u;
static constexpr uint32_t kTxDw2AggEnable    = (1u << 12);
static constexpr uint32_t kTxDw2AggBreak     = (1u << 16);
static constexpr uint32_t kTxDw2AmpduDensity = (5u << 20);
static constexpr uint32_t kTxDw3UseRate      = (1u << 8);
static constexpr uint32_t kTxDw4RetryLimitEn = (1u << 17);
static constexpr uint32_t kTxDw8HwSeqEnable  = (1u << 15);

static constexpr uint8_t  kQselBe            = 0x00;
static constexpr uint8_t  kQselBk            = 0x01;
static constexpr uint8_t  kQselVi            = 0x04;
static constexpr uint8_t  kQselVo            = 0x06;
static constexpr uint8_t  kQselMgmt          = 0x12;

// Standard WMM Information Element (7 bytes payload) & 802.11n HT Capabilities IE (26 bytes)
static constexpr uint8_t kWmmInfoIe[9] = {
    0xDD, 0x07, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00
};

static inline void putLe16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

static inline void putLe32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

static inline uint16_t getLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static inline uint32_t getLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Control-Plane Fast Lane + WMM QoS Classifier
// Inspects up to first 42 bytes of Ethernet II frame (zero full-packet copy!).
struct TxClassResult {
    uint8_t tid;
    bool    highPriorityPipe; // true => EP 0x02 (fPipeOut2)
    bool    isControlFastLane; // ARP / DHCP / DNS => AGG_BREAK for instant delivery
};

static inline TxClassResult classifyEthHeader(const uint8_t *hdr, size_t hdrLen) {
    TxClassResult r{0, false, false};
    if (!hdr || hdrLen < 14) return r;
    const uint16_t ethType = static_cast<uint16_t>((hdr[12] << 8) | hdr[13]);

    // ARP (0x0806): always high-priority fast lane
    if (ethType == 0x0806) {
        r.tid = 6; r.highPriorityPipe = true; r.isControlFastLane = true;
        return r;
    }
    if (ethType == 0x0800 && hdrLen >= 24) {
        const uint8_t ihl = static_cast<uint8_t>((hdr[14] & 0x0Fu) * 4u);
        const uint8_t proto = hdr[23];
        // UDP DHCP (67/68) or DNS (53) -> fast lane
        if (proto == 17 && ihl >= 20 && hdrLen >= static_cast<size_t>(14 + ihl + 4)) {
            uint16_t sport = static_cast<uint16_t>((hdr[14 + ihl] << 8) | hdr[14 + ihl + 1]);
            uint16_t dport = static_cast<uint16_t>((hdr[14 + ihl + 2] << 8) | hdr[14 + ihl + 3]);
            if (sport == 67 || sport == 68 || dport == 67 || dport == 68 || sport == 53 || dport == 53) {
                r.tid = 6; r.highPriorityPipe = true; r.isControlFastLane = true;
                return r;
            }
        }
        const uint8_t dscp = static_cast<uint8_t>(hdr[15] >> 2);
        const uint8_t cs = static_cast<uint8_t>(dscp >> 3);
        if (cs >= 6 || dscp == 46) { r.tid = 6; r.highPriorityPipe = true; }
        else if (cs == 4 || cs == 5) { r.tid = 4; r.highPriorityPipe = true; }
        else if (cs == 1) { r.tid = 1; }
    } else if (ethType == 0x86DD && hdrLen >= 16) {
        uint8_t tc = static_cast<uint8_t>(((hdr[14] & 0x0Fu) << 4) | (hdr[15] >> 4));
        uint8_t cs = static_cast<uint8_t>(tc >> 5);
        if (cs >= 6) { r.tid = 6; r.highPriorityPipe = true; }
        else if (cs == 4 || cs == 5) { r.tid = 4; r.highPriorityPipe = true; }
    }
    return r;
}

static inline uint8_t tidToQsel(uint8_t tid) {
    if (tid >= 6) return kQselVo;
    if (tid >= 4) return kQselVi;
    if (tid == 1 || tid == 2) return kQselBk;
    return kQselBe;
}

static inline uint16_t computeTxDescChecksum(const uint8_t *desc) {
    uint16_t sum = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 14) continue;
        sum ^= getLe16(desc + i * 2);
    }
    return sum;
}

// Guards USB 2.0 512-byte wMaxPacketSize boundary to prevent Realtek TX DMA stall
static inline uint32_t safeUsbBulkOutLen(uint32_t totalLen, uint8_t *buf, size_t bufCap) {
    if ((totalLen & 511u) == 0 && totalLen + 8u <= bufCap) {
        memset(buf + totalLen, 0, 8);
        return totalLen + 8u;
    }
    return totalLen;
}

struct TxDescParams {
    uint16_t frameLen;
    uint32_t secType;       // kTxDw1SecCcmp or 0 (Open network)
    uint8_t  tid;
    bool     isMulticast;
    bool     isQosData;
    bool     allowAmpdu;
    bool     useDriverRate;
    uint8_t  driverRate;
    uint8_t  macId;
};

static inline void buildTxDesc40(uint8_t *d, const TxDescParams &p) {
    memset(d, 0, kTxDescSize);
    putLe16(d + 0, p.frameLen);
    d[2] = static_cast<uint8_t>(kTxDescSize);
    d[3] = static_cast<uint8_t>(0x80u | 0x08u | 0x04u | (p.isMulticast ? 0x01u : 0x00u));

    const uint8_t qsel = p.isQosData ? tidToQsel(p.tid) : kQselMgmt;
    uint32_t dw1 = (static_cast<uint32_t>(p.macId) & 0x7Fu) |
                   (static_cast<uint32_t>(qsel) << 8) |
                   p.secType;
    putLe32(d + 4, dw1);

    uint32_t dw2 = 0;
    if (p.allowAmpdu && p.isQosData && !p.isMulticast) {
        dw2 |= kTxDw2AggEnable | kTxDw2AmpduDensity;
    } else {
        dw2 |= kTxDw2AggBreak;
    }
    putLe32(d + 8, dw2);

    uint32_t dw3 = p.useDriverRate ? kTxDw3UseRate : 0u;
    uint32_t dw4 = p.useDriverRate
        ? (static_cast<uint32_t>(p.driverRate) | (12u << 18) | kTxDw4RetryLimitEn)
        : 0u;
    putLe32(d + 12, dw3);
    putLe32(d + 16, dw4);
    putLe32(d + 32, kTxDw8HwSeqEnable);
    putLe16(d + 28, computeTxDescChecksum(d));
}

// Zero-copy QoS 802.11 header writer (supports BOTH WPA2 CCMP and Open unencrypted!)
// Returns header length written (42 for CCMP, 34 for Open), so caller copies payload ONCE directly after it.
static inline uint16_t writeQosDataHeader(uint8_t *out,
                                          const uint8_t ethHdr14[14],
                                          const uint8_t apBssid[6],
                                          const uint8_t staMac[6],
                                          bool encrypted,
                                          uint64_t txPn,
                                          uint8_t tid) {
    const uint16_t hlen = encrypted ? 42u : 34u;
    memset(out, 0, hlen);
    out[0] = 0x88;                             // QoS Data
    out[1] = encrypted ? 0x41u : 0x01u;        // To-DS (+ Protected if WPA2)
    memcpy(out + 4,  apBssid,      6);
    memcpy(out + 10, staMac,       6);
    memcpy(out + 16, ethHdr14 + 0, 6);
    out[24] = static_cast<uint8_t>(tid & 0x0Fu);
    out[25] = 0x00;

    uint16_t off = 26;
    if (encrypted) {
        out[26] = static_cast<uint8_t>(txPn & 0xFFu);
        out[27] = static_cast<uint8_t>((txPn >> 8) & 0xFFu);
        out[28] = 0x00;
        out[29] = 0x20; // ExtIV | KeyID 0
        out[30] = static_cast<uint8_t>((txPn >> 16) & 0xFFu);
        out[31] = static_cast<uint8_t>((txPn >> 24) & 0xFFu);
        out[32] = static_cast<uint8_t>((txPn >> 32) & 0xFFu);
        out[33] = static_cast<uint8_t>((txPn >> 40) & 0xFFu);
        off = 34;
    }
    out[off + 0] = 0xAA; out[off + 1] = 0xAA; out[off + 2] = 0x03;
    out[off + 3] = 0x00; out[off + 4] = 0x00; out[off + 5] = 0x00;
    out[off + 6] = ethHdr14[12];
    out[off + 7] = ethHdr14[13];
    return hlen;
}

// Ring-0 safe A-MSDU subframe splitter (handles Wi-Fi 5/6/7 routers sending A-MSDU bursts)
struct AmsduSubframe {
    const uint8_t *da;
    const uint8_t *sa;
    uint16_t       etherType;
    const uint8_t *payload;
    uint16_t       payloadLen;
};

static inline uint32_t splitAmsdu(const uint8_t *data, size_t len, AmsduSubframe *out, uint32_t maxOut) {
    uint32_t count = 0;
    size_t pos = 0;
    while (pos + 14 + 8 <= len && count < maxOut) {
        uint16_t msduLen = static_cast<uint16_t>((data[pos + 12] << 8) | data[pos + 13]);
        if (msduLen < 8 || msduLen > 1508 || pos + 14 + msduLen > len) break;
        const uint8_t *llc = data + pos + 14;
        if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03) {
            out[count].da         = data + pos;
            out[count].sa         = data + pos + 6;
            out[count].etherType  = static_cast<uint16_t>((llc[6] << 8) | llc[7]);
            out[count].payload    = llc + 8;
            out[count].payloadLen = static_cast<uint16_t>(msduLen - 8);
            count++;
        }
        size_t adv = 14u + msduLen;
        adv = (adv + 3u) & ~static_cast<size_t>(3u); // 4-byte A-MSDU padding
        if (adv == 0 || pos + adv > len) break;
        pos += adv;
    }
    return count;
}

// Decodes real hardware RSSI (dBm) from RTL8188FU 32-byte drvinfo PHY status block
static inline int32_t extractPhyRssiDbm(const uint8_t *drvInfo, uint32_t drvInfoLen, uint32_t rxRate) {
    if (!drvInfo || drvInfoLen < 16) return -65;
    if (rxRate < 4) {
        // CCK rate: AGC report in byte 1 (rtl8xxxu_rx_parse_phystats)
        uint8_t agc = drvInfo[1];
        uint8_t lna = static_cast<uint8_t>((agc >> 5) & 0x07u);
        uint8_t vga = static_cast<uint8_t>(agc & 0x1Fu);
        int32_t rssi = -36 - static_cast<int32_t>(lna * 8) - static_cast<int32_t>(vga * 2);
        if (rssi < -100) rssi = -100;
        if (rssi > -20)  rssi = -20;
        return rssi;
    } else {
        // OFDM / HT rate: path A gain_trsw in byte 0
        int32_t pwdb = static_cast<int32_t>(drvInfo[0] & 0x7Fu) - 110;
        if (pwdb < -100) pwdb = -100;
        if (pwdb > -20)  pwdb = -20;
        return pwdb;
    }
}

// Parses HT Operation IE (Tag 61) for secondary channel offset: +1 (HT40+), -1 (HT40-), 0 (HT20)
static inline int8_t parseHtOpSecondaryOffset(const uint8_t *ies, uint16_t ieLen) {
    uint16_t p = 0;
    while (p + 2 <= ieLen) {
        uint8_t tag = ies[p], len = ies[p + 1];
        if (p + 2 + len > ieLen) break;
        if (tag == 61 && len >= 2) {
            uint8_t info1 = ies[p + 3]; // byte 0=primary ch, byte 1=HT info subset 1
            uint8_t sec = info1 & 0x03u;
            bool staChWidth40 = (info1 & 0x04u) != 0;
            if (staChWidth40 && sec == 1) return +1;
            if (staChWidth40 && sec == 3) return -1;
            return 0;
        }
        p = static_cast<uint16_t>(p + 2 + len);
    }
    return 0;
}

// Builds 802.11n Block Ack ADDBA Request (Category=3, Action=0)
static inline uint16_t buildAddbaRequest(uint8_t *out, size_t outCap,
                                         const uint8_t apBssid[6], const uint8_t staMac[6],
                                         uint8_t dialogToken, uint8_t tid, uint16_t bufSize = 64) {
    if (!out || outCap < 33) return 0;
    memset(out, 0, 33);
    out[0] = 0xD0;
    memcpy(out + 4,  apBssid, 6);
    memcpy(out + 10, staMac,  6);
    memcpy(out + 16, apBssid, 6);
    out[24] = 0x03; out[25] = 0x00; out[26] = dialogToken;
    uint16_t baParam = static_cast<uint16_t>(0x0002u | ((tid & 0x0Fu) << 2) | ((bufSize & 0x03FFu) << 6));
    putLe16(out + 27, baParam);
    return 33;
}

struct H2cCommand { uint32_t cmd; uint16_t ext; };

static inline H2cCommand buildH2cMediaStatus(bool connected, uint8_t macId = 0) {
    H2cCommand h{};
    h.cmd = static_cast<uint32_t>(kH2cMediaStatusRpt) |
            (static_cast<uint32_t>(connected ? 1u : 0u) << 8) |
            (static_cast<uint32_t>(macId) << 16);
    return h;
}

static inline H2cCommand buildH2cRaMask(uint8_t macId, bool ht40, bool shortGi) {
    H2cCommand h{};
    const uint8_t raid = 3u;
    h.cmd = static_cast<uint32_t>(kH2cMacidCfg) |
            (static_cast<uint32_t>(macId & 0x7Fu) << 8) |
            (static_cast<uint32_t>((raid & 0x1Fu) | (shortGi ? 0x80u : 0x00u)) << 16) |
            (static_cast<uint32_t>(ht40 ? 1u : 0u) << 24);
    h.ext = 0xFFFFu;
    return h;
}

static inline H2cCommand buildH2cKeepAlive(bool enable, uint8_t intervalSec = 10) {
    H2cCommand h{};
    h.cmd = static_cast<uint32_t>(kH2cKeepAlive) |
            (static_cast<uint32_t>(enable ? 1u : 0u) << 8) |
            (static_cast<uint32_t>(intervalSec) << 16);
    return h;
}

} // namespace rtl8188f

#endif /* RTL_WIFI_DATA_PLANE_HPP */
