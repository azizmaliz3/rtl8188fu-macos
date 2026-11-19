#ifndef RTL_WIFI_STABILITY_HPP
#define RTL_WIFI_STABILITY_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "RtlWifiCrypto.h"

namespace rtl8188f {

// 64-slot RX A-MPDU Sequence Number Reordering Window (prevents TCP Dup-ACK storms!)
template <typename T>
class RtlRxReorderWindow {
public:
    static constexpr uint16_t kWinSize = 64;
    static constexpr uint16_t kSeqMask = 0x0FFFu;

    void reset(uint16_t startSeq = 0) {
        fHeadSeq = startSeq & kSeqMask;
        fInitialized = false;
        fStoredCount = 0;
        fFlushCount = 0;
        for (uint16_t i = 0; i < kWinSize; ++i) { fOccupied[i] = false; fItems[i] = T{}; }
    }

    static inline uint16_t seqDiff(uint16_t a, uint16_t b) {
        return static_cast<uint16_t>((a - b) & kSeqMask);
    }

    // Inserts item at 12-bit 802.11 sequence number; calls emit(item) for all ready in-order items.
    // Returns false if item was a duplicate/stale frame (caller should free it).
    template <typename EmitFn>
    bool insert(uint16_t seq, T item, EmitFn emit) {
        seq &= kSeqMask;
        if (!fInitialized) {
            fInitialized = true;
            fHeadSeq = static_cast<uint16_t>((seq + 1u) & kSeqMask);
            emit(item);
            return true;
        }
        uint16_t diff = seqDiff(seq, fHeadSeq);
        // Stale / duplicate behind window
        if (diff >= 2048u) return false;

        // Ahead of window: advance head and flush older slots
        if (diff >= kWinSize) {
            uint16_t shift = static_cast<uint16_t>(diff - kWinSize + 1u);
            advanceAndFlush(shift, emit);
            diff = seqDiff(seq, fHeadSeq);
        }

        // Fast path: exact in-order arrival!
        if (diff == 0) {
            emit(item);
            fHeadSeq = static_cast<uint16_t>((fHeadSeq + 1u) & kSeqMask);
            drainContiguous(emit);
            return true;
        }

        uint16_t idx = seq & (kWinSize - 1u);
        if (fOccupied[idx]) return false; // duplicate
        fItems[idx] = item;
        fOccupied[idx] = true;
        fStoredCount++;
        return true;
    }

    template <typename EmitFn>
    void flushAll(EmitFn emit) {
        if (fStoredCount == 0) return;
        fFlushCount++;
        advanceAndFlush(kWinSize, emit);
    }

    uint32_t storedCount() const { return fStoredCount; }
    uint32_t flushCount()  const { return fFlushCount; }

private:
    template <typename EmitFn>
    void drainContiguous(EmitFn emit) {
        while (fStoredCount > 0) {
            uint16_t idx = fHeadSeq & (kWinSize - 1u);
            if (!fOccupied[idx]) break;
            T it = fItems[idx];
            fOccupied[idx] = false;
            fStoredCount--;
            fHeadSeq = static_cast<uint16_t>((fHeadSeq + 1u) & kSeqMask);
            emit(it);
        }
    }

    template <typename EmitFn>
    void advanceAndFlush(uint16_t steps, EmitFn emit) {
        if (steps > kWinSize) steps = kWinSize;
        for (uint16_t i = 0; i < steps; ++i) {
            uint16_t idx = fHeadSeq & (kWinSize - 1u);
            if (fOccupied[idx]) {
                T it = fItems[idx];
                fOccupied[idx] = false;
                fStoredCount--;
                emit(it);
            }
            fHeadSeq = static_cast<uint16_t>((fHeadSeq + 1u) & kSeqMask);
        }
        drainContiguous(emit);
    }

    T        fItems[kWinSize]{};
    bool     fOccupied[kWinSize]{};
    uint16_t fHeadSeq     = 0;
    bool     fInitialized = false;
    uint32_t fStoredCount = 0;
    uint32_t fFlushCount  = 0;
};

// 8-entry PMK Cache (avoids re-running 4096 PBKDF2 iterations on wake/reconnect)
class RtlPmkCache {
public:
    static constexpr uint32_t kMaxEntries = 8;

    void clear() {
        for (uint32_t i = 0; i < kMaxEntries; ++i) {
            rtlcrypto::secure_zero(&fEntries[i], sizeof(Entry));
        }
        fNext = 0; fHits = 0;
    }

    bool lookup(const char *ssid, uint8_t ssidLen, uint8_t passHash[20], uint8_t outPmk[32]) {
        for (uint32_t i = 0; i < kMaxEntries; ++i) {
            if (fEntries[i].valid && fEntries[i].ssidLen == ssidLen &&
                memcmp(fEntries[i].ssid, ssid, ssidLen) == 0 &&
                rtlcrypto::ct_equal(fEntries[i].passHash, passHash, 20)) {
                memcpy(outPmk, fEntries[i].pmk, 32);
                fHits++;
                return true;
            }
        }
        return false;
    }

    void store(const char *ssid, uint8_t ssidLen, const uint8_t passHash[20], const uint8_t pmk[32]) {
        if (ssidLen == 0 || ssidLen > 32) return;
        uint32_t slot = fNext % kMaxEntries;
        fNext++;
        fEntries[slot].valid = true;
        fEntries[slot].ssidLen = ssidLen;
        memcpy(fEntries[slot].ssid, ssid, ssidLen);
        memcpy(fEntries[slot].passHash, passHash, 20);
        memcpy(fEntries[slot].pmk, pmk, 32);
    }

    uint32_t hits() const { return fHits; }

private:
    struct Entry {
        bool    valid;
        uint8_t ssidLen;
        char    ssid[32];
        uint8_t passHash[20];
        uint8_t pmk[32];
    };
    Entry    fEntries[kMaxEntries]{};
    uint32_t fNext = 0;
    uint32_t fHits = 0;
};

// Dynamic Initial Gain (DIG) controller
class RtlDigController {
public:
    static constexpr uint8_t kMinGain = 0x1C;
    static constexpr uint8_t kMaxGain = 0x3E;

    void reset() { fCurrentGain = 0x20; fRssiEwma = -65; fInitialized = false; }

    uint8_t updateFromRssi(int32_t rssiDbm) {
        if (!fInitialized) { fRssiEwma = rssiDbm; fInitialized = true; }
        else { fRssiEwma = (fRssiEwma * 3 + rssiDbm) / 4; }
        int32_t target = 0x1C + ((fRssiEwma + 90) * (kMaxGain - kMinGain)) / 60;
        if (target < kMinGain) target = kMinGain;
        if (target > kMaxGain) target = kMaxGain;
        int32_t diff = target - static_cast<int32_t>(fCurrentGain);
        if (diff > 2)       fCurrentGain = static_cast<uint8_t>(fCurrentGain + 2);
        else if (diff < -2) fCurrentGain = static_cast<uint8_t>(fCurrentGain - 2);
        return fCurrentGain;
    }

    uint8_t currentGain() const { return fCurrentGain; }
    int32_t rssiEwma() const { return fRssiEwma; }

    static uint8_t rssiToQualityPct(int32_t rssiDbm) {
        if (rssiDbm >= -50) return 100;
        if (rssiDbm <= -95) return 0;
        return static_cast<uint8_t>(((rssiDbm + 95) * 100) / 45);
    }

private:
    uint8_t fCurrentGain = 0x20;
    int32_t fRssiEwma    = -65;
    bool    fInitialized = false;
};

enum RtlRecoveryAction {
    kRecoveryNone            = 0,
    kRecoverySendKeepalive   = 1,
    kRecoveryClearPipeStall  = 2,
    kRecoverySoftReassoc     = 3,
    kRecoveryUsbReset        = 4
};

class RtlLinkWatchdog {
public:
    void reset() { fMissedBeacons = 0; fUsbErrStreak = 0; fTxStallTicks = 0; fResetCount = 0; }
    void onBeaconReceived() { fMissedBeacons = 0; }
    void onUsbSuccess()     { fUsbErrStreak = 0; }

    RtlRecoveryAction tick(bool connected, bool beaconSeen, uint32_t usbErrs, bool txStalled) {
        if (usbErrs > 0) {
            fUsbErrStreak += usbErrs;
            if (fUsbErrStreak >= 12) { fUsbErrStreak = 0; fResetCount++; return kRecoveryUsbReset; }
            if (fUsbErrStreak >= 3)  return kRecoveryClearPipeStall;
        }
        if (!connected) return kRecoveryNone;
        if (txStalled) {
            if (++fTxStallTicks >= 2) { fTxStallTicks = 0; return kRecoveryClearPipeStall; }
        } else { fTxStallTicks = 0; }

        if (!beaconSeen) {
            if (++fMissedBeacons >= 5) { fMissedBeacons = 0; fResetCount++; return kRecoverySoftReassoc; }
            if (fMissedBeacons >= 2)   return kRecoverySendKeepalive;
        } else { fMissedBeacons = 0; }
        return kRecoveryNone;
    }

    uint32_t resetCount() const { return fResetCount; }

private:
    uint32_t fMissedBeacons = 0, fUsbErrStreak = 0, fTxStallTicks = 0, fResetCount = 0;
};

class RtlThermalTracker {
public:
    void setBaseline(uint8_t efuseThermal) {
        fBaseline = (efuseThermal == 0xFF || efuseThermal == 0) ? 0x20 : efuseThermal;
    }
    int8_t computeTxPowerOffset(uint8_t cur) {
        fLastDelta = static_cast<int32_t>(cur) - static_cast<int32_t>(fBaseline);
        if (fLastDelta >= 6) return -2;
        if (fLastDelta >= 3) return -1;
        if (fLastDelta <= -6) return +2;
        if (fLastDelta <= -3) return +1;
        return 0;
    }
    int32_t lastDelta() const { return fLastDelta; }
private:
    uint8_t fBaseline = 0x20;
    int32_t fLastDelta = 0;
};

static inline bool shouldRoamToSibling(int32_t currentRssi, int32_t siblingRssi) {
    return (currentRssi < -78) && (siblingRssi >= currentRssi + 10);
}

static inline bool isDeauthOrDisassoc(uint8_t fc0) {
    const uint8_t type = (fc0 >> 2) & 0x03u;
    const uint8_t sub  = (fc0 >> 4) & 0x0Fu;
    return type == 0 && (sub == 0x0Au || sub == 0x0Cu);
}

// Complete Post-Association GTK Group Key Msg 1 Unwrap + Msg 2 Builder (24/7 uptime!)
struct GtkRekeyResult {
    bool    ok;
    uint8_t gtkId;
    uint8_t gtk16[16];
    uint8_t replayCounter[8];
};

static inline GtkRekeyResult unwrapEapolGroupKeyMsg1(const uint8_t *eapol, size_t len, const uint8_t ptk[48]) {
    GtkRekeyResult res{};
    if (!eapol || len < 99 || eapol[1] != 0x03) return res;
    uint16_t keyInfo = static_cast<uint16_t>((eapol[5] << 8) | eapol[6]);
    bool pairwise = (keyInfo & (1u << 3)) != 0;
    bool ack      = (keyInfo & (1u << 7)) != 0;
    bool mic      = (keyInfo & (1u << 8)) != 0;
    bool secure   = (keyInfo & (1u << 9)) != 0;
    if (pairwise || !ack || !mic || !secure) return res;

    // Verify EAPOL MIC in constant time using KCK (ptk[0..15])
    if (len > 320) return res;
    uint8_t tmp[320];
    memcpy(tmp, eapol, len);
    memset(tmp + 81, 0, 16);
    uint8_t calcMic[16];
    rtlcrypto::eapol_mic(ptk, tmp, len, calcMic);
    if (!rtlcrypto::ct_equal(calcMic, eapol + 81, 16)) return res;

    uint16_t kdLen = static_cast<uint16_t>((eapol[97] << 8) | eapol[98]);
    if (kdLen < 24 || kdLen > 208 || static_cast<size_t>(99 + kdLen) > len) return res;

    uint8_t plain[208];
    if (!rtlcrypto::aes_unwrap(ptk + 16, eapol + 99, kdLen, plain)) return res;

    uint16_t plen = static_cast<uint16_t>(kdLen - 8);
    uint16_t p = 0;
    while (p + 2 <= plen) {
        uint8_t t = plain[p], l = plain[p + 1];
        if (t == 0xDD && l == 0) break;
        if (p + 2 + l > plen) break;
        if (t == 0xDD && l >= 22 && plain[p+2] == 0x00 && plain[p+3] == 0x0F &&
            plain[p+4] == 0xAC && plain[p+5] == 0x01) {
            res.gtkId = plain[p + 6] & 0x03u;
            memcpy(res.gtk16, plain + p + 8, 16);
            memcpy(res.replayCounter, eapol + 9, 8);
            res.ok = true;
            break;
        }
        p = static_cast<uint16_t>(p + 2 + l);
    }
    rtlcrypto::secure_zero(plain, sizeof(plain));
    return res;
}

static inline uint16_t buildEapolGroupKeyMsg2(uint8_t *out, size_t outCap,
                                              const uint8_t replayCounter[8],
                                              const uint8_t ptk[48]) {
    if (!out || outCap < 99) return 0;
    memset(out, 0, 99);
    out[0] = 1; out[1] = 3; // EAPOL v1, Key
    out[2] = 0; out[3] = 95;
    out[4] = 2; // RSN
    out[5] = 0x03; out[6] = 0x02; // Secure + MIC + v2 (Group, not pairwise)
    memcpy(out + 9, replayCounter, 8);
    uint8_t mic[16];
    rtlcrypto::eapol_mic(ptk, out, 99, mic);
    memcpy(out + 81, mic, 16);
    return 99;
}

} // namespace rtl8188f

#endif /* RTL_WIFI_STABILITY_HPP */
