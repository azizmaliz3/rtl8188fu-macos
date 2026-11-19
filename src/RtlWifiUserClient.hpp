#ifndef RTL_WIFI_USER_CLIENT_HPP
#define RTL_WIFI_USER_CLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "RtlWifiShared.h"

class RtlWifiDriver;

class RtlWifiUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RtlWifiUserClient)

public:
    virtual bool initWithTask(task_t owningTask,
                              void *securityToken,
                              UInt32 type,
                              OSDictionary *properties) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target,
                                    void *reference) override;

private:
    RtlWifiDriver *fDriver;
    task_t         fTask;

    static const IOExternalMethodDispatch sMethods[kRtlMethodCount];

    static IOReturn sGetStatus(RtlWifiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sStartScan(RtlWifiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sGetScanResults(RtlWifiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sConnect(RtlWifiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sDisconnect(RtlWifiUserClient *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sGetDiagnostics(RtlWifiUserClient *target, void *ref, IOExternalMethodArguments *args);
};

#endif /* RTL_WIFI_USER_CLIENT_HPP */
