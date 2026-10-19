#include "RtlWifiDriver.hpp"
#include "rtl8188fufw.h"          // generated: rtl8188fufw_bin[], rtl8188fufw_bin_len
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

#define super IOService
OSDefineMetaClassAndStructors(RtlWifiDriver, IOService)

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

// Register map (from Linux rtl8xxxu regs.h)
#define REG_SYS_FUNC      0x0002
#define REG_APS_FSMCO     0x0004
#define REG_RSV_CTRL      0x001C
#define REG_MCU_FW_DL     0x0080
#define REG_CR            0x0100
#define REG_TRXFF_BNDY    0x0114
#define REG_FW_START_ADDR 0x1000
#define REG_SYS_ISO_CTRL  0x0000
#define REG_SYS_CLKR      0x0008
#define REG_EFUSE_CTRL    0x0030
#define REG_EFUSE_ACCESS  0x00CF
#define EFUSE_MAP_LEN     512
#define EFUSE_ACCESS_ON   0x69
#define EFUSE_ACCESS_OFF  0x00

#define FW_HEADER_SIZE    32      // sizeof(struct rtl8xxxu_firmware_header)
#define FW_PAGE_SIZE      4096    // RTL_FW_PAGE_SIZE
#define WRITEN_BLOCK      128     // rtl8188fu_fops.writeN_block_size
#define POLL_MAX_REG      500     // RTL8XXXU_MAX_REG_POLL
#define POLL_MAX_FW       1000    // RTL8XXXU_FIRMWARE_POLL_MAX

// ---------- raw register access ----------

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

IOReturn RtlWifiDriver::writeReg(uint16_t reg, const void *buf, uint16_t len) {
    StandardUSB::DeviceRequest req;
    req.bmRequestType = 0x40;   // host-to-device, vendor, device
    req.bRequest = 0x05;
    req.wValue = reg;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    return fDevice->deviceRequest(this, req, const_cast<void *>(buf), got, 1000);
}

uint8_t  RtlWifiDriver::rd8(uint16_t reg)  { uint8_t  v = 0; readReg(reg, &v, 1); return v; }
uint16_t RtlWifiDriver::rd16(uint16_t reg) { uint16_t v = 0; readReg(reg, &v, 2); return v; }
uint32_t RtlWifiDriver::rd32(uint16_t reg) { uint32_t v = 0; readReg(reg, &v, 4); return v; }
void RtlWifiDriver::wr8(uint16_t reg, uint8_t v)   { writeReg(reg, &v, 1); }
void RtlWifiDriver::wr16(uint16_t reg, uint16_t v) { writeReg(reg, &v, 2); }
void RtlWifiDriver::wr32(uint16_t reg, uint32_t v) { writeReg(reg, &v, 4); }

// ---------- power on (rtl8188fu_power_on) ----------

IOReturn RtlWifiDriver::powerOn() {
    uint8_t v8;
    int n;

    // disabled_to_emu
    v8 = rd8(REG_APS_FSMCO + 1);
    v8 &= ~((BIT(12) | BIT(11)) >> 8);        // APS_FSMCO_PCIE | HW_SUSPEND
    wr8(REG_APS_FSMCO + 1, v8);
    v8 = rd8(0xC4);
    v8 &= ~BIT(4);
    wr8(0xC4, v8);

    // emu_to_active
    v8 = rd8(REG_APS_FSMCO + 1);
    v8 &= ~(BIT(10) >> 8);                    // APS_FSMCO_SW_LPS
    wr8(REG_APS_FSMCO + 1, v8);

    for (n = POLL_MAX_REG; n; n--) {          // wait 0x04[17] power ready
        if (rd32(REG_APS_FSMCO) & BIT(17)) break;
        IODelay(10);
    }
    if (!n) return kIOReturnTimeout;

    v8 = rd8(REG_APS_FSMCO + 1);
    v8 &= ~(BIT(15) >> 8);                    // HW_POWERDOWN
    wr8(REG_APS_FSMCO + 1, v8);
    v8 = rd8(REG_APS_FSMCO + 1);
    v8 &= ~(BIT(11) >> 8);                    // HW_SUSPEND
    wr8(REG_APS_FSMCO + 1, v8);
    v8 = rd8(REG_APS_FSMCO + 1);
    v8 |= (BIT(8) >> 8);                      // MAC_ENABLE
    wr8(REG_APS_FSMCO + 1, v8);

    for (n = POLL_MAX_REG; n; n--) {          // poll MAC_ENABLE back to 0
        if ((rd32(REG_APS_FSMCO) & BIT(8)) == 0) break;
        IODelay(10);
    }
    if (!n) return kIOReturnTimeout;

    wr8(0x27, 0x35);

    wr8(REG_CR, 0);
    uint16_t cr = rd16(REG_CR);
    cr |= (BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(9) | BIT(10));
    wr16(REG_CR, cr);
    return kIOReturnSuccess;
}

// ---------- 8051 reset (rtl8xxxu_reset_8051) ----------

void RtlWifiDriver::reset8051() {
    uint8_t v8 = rd8(REG_RSV_CTRL + 1);
    v8 &= ~BIT(0);
    wr8(REG_RSV_CTRL + 1, v8);

    uint16_t sysFunc = rd16(REG_SYS_FUNC);
    sysFunc &= ~BIT(10);                      // SYS_FUNC_CPU_ENABLE
    wr16(REG_SYS_FUNC, sysFunc);

    v8 = rd8(REG_RSV_CTRL + 1);
    v8 |= BIT(0);
    wr8(REG_RSV_CTRL + 1, v8);

    sysFunc |= BIT(10);
    wr16(REG_SYS_FUNC, sysFunc);
}

// ---------- firmware download (rtl8xxxu_download_firmware) ----------

IOReturn RtlWifiDriver::writeN(uint16_t addr, const uint8_t *buf, uint32_t len) {
    while (len) {
        uint16_t chunk = (len > WRITEN_BLOCK) ? WRITEN_BLOCK : (uint16_t)len;
        IOReturn r = writeReg(addr, buf, chunk);
        if (r != kIOReturnSuccess) return r;
        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    return kIOReturnSuccess;
}

IOReturn RtlWifiDriver::downloadFirmware() {
    const uint8_t *fw = rtl8188fufw_bin;
    uint32_t total = rtl8188fufw_bin_len;
    if (total <= FW_HEADER_SIZE) return kIOReturnBadArgument;

    uint16_t sig = (uint16_t)(fw[0] | (fw[1] << 8));
    setProperty("fw-signature", (uint64_t)sig, 16);

    const uint8_t *p = fw + FW_HEADER_SIZE;
    uint32_t size = total - FW_HEADER_SIZE;

    uint8_t v8 = rd8(REG_SYS_FUNC + 1);
    v8 |= 4;
    wr8(REG_SYS_FUNC + 1, v8);

    uint16_t v16 = rd16(REG_SYS_FUNC);        // 8051 enable
    v16 |= BIT(10);
    wr16(REG_SYS_FUNC, v16);

    v8 = rd8(REG_MCU_FW_DL);
    if (v8 & BIT(7)) {                        // MCU_FW_RAM_SEL: firmware already running
        wr8(REG_MCU_FW_DL, 0x00);
        reset8051();
    }

    v8 = rd8(REG_MCU_FW_DL);                  // firmware download enable
    v8 |= BIT(0);
    wr8(REG_MCU_FW_DL, v8);

    uint32_t v32 = rd32(REG_MCU_FW_DL);       // 8051 reset
    v32 &= ~BIT(19);
    wr32(REG_MCU_FW_DL, v32);

    v8 = rd8(REG_MCU_FW_DL);                  // reset checksum report
    v8 |= BIT(2);
    wr8(REG_MCU_FW_DL, v8);

    uint32_t pages = size / FW_PAGE_SIZE;
    uint32_t remainder = size % FW_PAGE_SIZE;
    uint32_t i;
    IOReturn ret = kIOReturnSuccess;

    for (i = 0; i < pages; i++) {
        v8 = rd8(REG_MCU_FW_DL + 2) & 0xF8;
        v8 |= i;
        wr8(REG_MCU_FW_DL + 2, v8);
        ret = writeN(REG_FW_START_ADDR, p, FW_PAGE_SIZE);
        if (ret != kIOReturnSuccess) goto done;
        p += FW_PAGE_SIZE;
    }
    if (remainder) {
        v8 = rd8(REG_MCU_FW_DL + 2) & 0xF8;
        v8 |= i;
        wr8(REG_MCU_FW_DL + 2, v8);
        ret = writeN(REG_FW_START_ADDR, p, remainder);
    }

done:
    v16 = rd16(REG_MCU_FW_DL);                // firmware download disable
    v16 &= ~BIT(0);
    wr16(REG_MCU_FW_DL, v16);
    setProperty("fw-pages", (uint64_t)pages, 32);
    setProperty("fw-remainder", (uint64_t)remainder, 32);
    return ret;
}

// ---------- firmware start (rtl8xxxu_start_firmware) ----------

IOReturn RtlWifiDriver::startFirmware() {
    int i;
    uint32_t v32;

    for (i = 0; i < POLL_MAX_FW; i++) {       // poll checksum report
        v32 = rd32(REG_MCU_FW_DL);
        if (v32 & BIT(2)) break;
    }
    if (i == POLL_MAX_FW) return kIOReturnTimeout;

    v32 = rd32(REG_MCU_FW_DL);
    v32 |= BIT(1);                            // MCU_FW_DL_READY
    v32 &= ~BIT(6);                           // clear MCU_WINT_INIT_READY
    wr32(REG_MCU_FW_DL, v32);

    reset8051();                              // firmware starts running after reset

    for (i = 0; i < POLL_MAX_FW; i++) {       // wait for firmware ready
        v32 = rd32(REG_MCU_FW_DL);
        if (v32 & BIT(6)) break;
        IODelay(100);
    }
    if (i == POLL_MAX_FW) return kIOReturnTimeout;
    return kIOReturnSuccess;
}

// ---------- eFuse read (rtl8xxxu_read_efuse8 / rtl8xxxu_read_efuse) ----------

IOReturn RtlWifiDriver::readEfuse8(uint16_t offset, uint8_t *data) {
    wr8(REG_EFUSE_CTRL + 1, offset & 0xff);
    uint8_t v8 = rd8(REG_EFUSE_CTRL + 2);
    v8 &= 0xfc;
    v8 |= (offset >> 8) & 0x03;
    wr8(REG_EFUSE_CTRL + 2, v8);

    v8 = rd8(REG_EFUSE_CTRL + 3);
    wr8(REG_EFUSE_CTRL + 3, v8 & 0x7f);

    uint32_t v32 = 0;
    int i;
    for (i = 0; i < POLL_MAX_REG; i++) {      // poll for data ready (bit 31)
        v32 = rd32(REG_EFUSE_CTRL);
        if (v32 & BIT(31)) break;
    }
    if (i == POLL_MAX_REG) return kIOReturnIOError;

    IODelay(50);
    v32 = rd32(REG_EFUSE_CTRL);
    *data = (uint8_t)(v32 & 0xff);
    return kIOReturnSuccess;
}

IOReturn RtlWifiDriver::readEfuse() {
    IOReturn ret = kIOReturnSuccess;
    uint16_t v16, addr = 0, offset, mapAddr;
    uint8_t header = 0, ext = 0, val8 = 0, wordMask;
    int i;

    wr8(REG_EFUSE_ACCESS, EFUSE_ACCESS_ON);

    v16 = rd16(REG_SYS_ISO_CTRL);             // 1.2V power
    if (!(v16 & BIT(15))) { v16 |= BIT(15); wr16(REG_SYS_ISO_CTRL, v16); }

    v16 = rd16(REG_SYS_FUNC);                 // loader reset
    if (!(v16 & BIT(12))) { v16 |= BIT(12); wr16(REG_SYS_FUNC, v16); }

    v16 = rd16(REG_SYS_CLKR);                 // loader clock + ANA 8M
    if (!(v16 & BIT(5)) || !(v16 & BIT(1))) {
        v16 |= (BIT(5) | BIT(1));
        wr16(REG_SYS_CLKR, v16);
    }

    memset(fEfuse, 0xff, sizeof(fEfuse));

    while (addr < EFUSE_MAP_LEN) {
        ret = readEfuse8(addr++, &header);
        if (ret != kIOReturnSuccess || header == 0xff) goto exit;

        if ((header & 0x1f) == 0x0f) {        // extended header
            offset = (header & 0xe0) >> 5;
            ret = readEfuse8(addr++, &ext);
            if (ret != kIOReturnSuccess) goto exit;
            if ((ext & 0x0f) == 0x0f) continue;   // all words disabled
            offset |= ((ext & 0xf0) >> 1);
            wordMask = ext & 0x0f;
        } else {
            offset = (header >> 4) & 0x0f;
            wordMask = header & 0x0f;
        }

        mapAddr = offset * 8;
        for (i = 0; i < 4; i++) {
            if (wordMask & BIT(i)) { mapAddr += 2; continue; }

            ret = readEfuse8(addr++, &val8);
            if (ret != kIOReturnSuccess) goto exit;
            if (mapAddr >= EFUSE_MAP_LEN - 1) { ret = kIOReturnBadArgument; goto exit; }
            fEfuse[mapAddr++] = val8;

            ret = readEfuse8(addr++, &val8);
            if (ret != kIOReturnSuccess) goto exit;
            fEfuse[mapAddr++] = val8;
        }
    }

exit:
    wr8(REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);
    return ret;
}

// ---------- lifecycle ----------

bool RtlWifiDriver::start(IOService *provider) {
    fDevice = nullptr;
    if (!super::start(provider)) return false;
    setProperty("start-called", kOSBooleanTrue);
    setProperty("build-tag", "v5");
    fDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!fDevice) { IOLog("RtlWifi: provider is not IOUSBHostDevice\n"); return false; }
    if (!fDevice->open(this)) { IOLog("RtlWifi: open failed\n"); fDevice = nullptr; return false; }

    setProperty("sys-cfg", (uint64_t)rd32(0x00F0), 32);

    IOReturn r = powerOn();
    setProperty("power-on-status", (uint64_t)(uint32_t)r, 32);
    setProperty("fsmco-after-power-on", (uint64_t)rd32(REG_APS_FSMCO), 32);
    if (r != kIOReturnSuccess) return true;   // stay attached so properties can be read

    wr16(REG_TRXFF_BNDY + 2, 0x3f7f);         // rtl8188fu_fops.trxff_boundary

    r = downloadFirmware();
    setProperty("fw-dl-status", (uint64_t)(uint32_t)r, 32);
    setProperty("mcu-fw-dl-reg-after-dl", (uint64_t)rd32(REG_MCU_FW_DL), 32);
    if (r != kIOReturnSuccess) return true;

    r = startFirmware();
    setProperty("fw-start-status", (uint64_t)(uint32_t)r, 32);
    setProperty("mcu-fw-dl-reg-final", (uint64_t)rd32(REG_MCU_FW_DL), 32);

    r = readEfuse();
    setProperty("efuse-status", (uint64_t)(uint32_t)r, 32);
    setProperty("efuse-map", fEfuse, sizeof(fEfuse));
    uint16_t rtlId = (uint16_t)(fEfuse[0] | (fEfuse[1] << 8));
    setProperty("efuse-rtl-id", (uint64_t)rtlId, 16);      // expected 0x8129 = 33065
    char mac[20];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             fEfuse[0xd7], fEfuse[0xd8], fEfuse[0xd9],
             fEfuse[0xda], fEfuse[0xdb], fEfuse[0xdc]);
    setProperty("mac-address-str", mac);
    return true;
}

void RtlWifiDriver::stop(IOService *provider) {
    if (fDevice) { fDevice->close(this); fDevice = nullptr; }
    IOLog("RtlWifi: stopped\n");
    super::stop(provider);
}
