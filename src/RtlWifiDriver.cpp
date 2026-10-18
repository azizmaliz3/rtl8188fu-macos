#include "RtlWifiDriver.hpp"
#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(RtlWifiDriver, IOService)

bool RtlWifiDriver::start(IOService *provider) {
    if (!super::start(provider)) return false;
    IOLog("RtlWifi: started, provider %s\n", provider ? provider->getName() : "?");
    return true;
}

void RtlWifiDriver::stop(IOService *provider) {
    IOLog("RtlWifi: stopped\n");
    super::stop(provider);
}