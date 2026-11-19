#include "RtlWifiUserClient.hpp"
#include "RtlWifiDriver.hpp"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RtlWifiUserClient, IOUserClient)

const IOExternalMethodDispatch RtlWifiUserClient::sMethods[kRtlMethodCount] = {
    { (IOExternalMethodAction)&RtlWifiUserClient::sGetStatus,      0, 0, 0, sizeof(RtlWifiStatus) },
    { (IOExternalMethodAction)&RtlWifiUserClient::sStartScan,      0, 0, 0, 0 },
    { (IOExternalMethodAction)&RtlWifiUserClient::sGetScanResults, 0, 0, 0, sizeof(RtlWifiScanList) },
    { (IOExternalMethodAction)&RtlWifiUserClient::sConnect,        0, sizeof(RtlWifiConnectReq), 0, 0 },
    { (IOExternalMethodAction)&RtlWifiUserClient::sDisconnect,     0, 0, 0, 0 },
    { (IOExternalMethodAction)&RtlWifiUserClient::sGetDiagnostics, 0, 0, 0, sizeof(RtlWifiDiagnostics) },
};

bool RtlWifiUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityToken, type, properties)) return false;
    fTask = owningTask;
    fDriver = nullptr;
    return true;
}

bool RtlWifiUserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    fDriver = OSDynamicCast(RtlWifiDriver, provider);
    return fDriver != nullptr;
}

void RtlWifiUserClient::stop(IOService *provider) {
    fDriver = nullptr;
    super::stop(provider);
}

IOReturn RtlWifiUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn RtlWifiUserClient::externalMethod(uint32_t selector,
                                           IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *dispatch,
                                           OSObject *target,
                                           void *reference) {
    if (selector >= kRtlMethodCount) return kIOReturnUnsupported;
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target   = this;
    return super::externalMethod(selector, args, dispatch, target, reference);
}

IOReturn RtlWifiUserClient::sGetStatus(RtlWifiUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->fDriver || !a->structureOutput) return kIOReturnBadArgument;
    return t->fDriver->ucGetStatus(reinterpret_cast<RtlWifiStatus *>(a->structureOutput));
}

IOReturn RtlWifiUserClient::sStartScan(RtlWifiUserClient *t, void *, IOExternalMethodArguments *) {
    return (t && t->fDriver) ? t->fDriver->ucStartScan() : kIOReturnBadArgument;
}

IOReturn RtlWifiUserClient::sGetScanResults(RtlWifiUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->fDriver || !a->structureOutput) return kIOReturnBadArgument;
    return t->fDriver->ucGetScanResults(reinterpret_cast<RtlWifiScanList *>(a->structureOutput));
}

IOReturn RtlWifiUserClient::sConnect(RtlWifiUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->fDriver || !a->structureInput) return kIOReturnBadArgument;
    const auto *req = reinterpret_cast<const RtlWifiConnectReq *>(a->structureInput);
    if (req->abiVersion != RTL_WIFI_ABI_VERSION || req->ssidLen == 0 || req->ssidLen > RTL_WIFI_MAX_SSID_LEN)
        return kIOReturnBadArgument;
    return t->fDriver->ucConnect(req);
}

IOReturn RtlWifiUserClient::sDisconnect(RtlWifiUserClient *t, void *, IOExternalMethodArguments *) {
    return (t && t->fDriver) ? t->fDriver->ucDisconnect() : kIOReturnBadArgument;
}

IOReturn RtlWifiUserClient::sGetDiagnostics(RtlWifiUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->fDriver || !a->structureOutput) return kIOReturnBadArgument;
    return t->fDriver->ucGetDiagnostics(reinterpret_cast<RtlWifiDiagnostics *>(a->structureOutput));
}
