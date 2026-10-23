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
