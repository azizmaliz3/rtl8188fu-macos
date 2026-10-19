#ifndef RtlWifiDriver_hpp
#define RtlWifiDriver_hpp
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>

class RtlWifiDriver : public IOService {
    OSDeclareDefaultStructors(RtlWifiDriver)
    IOUSBHostDevice *fDevice;
    IOReturn readReg(uint16_t reg, void *buf, uint16_t len);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
#endif