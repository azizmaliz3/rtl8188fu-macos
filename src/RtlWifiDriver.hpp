#ifndef RtlWifiDriver_hpp
#define RtlWifiDriver_hpp
#include <IOKit/IOService.h>

class RtlWifiDriver : public IOService {
    OSDeclareDefaultStructors(RtlWifiDriver)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
#endif