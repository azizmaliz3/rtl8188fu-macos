#ifndef RtlWifiDriver_hpp
#define RtlWifiDriver_hpp
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

struct RtlReg32Val;
struct RtlRfRegVal;

class RtlWifiDriver : public IOService {
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
    IOReturn txEapolMsg2(uint8_t channel, char tag);
    void     runHandshake(uint8_t channel);
    IOReturn txEapolMsg4(uint8_t channel, char tag);
    void     camWrite(uint8_t idx, uint8_t cipher, uint8_t keyId, bool group, const uint8_t *mac, const uint8_t *key16);
    void     installKeys(uint8_t gtkCipher, uint8_t gtkId, const uint8_t *gtk16, uint8_t channel);
    uint8_t  fMsg3[400];
    uint16_t fMsg3Len;
    bool     fHaveMsg3;
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
    uint8_t  fApIes[256];
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
};
#endif
