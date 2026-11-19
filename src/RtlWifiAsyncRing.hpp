#ifndef RTL_WIFI_ASYNC_RING_HPP
#define RTL_WIFI_ASYNC_RING_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include "RtlWifiDataPlane.hpp"

struct RtlTxSlot {
    IOBufferMemoryDescriptor *md;
    uint8_t                  *buf;
    volatile bool             inUse;
};

template <uint32_t kCapacity>
class RtlAsyncTxRingT {
public:
    bool init() {
        fLock = IOSimpleLockAlloc();
        if (!fLock) return false;
        fInflight = 0;
        fNextHint = 0;
        fStopped  = false;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            fSlots[i].md = IOBufferMemoryDescriptor::withOptions(
                kIODirectionOut, rtl8188f::kTxBufSize, 1);
            if (!fSlots[i].md) {
                destroy();
                return false;
            }
            fSlots[i].buf = reinterpret_cast<uint8_t *>(fSlots[i].md->getBytesNoCopy());
            fSlots[i].inUse = false;
        }
        return true;
    }

    void destroy() {
        fStopped = true;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (fSlots[i].md) {
                fSlots[i].md->release();
                fSlots[i].md = nullptr;
                fSlots[i].buf = nullptr;
            }
            fSlots[i].inUse = false;
        }
        if (fLock) {
            IOSimpleLockFree(fLock);
            fLock = nullptr;
        }
    }

    int acquireSlot() {
        if (fStopped || !fLock) return -1;
        IOSimpleLockLock(fLock);
        int found = -1;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            uint32_t idx = (fNextHint + i) % kCapacity;
            if (!fSlots[idx].inUse) {
                fSlots[idx].inUse = true;
                fNextHint = (idx + 1) % kCapacity;
                fInflight++;
                found = static_cast<int>(idx);
                break;
            }
        }
        IOSimpleLockUnlock(fLock);
        return found;
    }

    void releaseSlot(uint32_t idx) {
        if (!fLock || idx >= kCapacity) return;
        IOSimpleLockLock(fLock);
        if (fSlots[idx].inUse) {
            fSlots[idx].inUse = false;
            if (fInflight > 0) fInflight--;
        }
        IOSimpleLockUnlock(fLock);
    }

    RtlTxSlot &slot(uint32_t idx) { return fSlots[idx]; }
    int32_t inflight() const { return fInflight; }
    void setStopped(bool s) { fStopped = s; }

private:
    RtlTxSlot        fSlots[kCapacity]{};
    IOSimpleLock    *fLock     = nullptr;
    uint32_t         fNextHint = 0;
    volatile int32_t fInflight = 0;
    volatile bool    fStopped  = false;
};

using RtlTxBeRing = RtlAsyncTxRingT<rtl8188f::kTxBeRingSize>;
using RtlTxHiRing = RtlAsyncTxRingT<rtl8188f::kTxHiRingSize>;

#endif /* RTL_WIFI_ASYNC_RING_HPP */
