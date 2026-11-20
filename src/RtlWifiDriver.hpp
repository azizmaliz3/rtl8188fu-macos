#ifndef RtlWifiDriver_hpp
#define RtlWifiDriver_hpp
#include <IOKit/IOService.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IONetworkStats.h>
#include "RtlWifiShared.h"
#include "RtlWifiDataPlane.hpp"
#include "RtlWifiStability.hpp"
#include "RtlWifiAsyncRing.hpp"

struct RtlReg32Val;
struct RtlRfRegVal;

class IOBufferMemoryDescriptor;

class RtlWifiDriver : public IOEthernetController {
    OSDeclareDefaultStructors(RtlWifiDriver)
    IOUSBHostInterface *fInterface;
    IOUSBHostPipe *fPipeIn;     // bulk IN  0x81
    IOUSBHostPipe *fPipeOut2;   // bulk OUT 0x02
    IOUSBHostPipe *fPipeOut3;   // bulk OUT 0x03

    // Raw vendor-request register access (request 0x05, 0xC0 read / 0x40 write)
    IOReturn readReg(uint16_t reg, void *buf, uint16_t len);
    IOReturn writeReg(uint16_t reg, const void *buf, uint16_t len);
    uint8_t  rd8(uint16_t reg);
    uint16_t rd16(uint16_t reg);
    uint32_t rd32(uint16_t reg);
    void     wr8(uint16_t reg, uint8_t v);
    void     wr16(uint16_t reg, uint16_t v);
    void     wr32(uint16_t reg, uint32_t v);

    // Bring-up steps (sequence taken from Linux rtl8xxxu, RTL8188FU path)
    IOReturn powerOn();
    void     reset8051();
    IOReturn writeN(uint16_t addr, const uint8_t *buf, uint32_t len);
    IOReturn downloadFirmware();
    IOReturn startFirmware();

    // Queue/MAC init (rtl8xxxu_init_queue_reserved_page / _priority / _init_mac)
    uint8_t  fEpTxHigh, fEpTxNormal, fEpTxLow, fEpTxCount;
    void     configEndpoints(uint8_t nrOutEps);
    void     initQueueReservedPage();
    IOReturn initQueuePriority();
    IOReturn initMac();

    // PHY / RF init and deferred MAC steps (rtl8188fu_init_phy_bb / _init_phy_rf / init_device tail)
    uint8_t  fChipCut;
    IOReturn writeRfReg(uint8_t reg, uint32_t data);
    uint32_t initPhyRegs(const struct RtlReg32Val *tbl, unsigned int len);
    IOReturn initPhyBb();
    IOReturn initPhyRf();
    IOReturn postRfInit();
    IOReturn initMacTail();
    uint32_t readRfReg(uint8_t reg);
    IOReturn initCalib();
    void     setTxPower(uint8_t channel);
    void     spurCalibration(uint8_t channel);
    IOReturn setChannel20(uint8_t channel);
    void     initStatistics();
    void     rxProbe(uint8_t channel, char tag);
    IOReturn txMgmt(const uint8_t *frame, uint16_t len, uint8_t rate, uint8_t channel, char tag);
    IOReturn txProbeRequest(uint8_t channel, uint8_t rate, char tag);
    IOReturn txAuth(uint8_t channel, uint8_t rate, char tag);
    IOReturn txAssoc(uint8_t channel, uint8_t rate, char tag);
    IOReturn txData(const uint8_t *frame, uint16_t len, uint8_t rate, uint8_t channel, char tag);
    IOReturn txDataSec(const uint8_t *frame, uint16_t len, uint8_t rate, uint8_t channel, char tag, uint32_t secType, bool bmc);
    void     runDhcpTest(uint8_t channel);
    uint8_t  fRxLoops;
    bool     fStopOnOffer, fHaveOffer, fOfferProt, fOfferBcast;
    uint8_t  fOfferIp[4], fOfferSrv[4];
    uint32_t fMsg3Retx;
    uint64_t fTxPn;
    // Network interface (IOEthernetController side)
    bool     startHardware(IOService *provider);
    bool     createMediumTables();
    IOEthernetInterface *fNetif;
    bool     fNetifEnabled, fKeysUp;
    uint32_t fNetTxDropped;
    // Data path (v25): asynchronous receive on bulk-IN, transmit from outputPacket()
    IOBufferMemoryDescriptor *fRxMd[rtl8188f::kRxRingSize];
    RtlTxBeRing fTxBeRing;
    RtlTxHiRing fTxHiRing;
    rtl8188f::RtlDigController fDig;
    rtl8188f::RtlLinkWatchdog fWatchdog;
    rtl8188f::RtlThermalTracker fThermal;
    rtl8188f::RtlPmkCache fPmkCache;
    rtl8188f::RtlRxReorderWindow<mbuf_t> fRxReorder;
    IONetworkStats *fNetStats;
    bool     fIsEncrypted;
    int8_t   fApSecOffset;
    uint32_t fAmsduSubframes;
    uint32_t fRoamCount;
    IOReturn setChannelBw(uint8_t primaryCh, int8_t secOffset);
    void     setBssidFilter(bool enable);
    void     handleGtkRekey(const uint8_t *eapol, size_t len);
    IOTimerEventSource *fWatchdogTimer;
    uint8_t  fH2cBox;
    bool     fHt40Active;
    bool     fBeaconSeen;
    uint32_t fUsbErrWindow;
    uint32_t fGtkRekeyCount;
    uint32_t fUsbStallRecoveries;
    uint32_t fDriverState;
    int32_t  fLastRssiDbm;
    char     fTargetSsid[RTL_WIFI_MAX_SSID_LEN + 1];
    uint8_t  fTargetSsidLen;
    RtlWifiScanList fScanList;
    void     sendH2c(const rtl8188f::H2cCommand &cmd);
    void     txBeComplete(unsigned idx, IOReturn status);
    void     txHiComplete(unsigned idx, IOReturn status);
    static void txBeCompleteStatic(void *owner, void *param, IOReturn status, uint32_t bytes);
    static void txHiCompleteStatic(void *owner, void *param, IOReturn status, uint32_t bytes);
    void     onWatchdogTick(IOTimerEventSource *sender);
    bool     connectToCurrentTarget();
    volatile int32_t fRxInflight;
    volatile bool    fRxRunning;
    uint32_t fRxErrRun;
    uint8_t  *fTxFrame;
    uint8_t  fGtkCipher;
    uint32_t fNetTxPackets, fNetTxErrors, fNetRxPackets, fNetRxDropped;
    void     startRx();
    void     stopRx();
    void     submitRx(unsigned idx);
    void     rxComplete(unsigned idx, IOReturn status, uint32_t bytes);
    void     handleRx(const uint8_t *buf, uint32_t len);
    bool     rxFrame(const uint8_t *f, uint32_t len, uint32_t desc0);
    static void rxCompleteStatic(void *owner, void *param, IOReturn status, uint32_t bytes);
    IOReturn txEapolMsg2(uint8_t channel, char tag);
    void     runHandshake(uint8_t channel);
    IOReturn txEapolMsg4(uint8_t channel, char tag);
    void     camWrite(uint8_t idx, uint8_t cipher, uint8_t keyId, bool group, const uint8_t *mac, const uint8_t *key16);
    void     installKeys(uint8_t gtkCipher, uint8_t gtkId, const uint8_t *gtk16, uint8_t channel);
    uint8_t  fMsg3[400];
    uint16_t fMsg3Len;
    bool     fHaveMsg3; volatile bool fTxStalled;
uint8_t fPendingGtk[320];
uint16_t fPendingGtkLen;
volatile bool fHavePendingGtk;
    void     selfTest();
    uint8_t  fPmk[32];
    uint8_t  fPtk[48];
    uint8_t  fAnonce[32];
    uint8_t  fSnonce[32];
    uint8_t  fReplay[8];
    uint8_t  fOwnRsn[22];
    bool     fHavePmk, fHavePtk, fHaveMsg1, fStopOnMsg1, fStopOnMsg3, fNewMsg1;
    uint8_t  fApBssid[6];
    bool     fHaveAp;
    uint8_t  fApIes[640];
    uint16_t fApIeLen;
    uint16_t fApCap;
    uint8_t  fApChannel;
    void     setStationMode();
    void     setCrystalCap(uint8_t cap);
    void     scanAll();

    // eFuse (rtl8xxxu_read_efuse8 / rtl8xxxu_read_efuse)
    IOReturn readEfuse8(uint16_t offset, uint8_t *data);
    IOReturn readEfuse();
    uint8_t  fEfuse[512];

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) override;

    // IOEthernetController overrides
    IOOutputQueue *createOutputQueue() override;
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    IOReturn enable(IONetworkInterface *netif) override;
    IOReturn disable(IONetworkInterface *netif) override;
    IOReturn selectMedium(const IONetworkMedium *medium) override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
    IOReturn setPromiscuousMode(bool active) override;
    UInt32   outputPacket(mbuf_t packet, void *param) override;

    // IOUserClient runtime control API (used by RtlWifiMenuBar.app & rtl8188ctl)
    IOReturn ucGetStatus(RtlWifiStatus *out);
    IOReturn ucStartScan();
    IOReturn ucGetScanResults(RtlWifiScanList *out);
    IOReturn ucConnect(const RtlWifiConnectReq *req);
    IOReturn ucDisconnect();
    IOReturn ucGetDiagnostics(RtlWifiDiagnostics *out);
    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;
};
#endif
