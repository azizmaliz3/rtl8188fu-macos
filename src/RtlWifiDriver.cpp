#include "RtlWifiDriver.hpp"
#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(RtlWifiDriver, IOService)

IOReturn RtlWifiDriver::readReg(uint16_t reg, void *buf, uint16_t len) {
    StandardUSB::DeviceRequest req;
    req.bmRequestType = 0xC0;   // device-to-host, vendor, device
    req.bRequest = 0x05;
    req.wValue = reg;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    return fDevice->deviceRequest(this, req, buf, got, 1000);
}

bool RtlWifiDriver::start(IOService *provider) {
    fDevice = nullptr;
    if (!super::start(provider)) return false;
    fDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!fDevice) { IOLog("RtlWifi: provider is not IOUSBHostDevice\n"); return false; }
    if (!fDevice->open(this)) { IOLog("RtlWifi: open failed\n"); fDevice = nullptr; return false; }
    uint8_t r8 = 0;
    uint32_t r32 = 0, r68 = 0;
    IOReturn a = readReg(0x00FC, &r8, 1);
    IOReturn b = readReg(0x00F0, &r32, 4);
    IOReturn c = readReg(0x0068, &r68, 4);
    IOLog("RtlWifi: reg FC=0x%02x(%x) F0=0x%08x(%x) 68=0x%08x(%x)\n", r8, a, r32, b, r68, c);
    return true;
}

void RtlWifiDriver::stop(IOService *provider) {
    if (fDevice) { fDevice->close(this); fDevice = nullptr; }
    IOLog("RtlWifi: stopped\n");
    super::stop(provider);
}