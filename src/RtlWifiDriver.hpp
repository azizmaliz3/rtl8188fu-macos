#ifndef RtlWifiDriver_hpp
#define RtlWifiDriver_hpp
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>

class RtlWifiDriver : public IOService {
    OSDeclareDefaultStructors(RtlWifiDriver)
    IOUSBHostDevice *fDevice;

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

    // eFuse (rtl8xxxu_read_efuse8 / rtl8xxxu_read_efuse)
    IOReturn readEfuse8(uint16_t offset, uint8_t *data);
    IOReturn readEfuse();
    uint8_t  fEfuse[512];

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
#endif
