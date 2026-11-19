#ifndef RTL_WIFI_SHARED_H
#define RTL_WIFI_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_WIFI_ABI_VERSION     3
#define RTL_WIFI_MAX_SSID_LEN    32
#define RTL_WIFI_MAX_PASS_LEN    64
#define RTL_WIFI_MAX_SCAN_COUNT  32

enum RtlWifiUserClientSelector {
    kRtlMethodGetStatus      = 0,
    kRtlMethodStartScan      = 1,
    kRtlMethodGetScanResults = 2,
    kRtlMethodConnect        = 3,
    kRtlMethodDisconnect     = 4,
    kRtlMethodGetDiagnostics = 5,
    kRtlMethodCount          = 6
};

enum RtlWifiSecurityType {
    kRtlSecOpen    = 0,
    kRtlSecWpa2Psk = 1
};

enum RtlWifiState {
    kRtlStateDisconnected = 0,
    kRtlStateScanning     = 1,
    kRtlStateConnecting   = 2,
    kRtlStateConnected    = 3,
    kRtlStateRecovering   = 4
};

struct __attribute__((packed)) RtlWifiStatus {
    uint32_t abiVersion;      // 4
    uint32_t state;           // 8
    uint32_t channel;         // 12
    int32_t  rssiDbm;         // 16
    uint32_t linkSpeedMbps;   // 20
    uint32_t txPackets;       // 24
    uint32_t txErrors;        // 28
    uint32_t rxPackets;       // 32
    uint32_t rxErrors;        // 36
    uint8_t  macAddr[6];      // 42
    uint8_t  bssid[6];        // 48
    uint8_t  ssidLen;         // 49
    uint8_t  ht40Active;      // 50
    uint8_t  linkQualityPct;  // 51
    uint8_t  digGain;         // 52
    char     ssid[RTL_WIFI_MAX_SSID_LEN + 1]; // 85
    uint8_t  pad[3];          // 88
};

struct __attribute__((packed)) RtlWifiDiagnostics {
    uint32_t abiVersion;         // 4
    uint32_t txRingInflightBe;   // 8
    uint32_t txRingInflightVo;   // 12
    uint32_t rxRingInflight;     // 16
    uint32_t watchdogResets;     // 20
    uint32_t gtkRekeyCount;      // 24
    uint32_t usbStallRecoveries; // 28
    int32_t  thermalDelta;       // 32
    uint32_t amsduSubframes;     // 36
    uint32_t reorderFlushes;     // 40
    uint32_t pmkCacheHits;       // 44
    uint32_t roamCount;          // 48
};

struct __attribute__((packed)) RtlWifiScanEntry {
    uint8_t  bssid[6];        // 6
    uint8_t  channel;         // 7
    uint8_t  security;        // 8
    int8_t   rssiDbm;         // 9
    uint8_t  ssidLen;         // 10
    uint8_t  htCapable;       // 11
    int8_t   ht40SecOffset;   // 12 (-1=HT40-, 0=HT20, +1=HT40+)
    char     ssid[RTL_WIFI_MAX_SSID_LEN + 1]; // 45
    uint8_t  pad[3];          // 48
};

struct __attribute__((packed)) RtlWifiScanList {
    uint32_t abiVersion;
    uint32_t count;
    struct RtlWifiScanEntry entries[RTL_WIFI_MAX_SCAN_COUNT];
};

struct __attribute__((packed)) RtlWifiConnectReq {
    uint32_t abiVersion;      // 4
    uint32_t security;        // 8
    uint8_t  ssidLen;         // 9
    uint8_t  passLen;         // 10
    uint8_t  preferHt40;      // 11
    uint8_t  reserved;        // 12
    char     ssid[RTL_WIFI_MAX_SSID_LEN + 1];       // 45
    char     passphrase[RTL_WIFI_MAX_PASS_LEN + 1]; // 110
    uint8_t  pad[2];          // 112
};

#ifdef __cplusplus
}
#endif

#endif /* RTL_WIFI_SHARED_H */
