#include "RtlWifiDriver.hpp"
#include "rtl8188fufw.h"          // generated: rtl8188fufw_bin[], rtl8188fufw_bin_len
#include "RtlWifiTables.h"         // generated: kRtl8188fMacTable[]
#include "RtlWifiCrypto.h"
#if defined(__has_include) && __has_include("RtlWifiSecrets.h")
#include "RtlWifiSecrets.h"
#endif
#include "RtlWifiPhyTables.h"      // generated: PHY, AGC and RF tables
#ifndef RTL_TEST_SSID                 // may be defined in RtlWifiSecrets.h; the default is the original test router
#define RTL_TEST_SSID ""
#endif
#ifndef RTL_TEST_PASSPHRASE
#define RTL_TEST_PASSPHRASE "CHANGE_ME"
#endif
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <sys/random.h>

#define super IOEthernetController

static int findIe(const uint8_t *ies, uint16_t len, uint8_t tag, const uint8_t **data);
OSDefineMetaClassAndStructors(RtlWifiDriver, IOEthernetController)

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
#define REG_TRXDMA_CTRL   0x010C
#define REG_RF_CTRL       0x001F
#define REG_PBP           0x0104
#define REG_TXDMA_OFFSET_CHK 0x020C
#define REG_AUTO_LLT      0x0224
#define REG_FPGA0_XA_HSSI_PARM2 0x0824
#define REG_FPGA0_XA_LSSI_PARM  0x0840
#define REG_FPGA0_XA_RF_INT_OE  0x0860
#define REG_FPGA0_XAB_RF_SW_CTRL 0x0870
#define REG_RQPN          0x0200
#define REG_RQPN_NPQ      0x0214

#define TX_TOTAL_PAGE_NUM    0xF7   // TX_TOTAL_PAGE_NUM_8188F
#define TX_PAGE_NUM_HI_PQ    0x0C
#define TX_PAGE_NUM_LO_PQ    0x02
#define TX_PAGE_NUM_NORM_PQ  0x02
#define TRXDMA_QUEUE_LOW     1
#define TRXDMA_QUEUE_NORMAL  2
#define TRXDMA_QUEUE_HIGH    3
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
    return fInterface->deviceRequest(req, buf, got, 1000);
}

IOReturn RtlWifiDriver::writeReg(uint16_t reg, const void *buf, uint16_t len) {
    StandardUSB::DeviceRequest req;
    req.bmRequestType = 0x40;   // host-to-device, vendor, device
    req.bRequest = 0x05;
    req.wValue = reg;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    return fInterface->deviceRequest(req, const_cast<void *>(buf), got, 1000);
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

// ---------- queue setup and MAC init ----------

// rtl8xxxu_config_endpoints_no_sie (used by the 8188F)
void RtlWifiDriver::configEndpoints(uint8_t nrOutEps) {
    fEpTxHigh = fEpTxNormal = fEpTxLow = fEpTxCount = 0;
    if (nrOutEps >= 3) { fEpTxLow = 1; fEpTxCount++; }
    if (nrOutEps >= 2) { fEpTxNormal = 1; fEpTxCount++; }
    if (nrOutEps >= 1) { fEpTxHigh = 1; fEpTxCount++; }
}

void RtlWifiDriver::initQueueReservedPage() {
    uint32_t hq = 0, lq = 0, nq = 0, eq = 0, pubq;
    if (fEpTxHigh)   hq = TX_PAGE_NUM_HI_PQ;
    if (fEpTxLow)    lq = TX_PAGE_NUM_LO_PQ;
    if (fEpTxNormal) nq = TX_PAGE_NUM_NORM_PQ;

    wr32(REG_RQPN_NPQ, (nq << 0) | (eq << 16));

    pubq = TX_TOTAL_PAGE_NUM - hq - lq - nq - 1;
    uint32_t v = BIT(31) | (hq << 0) | (lq << 8) | (pubq << 16);   // RQPN_LOAD
    wr32(REG_RQPN, v);
}

IOReturn RtlWifiDriver::initQueuePriority() {
    uint16_t hi = 0, lo = 0;
    uint16_t hiq, mgq, bkq, beq, viq, voq;

    switch (fEpTxCount) {
    case 1:
        if (fEpTxHigh) hi = TRXDMA_QUEUE_HIGH;
        else if (fEpTxLow) hi = TRXDMA_QUEUE_LOW;
        else if (fEpTxNormal) hi = TRXDMA_QUEUE_NORMAL;
        else return kIOReturnBadArgument;
        hiq = mgq = bkq = beq = viq = voq = hi;
        break;
    case 2:
        if (fEpTxHigh && fEpTxLow)         { hi = TRXDMA_QUEUE_HIGH;   lo = TRXDMA_QUEUE_LOW; }
        else if (fEpTxNormal && fEpTxLow)  { hi = TRXDMA_QUEUE_NORMAL; lo = TRXDMA_QUEUE_LOW; }
        else if (fEpTxHigh && fEpTxNormal) { hi = TRXDMA_QUEUE_HIGH;   lo = TRXDMA_QUEUE_NORMAL; }
        else return kIOReturnBadArgument;
        hiq = mgq = viq = voq = hi;
        bkq = beq = lo;
        break;
    case 3:
        beq = bkq = TRXDMA_QUEUE_LOW;
        viq = TRXDMA_QUEUE_NORMAL;
        voq = mgq = hiq = TRXDMA_QUEUE_HIGH;
        break;
    default:
        return kIOReturnBadArgument;
    }

    uint16_t v = rd16(REG_TRXDMA_CTRL);
    v &= 0x7;
    v |= (voq << 4) | (viq << 6) | (beq << 8) | (bkq << 10) | (mgq << 12) | (hiq << 14);
    wr16(REG_TRXDMA_CTRL, v);
    return kIOReturnSuccess;
}

// rtl8xxxu_init_mac: write the 8188F MAC table (8-bit writes); the 8188F has no extra tail writes
IOReturn RtlWifiDriver::initMac() {
    uint32_t writeErrors = 0, mismatches = 0;
    char first[32] = "none";

    for (unsigned int i = 0; i < kRtl8188fMacTableLen; i++) {
        uint8_t v = kRtl8188fMacTable[i].val;
        if (writeReg(kRtl8188fMacTable[i].reg, &v, 1) != kIOReturnSuccess) writeErrors++;
    }
    for (unsigned int i = 0; i < kRtl8188fMacTableLen; i++) {      // informational read-back
        uint8_t got = rd8(kRtl8188fMacTable[i].reg);
        if (got != kRtl8188fMacTable[i].val) {
            if (!mismatches)
                snprintf(first, sizeof(first), "%04x got %02x want %02x",
                         kRtl8188fMacTable[i].reg, got, kRtl8188fMacTable[i].val);
            mismatches++;
        }
    }
    setProperty("mac-table-count", (uint64_t)kRtl8188fMacTableLen, 32);
    setProperty("mac-table-write-errors", (uint64_t)writeErrors, 32);
    setProperty("mac-table-readback-mismatch", (uint64_t)mismatches, 32);
    setProperty("mac-table-first-mismatch", first);
    return writeErrors ? kIOReturnIOError : kIOReturnSuccess;
}

// ---------- PHY / RF init ----------

// rtl8xxxu_write_rfreg for RF path A
IOReturn RtlWifiDriver::writeRfReg(uint8_t reg, uint32_t data) {
    data &= 0x000fffff;                               // FPGA0_LSSI_PARM_DATA_MASK
    uint32_t dataaddr = ((uint32_t)reg << 20) | data; // FPGA0_LSSI_PARM_ADDR_SHIFT
    IOReturn r = writeReg(REG_FPGA0_XA_LSSI_PARM, &dataaddr, 4);
    IODelay(1);
    return r;
}

// rtl8xxxu_init_phy_regs: returns number of failed writes
uint32_t RtlWifiDriver::initPhyRegs(const RtlReg32Val *tbl, unsigned int len) {
    uint32_t errors = 0;
    for (unsigned int i = 0; i < len; i++) {
        uint32_t v = tbl[i].val;
        if (writeReg(tbl[i].reg, &v, 4) != kIOReturnSuccess) errors++;
        IODelay(1);
    }
    return errors;
}

// rtl8188fu_init_phy_bb
IOReturn RtlWifiDriver::initPhyBb() {
    uint16_t v16 = rd16(REG_SYS_FUNC);
    v16 |= BIT(1) | BIT(0) | BIT(13);          // BB_GLB_RSTN | BBRSTB | DIO_RF
    wr16(REG_SYS_FUNC, v16);

    wr8(REG_RF_CTRL, BIT(0) | BIT(1) | BIT(2)); // RF_ENABLE | RF_RSTB | RF_SDMRSTB
    IODelay(20);

    writeRfReg(0x01, 0x780);                   // RF6052_REG_IQADJ_G1

    wr8(REG_SYS_FUNC, BIT(1) | BIT(0) | BIT(2) | BIT(4)); // BB_GLB_RSTN | BBRSTB | USBA | USBD

    uint32_t e1 = initPhyRegs(kRtl8188fPhyTable, kRtl8188fPhyTableLen);
    uint32_t e2 = initPhyRegs(kRtl8188fAgcTable, kRtl8188fAgcTableLen);
    setProperty("phy-table-write-errors", (uint64_t)e1, 32);
    setProperty("agc-table-write-errors", (uint64_t)e2, 32);
    setProperty("bb-0x800-readback", (uint64_t)rd32(0x0800), 32);
    return (e1 || e2) ? kIOReturnIOError : kIOReturnSuccess;
}

// rtl8188fu_init_phy_rf -> rtl8xxxu_init_phy_rf (RF path A)
IOReturn RtlWifiDriver::initPhyRf() {
    uint16_t rfsiRfenv = rd16(REG_FPGA0_XAB_RF_SW_CTRL) & BIT(4);   // FPGA0_RF_RFENV

    uint32_t v = rd32(REG_FPGA0_XA_RF_INT_OE);
    v |= BIT(20);
    wr32(REG_FPGA0_XA_RF_INT_OE, v);
    IODelay(1);
    v = rd32(REG_FPGA0_XA_RF_INT_OE);
    v |= BIT(4);
    wr32(REG_FPGA0_XA_RF_INT_OE, v);
    IODelay(1);

    v = rd32(REG_FPGA0_XA_HSSI_PARM2);
    v &= ~0x400u;                              // FPGA0_HSSI_3WIRE_ADDR_LEN
    wr32(REG_FPGA0_XA_HSSI_PARM2, v);
    IODelay(1);
    v = rd32(REG_FPGA0_XA_HSSI_PARM2);
    v &= ~0x800u;                              // FPGA0_HSSI_3WIRE_DATA_LEN
    wr32(REG_FPGA0_XA_HSSI_PARM2, v);
    IODelay(1);

    const RtlRfRegVal *tbl = (fChipCut == 1) ? kRtl8188fRadioCutB : kRtl8188fRadioA;
    unsigned int len = (fChipCut == 1) ? kRtl8188fRadioCutBLen : kRtl8188fRadioALen;
    uint32_t errors = 0;

    for (unsigned int i = 0; i < len; i++) {
        uint8_t reg = tbl[i].reg;
        switch (reg) {
        case 0xfe: IOSleep(50); continue;
        case 0xfd: IOSleep(5);  continue;
        case 0xfc: IOSleep(1);  continue;
        case 0xfb: IODelay(50); continue;
        case 0xfa: IODelay(5);  continue;
        case 0xf9: IODelay(1);  continue;
        }
        if (writeRfReg(reg, tbl[i].val) != kIOReturnSuccess) errors++;
        IODelay(1);
    }

    uint16_t v16 = rd16(REG_FPGA0_XAB_RF_SW_CTRL);
    v16 &= ~BIT(4);
    v16 |= rfsiRfenv;
    wr16(REG_FPGA0_XAB_RF_SW_CTRL, v16);

    setProperty("rf-table-cut", (uint64_t)fChipCut, 8);
    setProperty("rf-table-write-errors", (uint64_t)errors, 32);
    return errors ? kIOReturnIOError : kIOReturnSuccess;
}

// rest of rtl8xxxu_init_device after init_phy_rf, up to usb_quirks (the !macpower branch for the 8188F)
IOReturn RtlWifiDriver::postRfInit() {
    wr32(0x0804, 0x00000003);                  // REG_FPGA0_TX_INFO

    uint32_t v = BIT(5) | BIT(6) | BIT(8) | BIT(9) | ((BIT(8) | BIT(9)) << 16);
    v |= BIT(10) | (BIT(10) << 16);            // PAPE (priv->no_pape assumed 0)
    wr32(REG_FPGA0_XAB_RF_SW_CTRL, v);

    wr32(REG_FPGA0_XA_RF_INT_OE, 0x66f60210);

    uint8_t b = TX_TOTAL_PAGE_NUM + 1;         // TX buffer boundary
    wr8(0x0424, b);                            // REG_TXPKTBUF_BCNQ_BDNY
    wr8(0x0425, b);                            // REG_TXPKTBUF_MGQ_BDNY
    wr8(0x045d, b);                            // REG_TXPKTBUF_WMAC_LBK_BF_HD
    wr8(REG_TRXFF_BNDY, b);
    wr8(0x0209, b);                            // REG_TDECTRL + 1

    wr8(REG_PBP, (2 << 0) | (2 << 4));         // PBP_PAGE_SIZE_256 for RX and TX
    setProperty("pbp-after-write", (uint64_t)rd8(REG_PBP), 8);

    // rtl8xxxu_auto_llt_table
    uint32_t v32 = rd32(REG_AUTO_LLT);
    v32 |= BIT(16);                            // AUTO_LLT_INIT_LLT
    wr32(REG_AUTO_LLT, v32);
    int i;
    for (i = 500; i; i--) {
        v32 = rd32(REG_AUTO_LLT);
        if (!(v32 & BIT(16))) break;
        IODelay(3);
    }
    IOReturn llt = i ? kIOReturnSuccess : kIOReturnTimeout;
    setProperty("pbp-after-llt", (uint64_t)rd8(REG_PBP), 8);
    setProperty("auto-llt-status", (uint64_t)(uint32_t)llt, 32);

    // rtl8188f_usb_quirks
    uint16_t cr = rd16(REG_CR);
    cr |= BIT(6) | BIT(7);                     // CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE
    wr16(REG_CR, cr);
    v32 = rd32(REG_TXDMA_OFFSET_CHK);
    v32 |= BIT(9);                             // TXDMA_OFFSET_DROP_DATA_EN
    wr32(REG_TXDMA_OFFSET_CHK, v32);

    setProperty("fpga0-tx-info-readback", (uint64_t)rd32(0x0804), 32);
    setProperty("txpktbuf-bcnq-bdny-readback", (uint64_t)rd8(0x0424), 8);
    setProperty("pbp-readback", (uint64_t)rd8(REG_PBP), 8);
    setProperty("cr-readback", (uint64_t)rd16(REG_CR), 16);
    setProperty("txdma-offset-chk-readback", (uint64_t)rd32(REG_TXDMA_OFFSET_CHK), 32);
    return llt;
}

// rest of rtl8xxxu_init_device after init_phy_rf/usb_quirks for the 8188F, up to (not including)
// calibration: TX report, RX/interrupt/WMAC setup, rate/SIFS/retry/EDCA, beacon, lifetimes, CCK/OFDM, CAM
IOReturn RtlWifiDriver::initMacTail() {
    uint32_t errs = 0;
#define W(reg, len, val) do { uint32_t _v = (uint32_t)(val); \
        if (writeReg((reg), &_v, (len)) != kIOReturnSuccess) errs++; } while (0)

    // TX report (has_tx_report)
    uint8_t v8 = rd8(0x04ec);
    v8 |= BIT(1);                              // TX_REPORT_CTRL_TIMER_ENABLE
    W(0x04ec, 1, v8);
    W(0x04ed, 1, 0x02);                        // max RPT MACID
    W(0x04f0, 2, 0xcdf0);                      // TX report timer, unit 32us
    v8 = rd8(0x00a3) & 0xf8;
    W(0x00a3, 1, v8);

    W(0x060f, 1, 4);                           // REG_RX_DRVINFO_SZ
    W(0x00b4, 4, 0xffffffff);                  // REG_HISR0
    W(0x00bc, 4, 0xffffffff);                  // REG_HISR1

    // WMAC: RCR and RX filter maps
    uint32_t rcr = BIT(1) | BIT(2) | BIT(3) | BIT(13) | BIT(14) | BIT(28) | BIT(29) | BIT(30);
    W(0x0608, 4, rcr);
    W(0x06a4, 2, 0xffff);                      // RXFLTMAP2: all data frames
    W(0x06a2, 2, 0x0400);                      // RXFLTMAP1
    W(0x06a0, 2, 0xffff);                      // RXFLTMAP0: all management frames

    // adaptive controls
    uint32_t v32 = rd32(0x0440);               // REG_RESPONSE_RATE_SET
    v32 &= ~0xfffffu;
    v32 |= 0xffff1;
    W(0x0440, 4, v32);
    W(0x0428, 2, (0x10 << 0) | (0x10 << 8));   // spec SIFS (cck 0x10, ofdm 0x10)
    W(0x042a, 2, (0x30 << 8) | 0x30);          // retry limit short/long = 0x30
    W(0x0428, 2, (0x0a << 0) | (0x10 << 8));   // spec SIFS (cck 0x0a, ofdm 0x10)

    // EDCA
    W(0x063a, 2, 0x100a);                      // MAC_SPEC_SIFS
    W(0x0514, 2, 0x100a);                      // SIFS_CCK
    W(0x0516, 2, 0x100a);                      // SIFS_OFDM
    W(0x0508, 4, 0x005ea42b);                  // EDCA BE
    W(0x050c, 4, 0x0000a44f);                  // EDCA BK
    W(0x0504, 4, 0x005ea324);                  // EDCA VI
    W(0x0500, 4, 0x002fa226);                  // EDCA VO

    // data auto rate fallback retry counts
    W(0x0430, 4, 0x00000000);
    W(0x0434, 4, 0x10080404);
    W(0x0438, 4, 0x04030201);
    W(0x043c, 4, 0x08070605);

    v8 = rd8(0x0420);                          // REG_FWHW_TXQ_CTRL
    v8 |= BIT(7);                              // AMPDU_RETRY
    W(0x0420, 1, v8);
    W(0x0640, 1, 0x40);                        // ACK timeout

    // beacon parameters
    W(0x0550, 2, BIT(4) | (BIT(4) << 8));      // BEACON_CTRL: disable TSF update
    W(0x0540, 2, 0x6404);                      // TBTT_PROHIBIT
    W(0x0559, 1, 2);                           // BEACON_DMA_TIME
    W(0x0510, 2, 0x660F);                      // BEACON_TCFG

    // packet lifetime (init_reg_pkt_life_time)
    W(0x04c0, 2, 0x0400);
    W(0x04c2, 2, 0x0400);

    // CCK + OFDM blocks, CAM invalidate
    v32 = rd32(0x0800);                        // REG_FPGA0_RF_MODE
    v32 |= BIT(24) | BIT(25);
    W(0x0800, 4, v32);
    W(0x0670, 4, BIT(31) | BIT(30));           // CAM_CMD_POLLING | undocumented bit 30

    W(0x0423, 1, 0xff);                        // HWSEQ_CTRL
    W(0x04cc, 4, 0x0201ffff);                  // BAR_MODE_CTRL

    v8 = rd8(0x0040);                          // GPIO_MUXCFG (8188F/8188E/8192F branch)
    v8 &= ~BIT(5);
    W(0x0040, 1, v8);
    W(0x0a0a, 1, 0x83);                        // CCK PD threshold (8188F)

    W(0x0652, 1, (30000 + 128 - 1) / 128);     // NAV_UPPER = 30000us, unit 128us

    v32 = rd32(0x0420);                        // ack for xmit mgmt frames
    v32 |= BIT(12);
    W(0x0420, 4, v32);

#undef W
    setProperty("mac-tail-write-errors", (uint64_t)errs, 32);
    setProperty("rcr-readback", (uint64_t)rd32(0x0608), 32);
    setProperty("rxfltmap0-readback", (uint64_t)rd16(0x06a0), 16);
    setProperty("resp-rate-readback", (uint64_t)rd32(0x0440), 32);
    setProperty("spec-sifs-readback", (uint64_t)rd16(0x0428), 16);
    setProperty("retry-limit-readback", (uint64_t)rd16(0x042a), 16);
    setProperty("edca-be-readback", (uint64_t)rd32(0x0508), 32);
    setProperty("rf-mode-readback", (uint64_t)rd32(0x0800), 32);
    setProperty("nav-upper-readback", (uint64_t)rd8(0x0652), 8);
    setProperty("cck-pd-readback", (uint64_t)rd8(0x0a0a), 8);
    setProperty("txrpt-time-readback", (uint64_t)rd16(0x04f0), 16);
    setProperty("beacon-tcfg-readback", (uint64_t)rd16(0x0510), 16);
    setProperty("bar-mode-readback", (uint64_t)rd32(0x04cc), 32);
    return errs ? kIOReturnIOError : kIOReturnSuccess;
}

// rtl8xxxu_read_rfreg for RF path A
uint32_t RtlWifiDriver::readRfReg(uint8_t reg) {
    uint32_t hssia = rd32(REG_FPGA0_XA_HSSI_PARM2);
    uint32_t v = hssia;
    v &= ~0x7f800000u;                          // FPGA0_HSSI_PARM2_ADDR_MASK
    v |= ((uint32_t)reg << 23);                 // ADDR_SHIFT
    v |= BIT(31);                               // EDGE_READ
    hssia &= ~BIT(31);
    wr32(REG_FPGA0_XA_HSSI_PARM2, hssia);
    IODelay(10);
    wr32(REG_FPGA0_XA_HSSI_PARM2, v);
    IODelay(100);
    hssia |= BIT(31);
    wr32(REG_FPGA0_XA_HSSI_PARM2, hssia);
    IODelay(10);
    uint32_t p = rd32(0x0820);                  // REG_FPGA0_XA_HSSI_PARM1
    uint32_t ret = (p & BIT(8)) ? rd32(0x08b8)  // PI mode: HSPI readback
                                : rd32(0x08a0); // LSSI readback
    return ret & 0xfffff;
}

// Remaining init_device steps for the 8188F: TX power, LC calibration, thermal meter,
// burst/aggregation, multicast filter, MAC address. IQ calibration is not included.
IOReturn RtlWifiDriver::initCalib() {
    uint32_t errs = 0;
#define W(reg, len, val) do { uint32_t _v = (uint32_t)(val); \
        if (writeReg((reg), &_v, (len)) != kIOReturnSuccess) errs++; } while (0)

    setTxPower(1);                                     // channel 1, 20 MHz
    uint32_t v32;

    // rtl8188f_phy_lc_calibrate
    uint32_t lstf = rd32(0x0d00);                      // REG_OFDM1_LSTF
    if (lstf & 0x70000000u) W(0x0d00, 4, lstf & ~0x70000000u);
    else W(0x0522, 1, 0xff);                           // REG_TXPAUSE: block all queues
    uint32_t rfAmode = readRfReg(0x18);                // RF6052_REG_MODE_AG
    writeRfReg(0x18, rfAmode | 0x08000);
    int i;
    for (i = 0; i < 100; i++) {
        if ((readRfReg(0x18) & 0x08000) == 0) break;
        IOSleep(10);
    }
    writeRfReg(0x18, rfAmode);
    if (lstf & 0x70000000u) W(0x0d00, 4, lstf);
    else W(0x0522, 1, 0x00);
    setProperty("lc-cal-loops", (uint64_t)i, 32);      // 100 = timed out
    setProperty("rf-0x18-before-cal", (uint64_t)rfAmode, 32);

    // thermal meter (gen2, 8188F)
    v32 = readRfReg(0x42);                             // RF6052_REG_T_METER_8723B
    v32 |= 0x30000;
    writeRfReg(0x42, v32);

    // rtl8xxxu_init_burst
    uint8_t v8 = rd8(0x0290);                          // REG_RXDMA_PRO_8723B
    v8 = (uint8_t)((v8 & ~0x3c) | (1 << 4) | (3 << 2) | BIT(1));
    W(0x0290, 1, v8);
    v8 = rd8(0x04c7);                                  // REG_HT_SINGLE_AMPDU_8723B
    v8 |= BIT(7);
    W(0x04c7, 1, v8);
    W(0x04ca, 2, 0x0c14);                              // REG_MAX_AGGR_NUM
    W(0x0456, 1, 0x70);                                // REG_AMPDU_MAX_TIME_8723B
    W(0x0458, 4, 0xffffffff);                          // REG_AGGLEN_LMT
    W(0x060c, 1, 0x18);                                // REG_RX_PKT_LIMIT
    W(0x0512, 1, 0x00);                                // REG_PIFS
    W(0x0420, 1, BIT(7));                              // FWHW_TXQ_CTRL_AMPDU_RETRY
    W(0x0460, 4, 0x03086666);                          // REG_FAST_EDCA_CTRL
    W(0x055c, 1, 0x28);                                // REG_USTIME_TSF_8723B
    W(0x0638, 1, 0x28);                                // REG_USTIME_EDCA
    v8 = rd8(REG_RSV_CTRL);
    v8 |= BIT(5) | BIT(6);                             // WLOCK_1C | DIS_PRST
    W(REG_RSV_CTRL, 1, v8);

    // rtl8188fu_init_aggregation
    v32 = rd32(0x0208);                                // REG_DWBCN0_CTRL_8188F
    v32 &= ~(0xfu << 4);
    v32 |= (6u << 4);
    W(0x0208, 4, v32);
    W(0x0228, 1, 6 << 1);                              // REG_DWBCN1_CTRL_8723B
    // V27: Enable USB RXDMA aggregation (8 pages / timeout 4) for multi-packet bulk-IN bursts
    uint8_t aggCtrl = rd8(REG_TRXDMA_CTRL);
    aggCtrl |= BIT(2);                                 // TRXDMA_CTRL_RXDMA_AGG_EN
    uint32_t aggRx = rd32(0x0280);                     // REG_RXDMA_AGG_PG_TH
    aggRx &= ~BIT(31);
    aggRx = (aggRx & ~0xFF0Fu) | 0x0408u;
    uint8_t rxdmaMode = rd8(0x0290);
    rxdmaMode |= BIT(1);
    W(REG_TRXDMA_CTRL, 1, aggCtrl);
    W(0x0280, 4, aggRx);
    W(0x0290, 1, rxdmaMode);

    // the reference sets XMIT_MGMT_ACK after the burst step; the write above cleared it
    v32 = rd32(0x0420);
    v32 |= BIT(12);
    W(0x0420, 4, v32);

    // multicast filter (init_reg_rxfltmap) and MAC address
    W(0x0620, 4, 0xffffffff);                          // REG_MAR
    W(0x0624, 4, 0xffffffff);
    for (int k = 0; k < 6; k++) W(0x0610 + k, 1, fEfuse[0xd7 + k]);   // REG_MACID

#undef W
    setProperty("calib-write-errors", (uint64_t)errs, 32);
    setProperty("tx-agc-ofdm-readback", (uint64_t)rd32(0x0e00), 32);
    setProperty("tx-agc-mcs-readback", (uint64_t)rd32(0x0e10), 32);
    setProperty("tx-agc-cck-readback", (uint64_t)rd32(0x0e08), 32);
    setProperty("rf-0x42-readback", (uint64_t)readRfReg(0x42), 32);
    setProperty("rf-0x01-readback", (uint64_t)readRfReg(0x01), 32);
    setProperty("rxdma-pro-readback", (uint64_t)rd8(0x0290), 8);
    setProperty("max-aggr-num-readback", (uint64_t)rd16(0x04ca), 16);
    setProperty("fwhw-txq-ctrl-readback", (uint64_t)rd32(0x0420), 32);
    setProperty("dwbcn0-ctrl-readback", (uint64_t)rd32(0x0208), 32);
    char mid[20];
    snprintf(mid, sizeof(mid), "%02x:%02x:%02x:%02x:%02x:%02x",
             rd8(0x0610), rd8(0x0611), rd8(0x0612), rd8(0x0613), rd8(0x0614), rd8(0x0615));
    setProperty("macid-readback-str", mid);
    return errs ? kIOReturnIOError : kIOReturnSuccess;
}

// rtl8188f_set_tx_power for an HT20 channel; efuse layout of the 8188FU
void RtlWifiDriver::setTxPower(uint8_t channel) {
    uint8_t cckIdx[6], ht40Idx[5];
    for (int i = 0; i < 6; i++) {
        cckIdx[i] = fEfuse[0x10 + i];
        if (cckIdx[i] > 0x3F) cckIdx[i] = 0x22;       // TX_POWER_INDEX_DEFAULT_CCK
    }
    for (int i = 0; i < 5; i++) {
        ht40Idx[i] = fEfuse[0x16 + i];
        if (ht40Idx[i] > 0x3F) ht40Idx[i] = 0x27;     // TX_POWER_INDEX_DEFAULT_HT40
    }
    int8_t ofdmDiff = (int8_t)(fEfuse[0x1b] & 0x0f);  // signed 4-bit fields
    if (ofdmDiff & 0x08) ofdmDiff |= 0xf0;
    int8_t ht20Diff = (int8_t)((fEfuse[0x1b] >> 4) & 0x0f);
    if (ht20Diff & 0x08) ht20Diff |= 0xf0;

    int group, cckGroup;                               // rtl8188f_channel_to_group
    if (channel < 3) group = 0;
    else if (channel < 6) group = 1;
    else if (channel < 9) group = 2;
    else if (channel < 12) group = 3;
    else group = 4;
    cckGroup = (channel == 14) ? 5 : group;

    uint8_t cck = cckIdx[cckGroup];
    uint32_t v32 = rd32(0x0e08);
    v32 &= 0xffff00ff;
    v32 |= ((uint32_t)cck << 8);
    wr32(0x0e08, v32);
    v32 = rd32(0x086c);
    v32 &= 0xff;
    v32 |= (((uint32_t)cck << 8) | ((uint32_t)cck << 16) | ((uint32_t)cck << 24));
    wr32(0x086c, v32);

    uint8_t ofdmBase = (uint8_t)(ht40Idx[group] + ofdmDiff);
    uint32_t ofdm = ofdmBase * 0x01010101u;
    wr32(0x0e00, ofdm);
    wr32(0x0e04, ofdm);
    uint8_t mcsBase = (uint8_t)(ht40Idx[group] + ht20Diff);
    uint32_t mcs = mcsBase * 0x01010101u;
    wr32(0x0e10, mcs);
    wr32(0x0e14, mcs);
    wr32(0x0e18, mcs);
    wr32(0x0e1c, mcs);
}

// rtl8188f_spur_calibration
void RtlWifiDriver::spurCalibration(uint8_t channel) {
    static const uint32_t freq[15] = {0,0,0,0,0,0xFCCD,0xFC4D,0xFFCD,0xFF4D,0,0,0xFDCD,0,0xFCCD,0xFF9A};
    static const uint32_t d40[15]  = {0,0,0,0,0,0x06000000,0x00000600,0,0,0,0,0,0,0x06000000,0};
    static const uint32_t d44[15]  = {0,0,0,0,0,0,0,0,0,0,0,0x04000000,0,0,0};
    static const uint32_t d4c[15]  = {0,0,0,0,0,0,0,0x06000000,0x00000380,0,0,0,0,0,0x00180000};
    const uint8_t threshold = 0x16;
    bool doNotch, hwCtrl, swCtrl, hwCtrlS1 = false, swCtrlS1 = false;
    uint32_t v32, initialGain, reg948;

    v32 = rd32(0x0c40);                                // REG_OFDM0_RX_D_SYNC_PATH
    v32 |= 0x1f000000u;                                // GENMASK(28, 24)
    wr32(0x0c40, v32);
    v32 = rd32(0x0c40);                                // enable notch filter
    v32 |= BIT(9);
    wr32(0x0c40, v32);

    if (channel <= 14 && freq[channel] > 0) {
        reg948 = rd32(0x0948);                         // REG_S0S1_PATH_SWITCH
        hwCtrl = reg948 & BIT(6);
        swCtrl = !hwCtrl;
        if (hwCtrl) {
            v32 = rd32(0x0864);                        // REG_FPGA0_XB_RF_INT_OE
            v32 &= 0x38;                               // GENMASK(5, 3)
            hwCtrlS1 = (v32 == BIT(3));
        } else if (swCtrl) {
            swCtrlS1 = !(reg948 & BIT(9));
        }
        if (hwCtrlS1 || swCtrlS1) {
            initialGain = rd32(0x0c50);                // REG_OFDM0_XA_AGC_CORE1
            v32 = rd32(0x0800);                        // disable CCK block
            v32 &= ~BIT(24);
            wr32(0x0800, v32);
            v32 = initialGain & ~0x7fu;                // OFDM0_X_AGC_CORE1_IGI_MASK
            v32 |= 0x30;
            wr32(0x0c50, v32);
            wr32(0x088c, 0xccf000c0);                  // disable 3-wire
            wr32(0x0808, freq[channel]);               // setup PSD
            wr32(0x0808, 0x400000 | freq[channel]);    // start PSD
            IOSleep(30);
            doNotch = rd32(0x08b4) >= threshold;       // REG_FPGA0_PSD_REPORT
            wr32(0x0808, freq[channel]);               // turn off PSD
            wr32(0x088c, 0xccc000c0);                  // enable 3-wire
            v32 = rd32(0x0800);                        // enable CCK block
            v32 |= BIT(24);
            wr32(0x0800, v32);
            wr32(0x0c50, initialGain);
            if (doNotch) {
                wr32(0x0d40, d40[channel]);
                wr32(0x0d44, d44[channel]);
                wr32(0x0d48, 0x0);
                wr32(0x0d4c, d4c[channel]);
                v32 = rd32(0x0d2c);                    // enable CSI mask
                v32 |= BIT(28);
                wr32(0x0d2c, v32);
                setProperty("spur-notch", kOSBooleanTrue);
                return;
            }
        }
    }
    v32 = rd32(0x0d2c);                                // disable CSI mask function
    v32 &= ~BIT(28);
    wr32(0x0d2c, v32);
    setProperty("spur-notch", kOSBooleanFalse);
}

// rtl8188fu_config_channel for a 20 MHz channel, followed by the TX power update
IOReturn RtlWifiDriver::setChannel20(uint8_t channel) {
    if (channel < 1 || channel > 14) return kIOReturnBadArgument;
    uint32_t v32 = readRfReg(0x18);                    // RF6052_REG_MODE_AG
    v32 &= ~0x3ffu;                                    // MODE_AG_CHANNEL_MASK
    v32 |= channel;
    writeRfReg(0x18, v32);

    spurCalibration(channel);

    v32 = rd32(0x0800);                                // bandwidth mode: 20 MHz
    v32 &= ~BIT(0);
    wr32(0x0800, v32);
    v32 = rd32(0x0900);                                // REG_FPGA1_RF_MODE
    v32 &= ~BIT(0);
    wr32(0x0900, v32);

    v32 = rd32(0x0800);                                // RXADC CLK
    v32 |= 0x700;                                      // GENMASK(10, 8)
    wr32(0x0800, v32);
    v32 = rd32(0x0800);                                // TXDAC CLK
    v32 |= BIT(14) | BIT(12);
    v32 &= ~BIT(13);
    wr32(0x0800, v32);

    v32 = rd32(0x0ce4);                                // REG_OFDM0_TX_PSDO_NOISE_WEIGHT: small BW
    v32 &= ~0xc0000000u;
    wr32(0x0ce4, v32);
    v32 = rd32(0x0ce4);                                // adc buffer clk
    v32 &= ~BIT(29);
    v32 |= BIT(28);
    wr32(0x0ce4, v32);
    v32 = rd32(0x0c10);                                // REG_OFDM0_XA_RX_AFE
    v32 &= ~BIT(29);
    v32 |= BIT(28);
    wr32(0x0c10, v32);
    v32 = rd32(0x0954);                                // REG_OFDM_RX_DFIR
    v32 &= ~BIT(19);
    wr32(0x0954, v32);
    v32 = rd32(0x0954);
    v32 &= ~0x00f00000u;                               // GENMASK(23, 20)
    v32 |= BIT(21);
    v32 |= BIT(20);                                    // 20 MHz
    wr32(0x0954, v32);

    v32 = channel | 0xc00;                             // RF TRX_BW: channel | MODE_AG_BW_20MHZ_8723B
    writeRfReg(0x18, v32);
    writeRfReg(0x87, 0x00065);                         // RXG_MIX_SWBW: filter BW for 20 MHz
    writeRfReg(0x1c, 0x0);                             // RX_BB2
    writeRfReg(0xdf, 0x00140);                         // GAIN_CCA: RC corner
    writeRfReg(0x1b, 0x01c6c);                         // RX_G2

    setTxPower(channel);
    setProperty("channel-set", (uint64_t)channel, 8);
    setProperty("rf-0x18-after-channel", (uint64_t)readRfReg(0x18), 32);
    setProperty("tx-agc-ofdm-after-channel", (uint64_t)rd32(0x0e00), 32);
    setProperty("rf-mode-after-channel", (uint64_t)rd32(0x0800), 32);
    return kIOReturnSuccess;
}

// rtl8188fu_init_statistics
void RtlWifiDriver::initStatistics() {
    wr16(0x0894 + 2, 0xc350);                          // REG_NHM_TIMER_8723B + 2
    wr16(0x0890 + 2, 0xffff);                          // REG_NHM_TH9_TH10_8723B + 2
    wr32(0x0898, 0xffffff50);                          // REG_NHM_TH3_TO_TH0_8723B
    wr32(0x089c, 0xffffffff);                          // REG_NHM_TH7_TO_TH4_8723B
    uint32_t v32 = rd32(0x0e28);                       // REG_FPGA0_IQK, TH8
    v32 |= 0xff;
    wr32(0x0e28, v32);
    v32 = rd32(0x0890);                                // enable CCK
    v32 &= ~(BIT(8) | BIT(9) | BIT(10));
    v32 |= BIT(8);
    wr32(0x0890, v32);
    v32 = rd32(0x0c0c);                                // max power amongst all RX antennas
    v32 |= BIT(7);
    wr32(0x0c0c, v32);
    setProperty("nhm-th3-th0-readback", (uint64_t)rd32(0x0898), 32);
    setProperty("fa-rstc-readback", (uint64_t)rd32(0x0c0c), 32);
}

// First receive test: synchronous bulk-IN reads on the current channel. Decodes the 24-byte RX descriptor
// (rtl8xxxu_rxdesc24: dword0 = pktlen[13:0], crc32[14], icverr[15], drvinfo_sz[19:16], shift[25:24])
// and reports counts, the first frame bytes and the SSID of the first beacon as properties.
static const uint8_t kDhcpXid[4] = {0x52, 0x54, 0x4c, 0x38};   // DHCP transaction id used by the test

void RtlWifiDriver::rxProbe(uint8_t channel, char tag) {
    if (!fPipeIn) return;
    const uint32_t kBuf = 16384;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kBuf, 1);
    if (!md) return;
    uint8_t *buf = (uint8_t *)md->getBytesNoCopy();
    uint32_t ok = 0, timeouts = 0, other = 0, beacons = 0, mgmt = 0, crcErr = 0;
    uint32_t lastErr = 0;
    bool haveFirst = false, haveSsid = false, haveProbeSsid = false;
    uint32_t probeResp = 0, authResp = 0, authStatus = 0xffff;
    uint32_t assocResp = 0, assocStatus = 0xffff, assocAid = 0, eapol = 0, msg3Ok = 0, msg3Bad = 0;
    uint32_t prot = 0, decSnap = 0, secFirst = 0xff, swdecFirst = 0xff;
    uint32_t ethArp = 0, ethIp4 = 0, ethIp6 = 0, ethOther = 0;
    uint32_t icvErr = 0, deauth = 0, deauthReason = 0xffff, dhcpReplies = 0, protMcs = 0xff, protIvLen = 0xff;
    char key[48];

    for (int n = 0; n < (int)fRxLoops; n++) {
        uint32_t got = 0;
        IOReturn r = fPipeIn->io(md, kBuf, got, 150);
        if (r == kIOReturnTimeout) { timeouts++; continue; }
        if (r != kIOReturnSuccess) { other++; lastErr = (uint32_t)r; continue; }
        if (got < 24) { other++; continue; }
        ok++;

        uint32_t d0 = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        uint32_t pktlen = d0 & 0x3fff;
        bool crc = (d0 >> 14) & 1;
        uint32_t drvinfo = ((d0 >> 16) & 0xf) * 8;
        uint32_t shift = (d0 >> 24) & 3;
        uint32_t off = 24 + drvinfo + shift;
        if (crc) crcErr++;
        if ((d0 >> 15) & 1) icvErr++;

        if (!haveFirst) {
            haveFirst = true;
            snprintf(key, sizeof(key), "rx-ch%d%c-first-bytes", channel, tag);
            setProperty(key, buf, got < 64 ? got : 64);
            snprintf(key, sizeof(key), "rx-ch%d%c-first-pktlen", channel, tag);
            setProperty(key, (uint64_t)pktlen, 32);
            snprintf(key, sizeof(key), "rx-ch%d%c-first-total", channel, tag);
            setProperty(key, (uint64_t)got, 32);
            snprintf(key, sizeof(key), "rx-ch%d%c-first-drvinfo-shift", channel, tag);
            setProperty(key, (uint64_t)((drvinfo << 8) | shift), 32);
        }

        if (!crc && off + 36 <= got) {
            uint8_t fc0 = buf[off];
            if (((fc0 >> 2) & 3) == 0) mgmt++;
            if (fc0 == 0xc0 || fc0 == 0xa0) {                  // deauthentication / disassociation
                deauth++;
                if (deauthReason == 0xffff) deauthReason = (uint32_t)buf[off + 24] | ((uint32_t)buf[off + 25] << 8);
            }
            if (fc0 == 0x80) {                         // beacon
                beacons++;
                if (!fHaveAp && fTargetSsidLen > 0) {
                    uint32_t ie2 = off + 24 + 12;
                    if (ie2 + 2 <= got && buf[ie2] == 0 && buf[ie2 + 1] == fTargetSsidLen &&
                        ie2 + 2 + buf[ie2 + 1] <= got && memcmp(buf + ie2 + 2, fTargetSsid, fTargetSsidLen) == 0) {
                        memcpy(fApBssid, buf + off + 16, 6);
                        uint32_t end = off + pktlen;               // frame end without the 4-byte FCS
                        if (end > got) end = got;
                        if (end >= off + 40) end -= 4;
                        uint32_t n = (end > off + 36) ? (end - (off + 36)) : 0;
                        if (n > sizeof(fApIes)) n = sizeof(fApIes);
                        memcpy(fApIes, buf + off + 36, n);
                        fApIeLen = (uint16_t)n;
                        fApCap = (uint16_t)(buf[off + 34] | (buf[off + 35] << 8));
                        setProperty("ap-ies", fApIes, fApIeLen);
                        {                                      // readable summary of the router's RSN information element
                            const uint8_t *rp;
                            int rl = findIe(fApIes, fApIeLen, 48, &rp);
                            char s[160];
                            if (rl >= 8) {
                                int m = snprintf(s, sizeof(s), "group %02x:%02x:%02x:%02x pairwise",
                                                 rp[2], rp[3], rp[4], rp[5]);
                                uint16_t pc = (uint16_t)(rp[6] | (rp[7] << 8));
                                int o = 8;
                                for (uint16_t q = 0; q < pc && q < 4 && o + 4 <= rl; q++, o += 4)
                                    m += snprintf(s + m, sizeof(s) - m, " %02x:%02x:%02x:%02x", rp[o], rp[o+1], rp[o+2], rp[o+3]);
                                o = 8 + 4 * pc;
                                if (o + 2 <= rl) {
                                    uint16_t ac = (uint16_t)(rp[o] | (rp[o+1] << 8));
                                    o += 2;
                                    m += snprintf(s + m, sizeof(s) - m, " akm");
                                    for (uint16_t q = 0; q < ac && q < 4 && o + 4 <= rl; q++, o += 4)
                                        m += snprintf(s + m, sizeof(s) - m, " %02x:%02x:%02x:%02x", rp[o], rp[o+1], rp[o+2], rp[o+3]);
                                    if (o + 2 <= rl) snprintf(s + m, sizeof(s) - m, " caps %02x%02x", rp[o], rp[o+1]);
                                }
                                setProperty("ap-rsn", s);
                            } else {
                                setProperty("ap-rsn", "none");
                            }
                        }
                        setProperty("ap-cap", (uint64_t)fApCap, 16);
                        fHaveAp = true;
                        fApChannel = channel;
                        {                                          // the beacon's DS Parameter Set names the real channel;
                            const uint8_t *dp;                     // neighbouring channels also hear a strong beacon
                            int dl = findIe(fApIes, fApIeLen, 3, &dp);
                            setProperty("ap-scan-channel", (uint64_t)channel, 8);
                            if (dl >= 1 && dp[0] >= 1 && dp[0] <= 13) fApChannel = dp[0];
                        }
                    }
                }
                if (!haveSsid) {
                    uint32_t ie = off + 24 + 12;       // 802.11 header + fixed beacon fields
                    if (ie + 2 <= got && buf[ie] == 0 && buf[ie + 1] <= 32 && ie + 2 + buf[ie + 1] <= got) {
                        char ssid[33];
                        uint8_t len = buf[ie + 1];
                        for (uint8_t i = 0; i < len; i++) {
                            uint8_t ch = buf[ie + 2 + i];
                            ssid[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
                        }
                        ssid[len] = 0;
                        snprintf(key, sizeof(key), "rx-ch%d%c-first-ssid", channel, tag);
                        setProperty(key, ssid);
                        haveSsid = true;
                    }
                }
            }
        }
        if (!crc && off + 36 <= got && buf[off] == 0x50) {      // probe response
            probeResp++;
            if (!haveProbeSsid) {
                uint32_t ie = off + 24 + 12;
                if (ie + 2 <= got && buf[ie] == 0 && buf[ie + 1] <= 32 && ie + 2 + buf[ie + 1] <= got) {
                    char ssid[33];
                    uint8_t len = buf[ie + 1];
                    for (uint8_t i = 0; i < len; i++) {
                        uint8_t ch = buf[ie + 2 + i];
                        ssid[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
                    }
                    ssid[len] = 0;
                    snprintf(key, sizeof(key), "rx-ch%d%c-probe-ssid", channel, tag);
                    setProperty(key, ssid);
                    haveProbeSsid = true;
                }
            }
        }
        if (!crc && off + 30 <= got && buf[off] == 0x10) {      // association response
            assocResp++;
            if (assocResp == 1) {
                assocStatus = (uint32_t)buf[off + 26] | ((uint32_t)buf[off + 27] << 8);
                assocAid = ((uint32_t)buf[off + 28] | ((uint32_t)buf[off + 29] << 8)) & 0x3fff;
            }
        }
        if (!crc && off + 40 <= got && (buf[off] & 0x0c) == 0x08) {   // data frame: look for EAPOL (LLC/SNAP 0x888e)
            uint32_t hl = 24 + ((buf[off] & 0x80) ? 2 : 0);
            static const uint8_t kSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};
            if (buf[off + 1] & 0x40) {                                 // protected data frame
                prot++;
                if (secFirst == 0xff) {
                    secFirst = (d0 >> 20) & 7; swdecFirst = (d0 >> 27) & 1;
                    uint32_t d2 = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
                    protIvLen = (d2 >> 18) & 0x3f;
                    protMcs = buf[12] & 0x7f;
                    uint32_t dl = got > off ? got - off : 0;               // 802.11 header, IV and the first payload bytes
                    if (dl > 96) dl = 96;
                    snprintf(key, sizeof(key), "rx-ch%d%c-prot-first-bytes", channel, tag);
                    setProperty(key, buf + off, dl);
                }
                if (off + hl + 8 + 8 <= got && memcmp(buf + off + hl + 8, kSnap, 3) == 0) {   // decrypted LLC: ethertype
                    uint16_t et = (uint16_t)((buf[off + hl + 8 + 6] << 8) | buf[off + hl + 8 + 7]);
                    if (et == 0x0806) ethArp++; else if (et == 0x0800) ethIp4++; else if (et == 0x86dd) ethIp6++; else ethOther++;
                }
                if (off + hl + 8 + 6 <= got && memcmp(buf + off + hl + 8, kSnap, 6) == 0) decSnap++;   // LLC after the 8-byte IV
            }
            {                                                              // DHCP reply (BOOTP op 2) from the router, decrypted by the chip
                bool pr = (buf[off + 1] & 0x40) != 0;
                uint32_t p0 = off + hl + (pr ? 8 : 0);                     // 8-byte IV stays in the frame
                uint32_t fend = off + pktlen;
                if (fend > got) fend = got;
                if (pktlen > 4 && fend >= 4) fend -= 4;                    // drop FCS
                static const uint8_t kIpSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00};
                if (p0 + 8 + 20 + 8 + 240 <= fend && memcmp(buf + p0, kIpSnap, 8) == 0) {
                    uint32_t ip = p0 + 8;
                    uint32_t ihl = (uint32_t)(buf[ip] & 0x0f) * 4;
                    if ((buf[ip] >> 4) == 4 && ihl >= 20 && buf[ip + 9] == 17 && ip + ihl + 8 + 240 <= fend) {
                        uint32_t u = ip + ihl;
                        uint16_t sp = (uint16_t)((buf[u] << 8) | buf[u + 1]);
                        uint16_t dp = (uint16_t)((buf[u + 2] << 8) | buf[u + 3]);
                        uint32_t b = u + 8;
                        static const uint8_t kCookie[4] = {0x63, 0x82, 0x53, 0x63};
                        if (sp == 67 && dp == 68 && buf[b] == 2 && memcmp(buf + b + 4, kDhcpXid, 4) == 0 &&
                            memcmp(buf + b + 236, kCookie, 4) == 0) {
                            dhcpReplies++;
                            uint8_t mt = 0;
                            const uint8_t *srv = nullptr;
                            uint32_t o = b + 240;
                            while (o + 2 <= fend && buf[o] != 255) {
                                if (buf[o] == 0) { o++; continue; }
                                uint8_t ot = buf[o], ol = buf[o + 1];
                                if (o + 2 + ol > fend) break;
                                if (ot == 53 && ol == 1) mt = buf[o + 2];
                                if (ot == 54 && ol == 4) srv = buf + o + 2;
                                o += 2 + ol;
                            }
                            if (mt == 2 && !fHaveOffer) {                  // DHCP offer
                                memcpy(fOfferIp, buf + b + 16, 4);
                                if (srv) memcpy(fOfferSrv, srv, 4); else memset(fOfferSrv, 0, 4);
                                fOfferProt = pr;
                                fOfferBcast = (buf[off + 4] & 1) != 0;     // addr1 group bit: sent with the group key
                                fHaveOffer = true;
                            }
                        }
                    }
                }
            }
            if (off + hl + 8 <= got && memcmp(buf + off + hl, kSnap, 8) == 0) {
                eapol++;
                uint32_t es = off + hl + 8;                        // EAPOL header
                if (es + 99 <= got && buf[es + 1] == 3 && buf[es + 4] == 2) {   // EAPOL-Key, RSN descriptor
                    uint16_t ki = (uint16_t)((buf[es + 5] << 8) | buf[es + 6]);
                    uint32_t flen = 4 + (uint32_t)((buf[es + 2] << 8) | buf[es + 3]);
                    if (ki == 0x008a) {                            // message 1: ANonce and replay counter
                        if (!fHaveMsg1 || memcmp(fAnonce, buf + es + 17, 32) != 0) {
                            memcpy(fAnonce, buf + es + 17, 32);    // a new ANonce: the router restarted the handshake
                            fNewMsg1 = true;
                        }
                        memcpy(fReplay, buf + es + 9, 8);
                        fHaveMsg1 = true;
                    } else if (ki == 0x13ca && fHavePtk && flen <= 400 && es + flen <= got) {   // message 3: check its MIC
                        uint8_t tmp[400];
                        memcpy(tmp, buf + es, flen);
                        memset(tmp + 81, 0, 16);
                        uint8_t mic[16];
                        rtlcrypto::eapol_mic(fPtk, tmp, flen, mic);
                        if (memcmp(mic, buf + es + 81, 16) == 0) {
                            msg3Ok++;
                            if (!fHaveMsg3) { memcpy(fMsg3, buf + es, flen); fMsg3Len = (uint16_t)flen; fHaveMsg3 = true; }
                            else { memcpy(fMsg3 + 9, buf + es + 9, 8); fMsg3Retx++; }   // retransmitted message 3: newest replay counter
                        } else msg3Bad++;
                    }
                }
            }
        }
        if (!crc && off + 30 <= got && buf[off] == 0xb0) {      // authentication response
            authResp++;
            if (authResp == 1) authStatus = (uint32_t)buf[off + 28] | ((uint32_t)buf[off + 29] << 8);
        }
        if ((fStopOnMsg1 && fHaveMsg1) || (fStopOnMsg3 && (fHaveMsg3 || fNewMsg1)) || (fStopOnOffer && fHaveOffer)) break;
    }
    md->release();
    snprintf(key, sizeof(key), "rx-ch%d%c-assocresp", channel, tag); setProperty(key, (uint64_t)assocResp, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-assoc-status", channel, tag); setProperty(key, (uint64_t)assocStatus, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-assoc-aid", channel, tag); setProperty(key, (uint64_t)assocAid, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-eapol", channel, tag); setProperty(key, (uint64_t)eapol, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-msg3-ok", channel, tag); setProperty(key, (uint64_t)msg3Ok, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-msg3-bad", channel, tag); setProperty(key, (uint64_t)msg3Bad, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-prot", channel, tag); setProperty(key, (uint64_t)prot, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-dec-snap", channel, tag); setProperty(key, (uint64_t)decSnap, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-sec-first", channel, tag); setProperty(key, (uint64_t)secFirst, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-swdec-first", channel, tag); setProperty(key, (uint64_t)swdecFirst, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-authresp", channel, tag); setProperty(key, (uint64_t)authResp, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-auth-status", channel, tag); setProperty(key, (uint64_t)authStatus, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-proberesp", channel, tag); setProperty(key, (uint64_t)probeResp, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-icverr", channel, tag); setProperty(key, (uint64_t)icvErr, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-eth-arp", channel, tag); setProperty(key, (uint64_t)ethArp, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-eth-ip4", channel, tag); setProperty(key, (uint64_t)ethIp4, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-eth-ip6", channel, tag); setProperty(key, (uint64_t)ethIp6, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-eth-other", channel, tag); setProperty(key, (uint64_t)ethOther, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-deauth", channel, tag); setProperty(key, (uint64_t)deauth, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-deauth-reason", channel, tag); setProperty(key, (uint64_t)deauthReason, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-dhcp-replies", channel, tag); setProperty(key, (uint64_t)dhcpReplies, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-prot-mcs", channel, tag); setProperty(key, (uint64_t)protMcs, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-prot-ivlen", channel, tag); setProperty(key, (uint64_t)protIvLen, 32);

    snprintf(key, sizeof(key), "rx-ch%d%c-ok", channel, tag);        setProperty(key, (uint64_t)ok, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-timeouts", channel, tag);  setProperty(key, (uint64_t)timeouts, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-other", channel, tag);     setProperty(key, (uint64_t)other, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-last-error", channel, tag); setProperty(key, (uint64_t)lastErr, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-beacons", channel, tag);   setProperty(key, (uint64_t)beacons, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-mgmt", channel, tag);      setProperty(key, (uint64_t)mgmt, 32);
    snprintf(key, sizeof(key), "rx-ch%d%c-crc-err", channel, tag);   setProperty(key, (uint64_t)crcErr, 32);
}

// Send one management frame through bulk-OUT 0x02 (management queue).
// 40-byte TX descriptor (rtl8xxxu_txdesc40) followed by the 802.11 frame.
IOReturn RtlWifiDriver::txMgmt(const uint8_t *frame, uint16_t len, uint8_t rate, uint8_t channel, char tag) {
    if (!fPipeOut2 || len > 200) return kIOReturnBadArgument;
    uint8_t pkt[256];
    memset(pkt, 0, sizeof(pkt));
    uint8_t *d = pkt;
    memcpy(pkt + 40, frame, len);
    bool bmc = (frame[4] & 1);                      // multicast/broadcast destination

    d[0] = len & 0xff; d[1] = len >> 8;             // pkt_size
    d[2] = 40;                                      // pkt_offset = descriptor size
    d[3] = 0x80 | 0x08 | 0x04 | (bmc ? 0x01 : 0);   // OWN | FIRST | LAST | BROADMULTICAST
    uint32_t w;
    w = (0x12u << 8);              memcpy(d + 4,  &w, 4);   // txdw1: queue = MGNT, macid 0
    w = BIT(16);                   memcpy(d + 8,  &w, 4);   // txdw2: AGG_BREAK
    w = BIT(8);                    memcpy(d + 12, &w, 4);   // txdw3: USE_DRIVER_RATE
    w = rate | (6u << 18) | BIT(17); memcpy(d + 16, &w, 4); // txdw4: rate (0 = 1M CCK, 4 = 6M OFDM), retry limit 6
    w = BIT(15);                   memcpy(d + 32, &w, 4);   // txdw8: HW_SEQ_ENABLE
    uint16_t csum = 0;                                      // XOR of the first 16 words, csum field zero
    for (int i = 0; i < 16; i++) csum ^= (uint16_t)(d[2 * i] | (d[2 * i + 1] << 8));
    d[28] = csum & 0xff; d[29] = csum >> 8;

    uint32_t total = 40 + len;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withOptions(kIODirectionOut, total, 1);
    if (!md) return kIOReturnNoMemory;
    memcpy(md->getBytesNoCopy(), pkt, total);
    uint32_t fifoBefore = rd32(0x0204);
    uint32_t got = 0;
    IOReturn r = fPipeOut2->io(md, total, got, 500);
    md->release();

    char key[48];
    snprintf(key, sizeof(key), "tx-ch%d%c-status", channel, tag); setProperty(key, (uint64_t)(uint32_t)r, 32);
    snprintf(key, sizeof(key), "tx-ch%d%c-bytes", channel, tag);  setProperty(key, (uint64_t)got, 32);
    snprintf(key, sizeof(key), "txdma-status-ch%d%c", channel, tag);
    setProperty(key, (uint64_t)rd32(0x0210), 32);          // REG_TXDMA_STATUS
    snprintf(key, sizeof(key), "txpause-ch%d%c", channel, tag);
    setProperty(key, (uint64_t)rd8(0x0522), 8);            // REG_TXPAUSE
    snprintf(key, sizeof(key), "fifopage-ch%d%c-before", channel, tag);
    setProperty(key, (uint64_t)fifoBefore, 32);            // REG_FIFOPAGE
    snprintf(key, sizeof(key), "fifopage-ch%d%c-after", channel, tag);
    setProperty(key, (uint64_t)rd32(0x0204), 32);
    return r;
}

// Broadcast probe request: wildcard SSID, rates 1/2/5.5/11 Mbps
IOReturn RtlWifiDriver::txProbeRequest(uint8_t channel, uint8_t rate, char tag) {
    uint8_t f[32];
    memset(f, 0, sizeof(f));
    f[0] = 0x40;                                    // management, probe request
    memset(f + 4, 0xff, 6);                         // DA
    memcpy(f + 10, &fEfuse[0xd7], 6);               // SA = adapter MAC
    memset(f + 16, 0xff, 6);                        // BSSID
    f[26] = 0x01; f[27] = 0x04;                     // supported rates IE (SSID IE at 24 is empty)
    f[28] = 0x82; f[29] = 0x84; f[30] = 0x8b; f[31] = 0x96;
    return txMgmt(f, sizeof(f), rate, channel, tag);
}

// Unicast open-system authentication request (algorithm 0, sequence 1) to the AP found in the scan
IOReturn RtlWifiDriver::txAuth(uint8_t channel, uint8_t rate, char tag) {
    if (!fHaveAp) return kIOReturnBadArgument;
    uint8_t f[30];
    memset(f, 0, sizeof(f));
    f[0] = 0xb0;                                    // management, authentication
    memcpy(f + 4, fApBssid, 6);                     // DA = AP
    memcpy(f + 10, &fEfuse[0xd7], 6);               // SA = adapter MAC
    memcpy(f + 16, fApBssid, 6);                    // BSSID = AP
    f[24] = 0x00; f[25] = 0x00;                     // algorithm: open system
    f[26] = 0x01; f[27] = 0x00;                     // sequence 1
    f[28] = 0x00; f[29] = 0x00;                     // status 0
    return txMgmt(f, sizeof(f), rate, channel, tag);
}

// Find an information element by tag in a beacon IE list; returns its length or -1.
static int findIe(const uint8_t *ies, uint16_t len, uint8_t tag, const uint8_t **data) {
    uint16_t p = 0;
    while (p + 2 <= len) {
        uint8_t t = ies[p], l = ies[p + 1];
        if (p + 2 + l > len) break;
        if (t == tag) { *data = ies + p + 2; return l; }
        p += 2 + l;
    }
    return -1;
}

// Unicast association request to the AP found in the scan. Test shortcut: the SSID is hard-coded, and the
// supported rates, extended rates and HT capabilities are copied from the AP's own beacon so that the
// AP's basic rate set is satisfied by construction. The RSN IE selects CCMP + PSK with the AP's group cipher.
IOReturn RtlWifiDriver::txAssoc(uint8_t channel, uint8_t rate, char tag) {
    if (!fHaveAp) return kIOReturnBadArgument;
    if (fTargetSsidLen == 0) return kIOReturnBadArgument;
    const uint8_t ssidLen = fTargetSsidLen;
    uint8_t f[200];
    memset(f, 0, sizeof(f));
    f[0] = 0x00;                                    // management, association request
    memcpy(f + 4, fApBssid, 6);                     // DA = AP
    memcpy(f + 10, &fEfuse[0xd7], 6);               // SA = adapter MAC
    memcpy(f + 16, fApBssid, 6);                    // BSSID = AP
    uint16_t n = 24;
    uint16_t cap = (uint16_t)((fApCap & 0x0430) | 0x0001);   // ESS | privacy | short preamble/slot as the AP
    f[n++] = cap & 0xff; f[n++] = cap >> 8;
    f[n++] = 0x0a; f[n++] = 0x00;                   // listen interval 10
    f[n++] = 0x00; f[n++] = ssidLen;                // SSID IE
    memcpy(f + n, fTargetSsid, ssidLen); n += ssidLen;

    const uint8_t *p;
    int l = findIe(fApIes, fApIeLen, 1, &p);        // supported rates, mirrored
    if (l > 0 && l <= 8) { f[n++] = 1; f[n++] = (uint8_t)l; memcpy(f + n, p, l); n += l; }
    l = findIe(fApIes, fApIeLen, 50, &p);           // extended rates (may hold the HT membership selector)
    if (l > 0 && l <= 16) { f[n++] = 50; f[n++] = (uint8_t)l; memcpy(f + n, p, l); n += l; }

    uint8_t group[4] = {0x00, 0x0f, 0xac, 0x04};
    uint8_t rsnCaps[2] = {0x00, 0x00};
    l = findIe(fApIes, fApIeLen, 48, &p);           // AP's RSN IE: take its group cipher and capabilities
    if (l >= 8) {
        memcpy(group, p + 2, 4);
        uint16_t pc = (uint16_t)(p[6] | (p[7] << 8));
        uint16_t o = 8 + 4 * pc;
        if (o + 2 <= l) {
            uint16_t ac = (uint16_t)(p[o] | (p[o + 1] << 8));
            uint16_t co = o + 2 + 4 * ac;
            if (co + 2 <= l) memcpy(rsnCaps, p + co, 2);
        }
    }
    uint16_t rsnStart = n;
    f[n++] = 48; f[n++] = 20;
    f[n++] = 0x01; f[n++] = 0x00;                   // RSN version
    memcpy(f + n, group, 4); n += 4;
    f[n++] = 0x01; f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0x0f; f[n++] = 0xac; f[n++] = 0x04;   // pairwise: CCMP
    f[n++] = 0x01; f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0x0f; f[n++] = 0xac; f[n++] = 0x02;   // AKM: PSK
    f[n++] = rsnCaps[0]; f[n++] = rsnCaps[1];
    memcpy(fOwnRsn, f + rsnStart, 22);

    l = findIe(fApIes, fApIeLen, 45, &p);           // HT capabilities, mirrored
    if (l == 26) { f[n++] = 45; f[n++] = 26; memcpy(f + n, p, 26); n += 26; }

    setProperty("assoc-frame-len", (uint64_t)n, 16);
    return txMgmt(f, n, rate, channel, tag);
}

// Send one data frame through the best-effort queue (bulk-OUT 0x03). Same descriptor layout as txMgmt,
// but queue 0 (BE), first/last segment.
// secType: 0 = clear, 0x00c00000 = AES/CCMP, 0x00400000 = RC4/TKIP (txdw1 bits 22..23, from rtl8xxxu.h).
// For a protected frame the CALLER builds the 8-byte CCMP header (IV) behind the 802.11 header: mac80211 does
// that in software for keys with IEEE80211_KEY_FLAG_GENERATE_IV (set by rtl8xxxu); the chip only encrypts the
// payload and appends the MIC. tag = 0: no ioreg properties (used for every packet of the data path).
IOReturn RtlWifiDriver::txDataSec(const uint8_t *frame, uint16_t len, uint8_t rate, uint8_t channel, char tag, uint32_t secType, bool bmc) {
    if (!fPipeOut3 || len > 1600) return kIOReturnBadArgument;
    uint32_t total = 40 + len;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withOptions(kIODirectionOut, total, 1);
    if (!md) return kIOReturnNoMemory;
    uint8_t *d = (uint8_t *)md->getBytesNoCopy();
    memset(d, 0, 40);
    memcpy(d + 40, frame, len);
    d[0] = len & 0xff; d[1] = len >> 8;             // pkt_size
    d[2] = 40;                                      // pkt_offset = descriptor size
    d[3] = 0x80 | 0x08 | 0x04;                      // OWN | FIRST | LAST
    if (bmc) d[3] |= 0x01;                          // TXDESC_BROADMULTICAST (destination address is group)
    uint32_t w;
    w = (0x0u << 8) | secType;     memcpy(d + 4,  &w, 4);   // txdw1: queue = BE, macid 0, security type
    w = BIT(16);                   memcpy(d + 8,  &w, 4);   // txdw2: AGG_BREAK
    w = BIT(8);                    memcpy(d + 12, &w, 4);   // txdw3: USE_DRIVER_RATE
    w = rate | (6u << 18) | BIT(17); memcpy(d + 16, &w, 4); // txdw4: rate, retry limit 6
    w = BIT(15);                   memcpy(d + 32, &w, 4);   // txdw8: HW_SEQ_ENABLE
    uint16_t csum = 0;
    for (int i = 0; i < 16; i++) csum ^= (uint16_t)(d[2 * i] | (d[2 * i + 1] << 8));
    d[28] = csum & 0xff; d[29] = csum >> 8;

    uint32_t got = 0;
    IOReturn r = fPipeOut3->io(md, total, got, 500);
    md->release();
    if (tag) {
        char key[48];
        snprintf(key, sizeof(key), "tx-ch%d%c-status", channel, tag); setProperty(key, (uint64_t)(uint32_t)r, 32);
        snprintf(key, sizeof(key), "tx-ch%d%c-bytes", channel, tag);  setProperty(key, (uint64_t)got, 32);
    }
    return r;
}

IOReturn RtlWifiDriver::txData(const uint8_t *frame, uint16_t len, uint8_t rate, uint8_t channel, char tag) {
    return txDataSec(frame, len, rate, channel, tag, 0, false);
}

// EAPOL-Key message 2 of the WPA2 four-way handshake: SNonce, our RSN IE, MIC over the whole EAPOL frame
IOReturn RtlWifiDriver::txEapolMsg2(uint8_t channel, char tag) {
    uint8_t f[24 + 8 + 99 + 22];
    memset(f, 0, sizeof(f));
    f[0] = 0x08; f[1] = 0x01;                       // data frame, to DS
    memcpy(f + 4, fApBssid, 6);                     // addr1 = AP
    memcpy(f + 10, &fEfuse[0xd7], 6);               // addr2 = SA
    memcpy(f + 16, fApBssid, 6);                    // addr3 = DA (the authenticator)
    static const uint8_t kSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};
    memcpy(f + 24, kSnap, 8);
    uint8_t *e = f + 32;                            // EAPOL frame: 99 + 22 bytes
    e[0] = 1; e[1] = 3;                             // version 1, type key
    uint16_t body = (99 - 4) + 22;
    e[2] = body >> 8; e[3] = body & 0xff;
    e[4] = 2;                                       // RSN key descriptor
    e[5] = 0x01; e[6] = 0x0a;                       // key info: version 2 (HMAC-SHA1/AES), pairwise, MIC
    memcpy(e + 9, fReplay, 8);                      // replay counter from message 1
    memcpy(e + 17, fSnonce, 32);
    e[97] = 0; e[98] = 22;                          // key data length
    memcpy(e + 99, fOwnRsn, 22);
    uint8_t mic[16];
    rtlcrypto::eapol_mic(fPtk, e, 99 + 22, mic);    // KCK = first 16 bytes of the PTK, MIC field still zero
    memcpy(e + 81, mic, 16);
    return txData(f, sizeof(f), 0x00, channel, tag);   // 1M CCK
}

// Derive the keys and answer message 1; then listen for message 3 and check its MIC
void RtlWifiDriver::runHandshake(uint8_t ch) {
    if (!fHavePmk) { setProperty("hs-status", "no passphrase set"); return; }
    if (!fHaveMsg1) { setProperty("hs-status", "no EAPOL message 1"); return; }
    int attempt = 0;
    IOReturn r = kIOReturnSuccess;
    for (attempt = 1; attempt <= 4; attempt++) {
        read_random(fSnonce, sizeof(fSnonce)); // V28: cryptographic SNonce
        rtlcrypto::derive_ptk(fPmk, fApBssid, &fEfuse[0xd7], fAnonce, fSnonce, fPtk);
        fHavePtk = true;
        fHaveMsg3 = false;
        fNewMsg1 = false;
        r = txEapolMsg2(ch, 'g');
        fStopOnMsg3 = true;                                                // stop at message 3 or at a new message 1
        rxProbe(ch, 'g');
        fStopOnMsg3 = false;
        if (fHaveMsg3 || !fNewMsg1) break;                                 // got message 3, or nothing new arrived
    }
    setProperty("hs-msg2-status", (uint64_t)(uint32_t)r, 32);
    setProperty("hs-attempts", (uint64_t)attempt, 8);
    if (!fHaveMsg3) { setProperty("hs-status", "message 2 sent, no valid message 3"); return; }

    // message 3: key data is AES key-wrapped with the KEK (PTK bytes 16..31)
    uint16_t kdLen = (uint16_t)((fMsg3[97] << 8) | fMsg3[98]);
    uint8_t plain[208];
    bool ok = (kdLen >= 24 && kdLen <= 208 && (uint32_t)99 + kdLen <= fMsg3Len) &&
              rtlcrypto::aes_unwrap(fPtk + 16, fMsg3 + 99, kdLen, plain);
    setProperty("gtk-unwrap-ok", ok ? kOSBooleanTrue : kOSBooleanFalse);
    if (!ok) { setProperty("hs-status", "key data unwrap failed"); return; }

    uint8_t gtk[16];
    uint8_t gtkId = 0;
    bool haveGtk = false;
    uint16_t plen = kdLen - 8;
    uint16_t p = 0;
    while (p + 2 <= plen) {
        uint8_t t = plain[p], l = plain[p + 1];
        if (t == 0xdd && l == 0) break;                                    // padding
        if (p + 2 + l > plen) break;
        if (t == 0xdd && l >= 6 + 16 && plain[p + 2] == 0x00 && plain[p + 3] == 0x0f &&
            plain[p + 4] == 0xac && plain[p + 5] == 0x01) {                // GTK KDE
            gtkId = plain[p + 6] & 3;
            memcpy(gtk, plain + p + 8, 16);                                // only the first 16 bytes go into the CAM
            setProperty("gtk-len", (uint64_t)(l - 6), 8);
            haveGtk = true;
            break;
        }
        p += 2 + l;
    }
    setProperty("gtk-found", haveGtk ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("gtk-keyid", (uint64_t)gtkId, 8);
    if (!haveGtk) { setProperty("hs-status", "no GTK in message 3"); return; }

    IOReturn r4 = txEapolMsg4(ch, 'h');                                    // sent in the clear, before the keys are active
    setProperty("hs-msg4-status", (uint64_t)(uint32_t)r4, 32);
    installKeys((uint8_t)(fOwnRsn[7] & 0x0f), gtkId, gtk, ch);             // group cipher type from our RSN IE
    fMsg3Retx = 0;
    rxProbe(ch, 'i');                                                      // protected data frames after the keys are in
    setProperty("hs-msg3-retx", (uint64_t)fMsg3Retx, 32);                  // > 0: the router repeated message 3
    if (fMsg3Retx > 0) {                                                   // answer the repeated message 3 once more
        IOReturn r4b = txEapolMsg4(ch, 'm');
        setProperty("hs-msg4b-status", (uint64_t)(uint32_t)r4b, 32);
        fMsg3Retx = 0;
    }
    setProperty("hs-status", "message 4 sent, keys installed");
    // V28: skipped 7.5s dummy DHCP test so macOS configd acquires IP immediately
}

static uint16_t ipChecksum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

// Encrypted transmit test: a broadcast DHCP Discover sent as a CCMP-protected data frame (to the AP).
// An offer coming back proves: pairwise key accepted by the router, chip-side CCMP encryption and, when the
// offer is a unicast frame, pairwise decryption in the chip.
void RtlWifiDriver::runDhcpTest(uint8_t ch) {
    uint8_t f[352];
    memset(f, 0, sizeof(f));
    f[0] = 0x08; f[1] = 0x41;                       // data frame, to DS, protected
    memcpy(f + 4, fApBssid, 6);                     // addr1 = AP (receiver)
    memcpy(f + 10, &fEfuse[0xd7], 6);               // addr2 = SA
    memset(f + 16, 0xff, 6);                        // addr3 = DA: broadcast
    // f[24..31]: CCMP header (IV) with the packet number, filled in before every send. mac80211 builds it in software
    // when a key has IEEE80211_KEY_FLAG_GENERATE_IV (set by rtl8xxxu); the chip only encrypts and appends the MIC.
    static const uint8_t kSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00};   // LLC/SNAP, IPv4
    memcpy(f + 32, kSnap, 8);
    uint8_t *ip = f + 40;
    const uint16_t bootpLen = 240 + 19;             // fixed part + options below
    const uint16_t udpLen = 8 + bootpLen;
    const uint16_t ipLen = 20 + udpLen;
    ip[0] = 0x45; ip[2] = ipLen >> 8; ip[3] = ipLen & 0xff;
    ip[8] = 64; ip[9] = 17;                         // TTL, UDP
    memset(ip + 16, 0xff, 4);                       // dst 255.255.255.255, src 0.0.0.0
    uint16_t cs = ipChecksum(ip, 20);
    ip[10] = cs >> 8; ip[11] = cs & 0xff;
    uint8_t *u = ip + 20;
    u[1] = 68; u[3] = 67;                           // ports 68 -> 67
    u[4] = udpLen >> 8; u[5] = udpLen & 0xff;       // UDP checksum 0 = none
    uint8_t *b = u + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;                   // BOOTREQUEST, Ethernet, hlen 6
    memcpy(b + 4, kDhcpXid, 4);
    memcpy(b + 28, &fEfuse[0xd7], 6);               // chaddr; broadcast flag left 0: unicast reply expected
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = 1;                  // DHCP Discover
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, &fEfuse[0xd7], 6); o += 6;   // client id
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;         // parameter request list
    *o++ = 255;
    uint16_t flen = (uint16_t)(24 + 8 + 8 + ipLen); // 327: header, CCMP header, LLC/SNAP, IP packet

    fHaveOffer = false;
    fStopOnOffer = true;
    fRxLoops = 25;                                  // about 2.5 s per round (a beacon every ~100 ms keeps reads returning)
    IOReturn r = kIOReturnSuccess;
    int sent = 0;
    char offerRound = 0;
    for (int i = 0; i < 3 && !fHaveOffer; i++) {    // up to 3 tries; round 3 is a variant: destination = the AP itself, no group flag
        char tg = (char)('j' + i);
        bool alt = (i == 2);
        if (alt) memcpy(f + 16, fApBssid, 6); else memset(f + 16, 0xff, 6);
        fTxPn++;                                    // one packet number per transmitted frame
        f[24] = (uint8_t)fTxPn; f[25] = (uint8_t)(fTxPn >> 8); f[26] = 0; f[27] = 0x20;       // PN0, PN1, rsvd, ExtIV | key id 0
        f[28] = (uint8_t)(fTxPn >> 16); f[29] = (uint8_t)(fTxPn >> 24); f[30] = (uint8_t)(fTxPn >> 32); f[31] = (uint8_t)(fTxPn >> 40);
        r = txDataSec(f, flen, 0x00, ch, tg, 0x00c00000, !alt);   // 1M CCK, AES
        sent++;
        rxProbe(ch, tg);
        if (fHaveOffer) offerRound = tg;
    }
    fStopOnOffer = false;
    fRxLoops = 10;
    setProperty("dhcp-sent", (uint64_t)sent, 8);
    setProperty("dhcp-tx-status", (uint64_t)(uint32_t)r, 32);
    setProperty("dhcp-offer", fHaveOffer ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("dhcp-tx-pn-last", fTxPn, 32);
    if (fHaveOffer) {
        char s[24];
        snprintf(s, sizeof(s), "%d.%d.%d.%d", fOfferIp[0], fOfferIp[1], fOfferIp[2], fOfferIp[3]);
        { char rd[2] = {offerRound, 0}; setProperty("dhcp-offer-round", rd); }
        setProperty("dhcp-offer-ip", s);
        snprintf(s, sizeof(s), "%d.%d.%d.%d", fOfferSrv[0], fOfferSrv[1], fOfferSrv[2], fOfferSrv[3]);
        setProperty("dhcp-server-ip", s);
        setProperty("dhcp-offer-protected", fOfferProt ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("dhcp-offer-group-addr", fOfferBcast ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("hs-status", "DHCP offer received");
    } else {
        setProperty("hs-status", "keys installed, no DHCP offer");
    }
}

// EAPOL-Key message 4 (final message): empty key data, secure + MIC + pairwise
IOReturn RtlWifiDriver::txEapolMsg4(uint8_t channel, char tag) {
    uint8_t f[24 + 8 + 99];
    memset(f, 0, sizeof(f));
    f[0] = 0x08; f[1] = 0x01;                       // data frame, to DS
    memcpy(f + 4, fApBssid, 6);
    memcpy(f + 10, &fEfuse[0xd7], 6);
    memcpy(f + 16, fApBssid, 6);
    static const uint8_t kSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};
    memcpy(f + 24, kSnap, 8);
    uint8_t *e = f + 32;
    e[0] = 1; e[1] = 3;
    uint16_t body = 99 - 4;
    e[2] = body >> 8; e[3] = body & 0xff;
    e[4] = 2;
    e[5] = 0x03; e[6] = 0x0a;                       // key info: version 2, pairwise, MIC, secure
    memcpy(e + 9, fMsg3 + 9, 8);                    // replay counter of message 3
    uint8_t mic[16];
    rtlcrypto::eapol_mic(fPtk, e, 99, mic);
    memcpy(e + 81, mic, 16);
    return txData(f, sizeof(f), 0x00, channel, tag);
}

// rtl8xxxu_cam_write: one key entry = 6 words (control + MAC address, then 16 key bytes)
void RtlWifiDriver::camWrite(uint8_t idx, uint8_t cipher, uint8_t keyId, bool group, const uint8_t *mac, const uint8_t *key16) {
    uint32_t ctrl = ((uint32_t)(cipher & 0x0f) << 2) | keyId | BIT(15);      // CAM_WRITE_VALID
    if (group) ctrl |= BIT(6);
    for (int j = 5; j >= 0; j--) {
        uint32_t v;
        if (j == 0) v = ctrl | ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 24);
        else if (j == 1) v = (uint32_t)mac[2] | ((uint32_t)mac[3] << 8) | ((uint32_t)mac[4] << 16) | ((uint32_t)mac[5] << 24);
        else {
            int i = (j - 2) << 2;
            v = (uint32_t)key16[i] | ((uint32_t)key16[i + 1] << 8) | ((uint32_t)key16[i + 2] << 16) | ((uint32_t)key16[i + 3] << 24);
        }
        wr32(0x0674, v);                                                    // REG_CAM_WRITE
        wr32(0x0670, BIT(31) | BIT(16) | (((uint32_t)idx << 3) + j));       // REG_CAM_CMD: POLLING | WRITE | address
        IODelay(100);
    }
}

// rtl8xxxu_set_key for the pairwise CCMP key (entry 0) and the group key (entry 1), then enable security
void RtlWifiDriver::installKeys(uint8_t gtkCipher, uint8_t gtkId, const uint8_t *gtk16, uint8_t channel) {
    (void)channel;
    fGtkCipher = gtkCipher;
    fTxPn = 0;                                                              // CCMP packet number: first frame uses 1, like mac80211
    uint16_t cr = rd16(REG_CR);
    cr |= BIT(9);                                                           // CR_SECURITY_ENABLE
    wr16(REG_CR, cr);
    wr8(0x0680, BIT(2) | BIT(6) | BIT(3) | BIT(7) | BIT(0) | BIT(1));       // REG_SECURITY_CFG
    camWrite(0, 4, 0, false, fApBssid, fPtk + 32);                          // CCMP (cipher 4), TK = PTK bytes 32..47
    camWrite(1, gtkCipher, gtkId, true, fApBssid, gtk16);
    setProperty("keys-installed", kOSBooleanTrue);
    fKeysUp = true;
    fDriverState = kRtlStateConnected;
    // V27: Activate 8051 H2C firmware rate adaptation (MCS0-MCS7 + SGI), keepalive, and ADDBA
    sendH2c(rtl8188f::buildH2cMediaStatus(true, 0));
    sendH2c(rtl8188f::buildH2cRaMask(0, fHt40Active, true));
    sendH2c(rtl8188f::buildH2cKeepAlive(true, 10));
    uint8_t addba[40];
    uint16_t alen = rtl8188f::buildAddbaRequest(addba, sizeof(addba), fApBssid, &fEfuse[0xd7], 1, 0, 64);
    if (alen > 0) txMgmt(addba, alen, 0x04, fApChannel, 0);
    setProperty("sec-cfg-readback", (uint64_t)rd8(0x0680), 8);
    setProperty("cr-after-keys", (uint64_t)rd16(REG_CR), 16);
}

// Known-answer tests for the crypto code, run in the kernel
void RtlWifiDriver::selfTest() {
    uint8_t o[20];
    rtlcrypto::Sha1 s;
    rtlcrypto::sha1_init(s);
    rtlcrypto::sha1_update(s, (const uint8_t *)"abc", 3);
    rtlcrypto::sha1_final(s, o);
    static const uint8_t kSha[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    setProperty("selftest-sha1", memcmp(o, kSha, 20) == 0 ? kOSBooleanTrue : kOSBooleanFalse);
    uint8_t key[20];
    memset(key, 0x0b, 20);
    rtlcrypto::hmac_sha1(key, 20, (const uint8_t *)"Hi There", 8, o);
    static const uint8_t kHmac[20] = {0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
    setProperty("selftest-hmac", memcmp(o, kHmac, 20) == 0 ? kOSBooleanTrue : kOSBooleanFalse);
    uint8_t pmk[32];
    rtlcrypto::pbkdf2_pmk("password", (const uint8_t *)"IEEE", 4, pmk);
    static const uint8_t kPmk[32] = {0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
                                     0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};
    setProperty("selftest-pbkdf2", memcmp(pmk, kPmk, 32) == 0 ? kOSBooleanTrue : kOSBooleanFalse);
    // AES-128 decrypt (FIPS-197) and RFC 3394 key unwrap known answers
    uint8_t k16[16];
    for (int i = 0; i < 16; i++) k16[i] = (uint8_t)i;
    rtlcrypto::Aes aes;
    rtlcrypto::aes_init(aes, k16);
    static const uint8_t kCt[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t pt[16];
    rtlcrypto::aes_decrypt_block(aes, kCt, pt);
    bool aesOk = true;
    for (int i = 0; i < 16; i++) if (pt[i] != (uint8_t)(0x11 * i)) aesOk = false;
    setProperty("selftest-aes", aesOk ? kOSBooleanTrue : kOSBooleanFalse);
    static const uint8_t kWrapped[24] = {0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5};
    uint8_t unw[16];
    bool wrapOk = rtlcrypto::aes_unwrap(k16, kWrapped, 24, unw);
    for (int i = 0; i < 16; i++) if (unw[i] != (uint8_t)(0x11 * i)) wrapOk = false;
    setProperty("selftest-keywrap", wrapOk ? kOSBooleanTrue : kOSBooleanFalse);
}

// rtl8xxxu_add_interface for a station: beacon control, link type, MAC address (set earlier)
void RtlWifiDriver::setStationMode() {
    uint8_t v8 = rd8(0x0550);                              // REG_BEACON_CTRL
    v8 |= BIT(0) | BIT(3) | BIT(4);                        // ATIM | FUNCTION_ENABLE | DISABLE_TSF_UPDATE
    wr8(0x0550, v8);
    v8 = rd8(0x0454);                                      // REG_CCK_CHECK
    v8 &= ~BIT(5);                                         // BIT_BCN_PORT_SEL
    wr8(0x0454, v8);
    v8 = rd8(0x0102) & 0x0c;                               // REG_MSR, port 0
    v8 |= 0x2;                                             // MSR_LINKTYPE_STATION
    wr8(0x0102, v8);
    setProperty("msr-readback", (uint64_t)rd8(0x0102), 8);
    setProperty("beacon-ctrl-readback", (uint64_t)rd8(0x0550), 8);
    setProperty("cck-check-readback", (uint64_t)rd8(0x0454), 8);
}

// rtl8188f_set_crystal_cap: the reference applies efuse xtal_k & 0x3f after the PHY tables.
// Called after the eFuse read here (the efuse is read late in this driver's start()).
void RtlWifiDriver::setCrystalCap(uint8_t cap) {
    uint32_t v = rd32(0x0024);                             // REG_AFE_XTAL_CTRL
    setProperty("afe-xtal-before", (uint64_t)v, 32);
    setProperty("xtal-cap", (uint64_t)cap, 8);
    if (cap == 0) return;                                  // reference skips the write when equal to its initial value 0
    v &= ~(0x007e0000u | 0x0001f800u);                     // XTAL1 = GENMASK(22,17), XTAL0 = GENMASK(16,11)
    v |= ((uint32_t)cap << 17) | ((uint32_t)cap << 11);
    wr32(0x0024, v);
    setProperty("afe-xtal-after", (uint64_t)rd32(0x0024), 32);
}

// Active scan of channels 1..13: two broadcast probe requests per channel, then collect beacons and
// probe responses (synchronous bulk-IN reads). Unique BSSIDs are published as scan-NN properties.
struct RtlScanEntry { uint8_t bssid[6]; char ssid[33]; uint8_t ch; uint8_t sec; };

void RtlWifiDriver::scanAll() {
    if (!fPipeIn) return;
    const uint32_t kBuf = 16384;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kBuf, 1);
    if (!md) return;
    uint8_t *buf = (uint8_t *)md->getBytesNoCopy();
    RtlScanEntry list[24];
    unsigned n = 0;
    uint32_t frames = 0;

    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (setChannel20(ch) != kIOReturnSuccess) continue;
        for (int t = 0; t < 2; t++) { txProbeRequest(ch, 0x00, 'a'); IOSleep(5); }
        int idle = 0;
        for (int i = 0; i < 12 && idle < 2; i++) {
            uint32_t got = 0;
            IOReturn r = fPipeIn->io(md, kBuf, got, 80);
            if (r == kIOReturnTimeout) { idle++; continue; }
            idle = 0;
            if (r != kIOReturnSuccess || got < 24) continue;

            uint32_t d0 = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            uint32_t pktlen = d0 & 0x3fff;
            bool crc = (d0 >> 14) & 1;
            uint32_t off = 24 + ((d0 >> 16) & 0xf) * 8 + ((d0 >> 24) & 3);
            if (crc || off + 36 > got) continue;
            uint8_t fc0 = buf[off];
            if (fc0 != 0x80 && fc0 != 0x50) continue;          // beacon or probe response
            frames++;

            const uint8_t *bssid = buf + off + 16;
            uint16_t cap = (uint16_t)(buf[off + 34] | (buf[off + 35] << 8));
            uint32_t ie = off + 36;
            uint32_t end = off + pktlen;
            if (end > got) end = got;
            char ssid[33] = {0};
            uint8_t apCh = ch;
            bool rsn = false, wpa = false;
            while (ie + 2 <= end) {
                uint8_t tag = buf[ie], len = buf[ie + 1];
                if (ie + 2 + len > end) break;
                if (tag == 0 && len <= 32) {
                    for (uint8_t k = 0; k < len; k++) {
                        uint8_t c = buf[ie + 2 + k];
                        ssid[k] = (c >= 32 && c < 127) ? (char)c : '?';
                    }
                    ssid[len] = 0;
                } else if (tag == 3 && len >= 1) {
                    apCh = buf[ie + 2];
                } else if (tag == 48) {
                    rsn = true;
                } else if (tag == 221 && len >= 4 && buf[ie + 2] == 0x00 && buf[ie + 3] == 0x50 &&
                           buf[ie + 4] == 0xf2 && buf[ie + 5] == 0x01) {
                    wpa = true;
                }
                ie += 2 + len;
            }

            bool known = false;
            for (unsigned k = 0; k < n; k++)
                if (memcmp(list[k].bssid, bssid, 6) == 0) { known = true; break; }
            if (!known && n < 24) {
                memcpy(list[n].bssid, bssid, 6);
                memcpy(list[n].ssid, ssid, sizeof(ssid));
                list[n].ch = apCh;
                list[n].sec = rsn ? 3 : (wpa ? 2 : ((cap & 0x10) ? 1 : 0));
                n++;
            }
        }
    }
    md->release();

    memset(&fScanList, 0, sizeof(fScanList));
    fScanList.abiVersion = RTL_WIFI_ABI_VERSION;
    fScanList.count = (n > RTL_WIFI_MAX_SCAN_COUNT) ? RTL_WIFI_MAX_SCAN_COUNT : n;
    for (unsigned k = 0; k < fScanList.count; k++) {
        memcpy(fScanList.entries[k].bssid, list[k].bssid, 6);
        fScanList.entries[k].channel = list[k].ch;
        fScanList.entries[k].security = (list[k].sec >= 2) ? kRtlSecWpa2Psk : kRtlSecOpen;
        fScanList.entries[k].rssiDbm = -60;
        size_t sl = strlen(list[k].ssid);
        if (sl > RTL_WIFI_MAX_SSID_LEN) sl = RTL_WIFI_MAX_SSID_LEN;
        fScanList.entries[k].ssidLen = (uint8_t)sl;
        memcpy(fScanList.entries[k].ssid, list[k].ssid, sl);
        fScanList.entries[k].htCapable = 1;
    }
    static const char *kSec[] = {"open", "WEP", "WPA", "WPA2"};
    setProperty("scan-count", (uint64_t)n, 32);
    setProperty("scan-frames", (uint64_t)frames, 32);
    for (unsigned k = 0; k < n; k++) {
        char key[16], val[96];
        snprintf(key, sizeof(key), "scan-%02u", k);
        snprintf(val, sizeof(val), "ch=%d %02x:%02x:%02x:%02x:%02x:%02x %s %s", list[k].ch,
                 list[k].bssid[0], list[k].bssid[1], list[k].bssid[2],
                 list[k].bssid[3], list[k].bssid[4], list[k].bssid[5],
                 kSec[list[k].sec], list[k].ssid);
        setProperty(key, val);
    }
}

// ---------- lifecycle ----------



void RtlWifiDriver::sendH2c(const rtl8188f::H2cCommand &cmd) {
    for (int i = 0; i < 50; ++i) {
        if ((rd8(rtl8188f::kRegHmetfr) & (1u << fH2cBox)) == 0) break;
        IODelay(100);
    }
    wr16(static_cast<uint16_t>(rtl8188f::kRegHmeboxExt0 + 2u * fH2cBox), cmd.ext);
    wr32(static_cast<uint16_t>(rtl8188f::kRegHmebox0 + 4u * fH2cBox), cmd.cmd);
    fH2cBox = (fH2cBox + 1u) & 3u;
}

void RtlWifiDriver::setBssidFilter(bool enable) {
    uint32_t rcr = rd32(rtl8188f::kRegRcr);
    if (enable && fHaveAp) {
        for (int i = 0; i < 6; ++i) wr8(static_cast<uint16_t>(rtl8188f::kRegBssid + i), fApBssid[i]);
        rcr |= rtl8188f::kRcrCbssidData | rtl8188f::kRcrCbssidBcn;
    } else {
        rcr &= ~(rtl8188f::kRcrCbssidData | rtl8188f::kRcrCbssidBcn);
    }
    wr32(rtl8188f::kRegRcr, rcr);
}

IOReturn RtlWifiDriver::setChannelBw(uint8_t primaryCh, int8_t secOffset) {
    if (secOffset == 0 || !fHt40Active) {
        fHt40Active = false;
        return setChannel20(primaryCh);
    }
    uint8_t centerCh = static_cast<uint8_t>(primaryCh + (secOffset > 0 ? 2 : -2));
    if (centerCh < 3 || centerCh > 11) {
        fHt40Active = false;
        return setChannel20(primaryCh);
    }
    IOReturn r = setChannel20(centerCh);
    if (r != kIOReturnSuccess) return r;
    // Enable 40 MHz BB/RF mode (rtl8xxxu_8188f HT40 path)
    wr32(0x0800, rd32(0x0800) | BIT(0));
    wr32(0x0900, rd32(0x0900) | BIT(0));
    wr8(0x0483, (secOffset > 0) ? 0x02 : 0x01); // REG_DATA_SC (upper/lower 20MHz subchannel)
    uint32_t rf18 = readRfReg(0x18);
    rf18 &= ~0x00C00u; // Clear 20MHz BW bits -> 40MHz
    writeRfReg(0x18, rf18);
    fHt40Active = true;
    return kIOReturnSuccess;
}

void RtlWifiDriver::handleGtkRekey(const uint8_t *eapol, size_t len) {
    auto res = rtl8188f::unwrapEapolGroupKeyMsg1(eapol, len, fPtk);
    if (!res.ok) return;
    camWrite(1, fGtkCipher, res.gtkId, true, fApBssid, res.gtk16);
    fGtkRekeyCount++;

    // Reply with encrypted EAPOL Group Key Msg 2
    uint8_t frame[42 + 99];
    uint8_t ethHdr[14]{};
    memcpy(ethHdr, fApBssid, 6);
    ethHdr[12] = 0x88; ethHdr[13] = 0x8E;
    fTxPn++;
    uint16_t hlen = rtl8188f::writeQosDataHeader(frame, ethHdr, fApBssid, &fEfuse[0xd7], true, fTxPn, 6);
    uint16_t elen = rtl8188f::buildEapolGroupKeyMsg2(frame + hlen, sizeof(frame) - hlen, res.replayCounter, fPtk);
    if (elen > 0) {
        txDataSec(frame, static_cast<uint16_t>(hlen + elen), 0x04, fApChannel, 0, rtl8188f::kTxDw1SecCcmp, false);
    }
    rtlcrypto::secure_zero(&res, sizeof(res));
}

bool RtlWifiDriver::connectToCurrentTarget() {
    if (!fPipeIn || fTargetSsidLen == 0) return false;
    setBssidFilter(false);
    fDriverState = kRtlStateConnecting;
    fHaveAp = false;
    fHaveMsg1 = fHaveMsg3 = fNewMsg1 = false;
    fRxReorder.reset(0);

    // Passive-first on regulatory channels 12-13, active on 1-11
    static const uint8_t kProbeCh[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    for (unsigned k = 0; k < sizeof(kProbeCh); k++) {
        uint8_t ch = kProbeCh[k];
        if (setChannel20(ch) != kIOReturnSuccess) continue;
        if (ch <= 11) {
            for (int t = 0; t < 2; t++) { txProbeRequest(ch, 0x00, 0); IOSleep(8); }
        }
        rxProbe(ch, 'a');
        if (fHaveAp) break;
    }
    if (!fHaveAp) { fDriverState = kRtlStateDisconnected; return false; }

    fApSecOffset = rtl8188f::parseHtOpSecondaryOffset(fApIes, fApIeLen);
    if (setChannelBw(fApChannel, fHt40Active ? fApSecOffset : 0) != kIOReturnSuccess) {
        fDriverState = kRtlStateDisconnected;
        return false;
    }

    for (int t = 0; t < 2; t++) { txAuth(fApChannel, 0x04, 0); IOSleep(10); }
    rxProbe(fApChannel, 'e');
    for (int t = 0; t < 2; t++) { txAuth(fApChannel, 0x00, 0); IOSleep(10); }
    rxProbe(fApChannel, 'c');
    txAssoc(fApChannel, 0x00, 0);
    IOSleep(10);

    if (fIsEncrypted) {
        fStopOnMsg1 = true;
        rxProbe(fApChannel, 'f');
        fStopOnMsg1 = false;
        runHandshake(fApChannel);
    } else {
        // Open (unencrypted) network: skip 4-way handshake & activate H2C RA directly!
        fKeysUp = true;
        fDriverState = kRtlStateConnected;
        sendH2c(rtl8188f::buildH2cMediaStatus(true, 0));
        sendH2c(rtl8188f::buildH2cRaMask(0, fHt40Active, true));
        sendH2c(rtl8188f::buildH2cKeepAlive(true, 10));
    }

    if (fKeysUp) {
        setBssidFilter(true); // Drop off-network traffic in silicon!
        uint64_t bps = fHt40Active ? 150000000ULL : 72200000ULL;
        setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, getCurrentMedium(), bps);
        if (fNetifEnabled) startRx();
        return true;
    }
    fDriverState = kRtlStateDisconnected;
    return false;
}

bool RtlWifiDriver::startHardware(IOService *provider) {
    fInterface = nullptr;
    fPipeIn = fPipeOut2 = fPipeOut3 = nullptr;
    fHaveAp = false; fHavePmk = fHavePtk = fHaveMsg1 = fStopOnMsg1 = false;
    fHaveMsg3 = false; fMsg3Len = 0; fStopOnMsg3 = false;
    fRxLoops = 10; fStopOnOffer = fHaveOffer = fOfferProt = fOfferBcast = false;
    fMsg3Retx = 0; fTxPn = 0; fNetif = nullptr; fNetStats = nullptr;
    fNetifEnabled = fKeysUp = false; fIsEncrypted = true; fApSecOffset = 0;
    fNetTxDropped = 0;
    for (uint32_t i = 0; i < rtl8188f::kRxRingSize; ++i) fRxMd[i] = nullptr;
    fRxInflight = 0; fRxRunning = false; fRxErrRun = 0;
    fTxFrame = nullptr; fGtkCipher = 0;
    fNetTxPackets = fNetTxErrors = fNetRxPackets = fNetRxDropped = 0;
    fApChannel = 0; fApIeLen = 0; fApCap = 0;
    fWatchdogTimer = nullptr; fH2cBox = 0; fHt40Active = false; fBeaconSeen = false;
    fUsbErrWindow = 0; fGtkRekeyCount = 0; fUsbStallRecoveries = 0;
    fAmsduSubframes = 0; fRoamCount = 0;
    fDriverState = kRtlStateDisconnected; fLastRssiDbm = -65;
    memset(fTargetSsid, 0, sizeof(fTargetSsid)); fTargetSsidLen = 0;
    memset(&fScanList, 0, sizeof(fScanList)); fScanList.abiVersion = RTL_WIFI_ABI_VERSION;
    fDig.reset(); fWatchdog.reset(); fThermal.setBaseline(0x20);
    fPmkCache.clear(); fRxReorder.reset(0);

    if (!super::start(provider)) return false;
    setProperty("build-tag", "v28-ultimate");
    fInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!fInterface || !fInterface->open(this)) { fInterface = nullptr; return false; }

    fPipeIn   = fInterface->copyPipe(0x81);
    fPipeOut2 = fInterface->copyPipe(0x02);
    fPipeOut3 = fInterface->copyPipe(0x03);
    fChipCut  = (uint8_t)((rd32(0x00F0) & 0xf000) >> 12);

    if (powerOn() != kIOReturnSuccess) return true;
    configEndpoints((fPipeOut2 ? 1 : 0) + (fPipeOut3 ? 1 : 0));
    initQueueReservedPage();
    initQueuePriority();
    wr16(REG_TRXFF_BNDY + 2, 0x3f7f);

    if (downloadFirmware() != kIOReturnSuccess) return true;
    if (startFirmware() != kIOReturnSuccess) return true;
    if (initMac() != kIOReturnSuccess) return true;
    if (initPhyBb() != kIOReturnSuccess) return true;
    if (initPhyRf() != kIOReturnSuccess) return true;
    if (postRfInit() != kIOReturnSuccess) return true;
    if (initMacTail() != kIOReturnSuccess) return true;

    if (readEfuse() == kIOReturnSuccess && (uint16_t)(fEfuse[0] | (fEfuse[1] << 8)) == 0x8129) {
        setCrystalCap(fEfuse[0xb9] & 0x3f);
        fThermal.setBaseline(fEfuse[0xb8]);
        if (initCalib() == kIOReturnSuccess) {
            initStatistics();
            setChannel20(6);
            setStationMode();
            if (RTL_TEST_SSID[0] != '\0' && strcmp(RTL_TEST_PASSPHRASE, "CHANGE_ME") != 0 && strlen(RTL_TEST_PASSPHRASE) >= 8) {
                size_t sl = strlen(RTL_TEST_SSID);
                if (sl <= RTL_WIFI_MAX_SSID_LEN) {
                    memcpy(fTargetSsid, RTL_TEST_SSID, sl);
                    fTargetSsidLen = (uint8_t)sl;
                    fIsEncrypted = true;
                    rtlcrypto::pbkdf2_pmk(RTL_TEST_PASSPHRASE, (const uint8_t *)fTargetSsid, fTargetSsidLen, fPmk);
                    fHavePmk = true;
                    connectToCurrentTarget();
                }
            }
        }
    }
    return true;
}

static IOPMPowerState kRtlPowerStates[2] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

bool RtlWifiDriver::start(IOService *provider) {
    if (!startHardware(provider)) return false;
    if (!fTxBeRing.init() || !fTxHiRing.init()) return false;

    OSObject *providerClass = provider->getProperty("IOClassNameOverride");
    if (providerClass) setProperty("IOProviderClass", providerClass);

    if (!createMediumTables()) return true;
    if (!attachInterface((IONetworkInterface **)&fNetif, true)) return true;
    if (fNetif) {
        IONetworkData *nd = fNetif->getNetworkData(kIONetworkStatsKey);
        if (nd) fNetStats = static_cast<IONetworkStats *>(const_cast<void *>(nd->getBuffer()));
    }
    setLinkStatus(kIONetworkLinkValid);

    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, kRtlPowerStates, 2);

    if (IOWorkLoop *wl = getWorkLoop()) {
        fWatchdogTimer = IOTimerEventSource::timerEventSource(
            this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &RtlWifiDriver::onWatchdogTick));
        if (fWatchdogTimer) {
            wl->addEventSource(fWatchdogTimer);
            fWatchdogTimer->setTimeoutMS(2000);
        }
    }
    registerService();
    return true;
}

IOReturn RtlWifiDriver::setPowerState(unsigned long powerStateOrdinal, IOService *) {
    if (powerStateOrdinal == 0) {
        // Entering macOS sleep: stop RX and drain queues cleanly
        stopRx();
        if (IOOutputQueue *q = getOutputQueue()) q->stop();
    } else if (fNetifEnabled) {
        if (IOOutputQueue *q = getOutputQueue()) q->start();
        if (fKeysUp) startRx();
    }
    return kIOPMAckImplied;
}

bool RtlWifiDriver::createMediumTables() {
    OSDictionary *dict = OSDictionary::withCapacity(2);
    if (!dict) return false;
    IONetworkMedium *mAuto = IONetworkMedium::medium(kIOMediumEthernetAuto, 150000000ULL);
    if (!mAuto) { dict->release(); return false; }
    IONetworkMedium::addMedium(dict, mAuto);
    mAuto->release();
    bool ok = publishMediumDictionary(dict) && setCurrentMedium(mAuto);
    dict->release();
    return ok;
}

IOOutputQueue *RtlWifiDriver::createOutputQueue() {
    return IOGatedOutputQueue::withTarget(this, getWorkLoop(), 512);
}

IOReturn RtlWifiDriver::getHardwareAddress(IOEthernetAddress *addr) {
    memcpy(addr->bytes, &fEfuse[0xd7], 6);
    return kIOReturnSuccess;
}

IOReturn RtlWifiDriver::getPacketFilters(const OSSymbol *group, UInt32 *filters) const {
    if (group == gIOEthernetWakeOnLANFilterGroup) { *filters = 0; return kIOReturnSuccess; }
    if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast | kIOPacketFilterPromiscuous |
                   kIOPacketFilterMulticast | kIOPacketFilterMulticastAll;
        return kIOReturnSuccess;
    }
    return super::getPacketFilters(group, filters);
}

IOReturn RtlWifiDriver::selectMedium(const IONetworkMedium *medium) { setSelectedMedium(medium); return kIOReturnSuccess; }
IOReturn RtlWifiDriver::setMulticastMode(bool) { return kIOReturnSuccess; }
IOReturn RtlWifiDriver::setMulticastList(IOEthernetAddress *, UInt32) { return kIOReturnSuccess; }
IOReturn RtlWifiDriver::setPromiscuousMode(bool) { return kIOReturnSuccess; }

IOReturn RtlWifiDriver::enable(IONetworkInterface *) {
    if (fNetifEnabled) return kIOReturnSuccess;
    getOutputQueue()->setCapacity(512);
    getOutputQueue()->start();
    uint64_t bps = fHt40Active ? 150000000ULL : 72200000ULL;
    setLinkStatus((fKeysUp ? kIONetworkLinkActive : 0) | kIONetworkLinkValid, getCurrentMedium(), bps);
    fNetifEnabled = true;
    startRx();
    return kIOReturnSuccess;
}

IOReturn RtlWifiDriver::disable(IONetworkInterface *) {
    if (!fNetifEnabled) return kIOReturnSuccess;
    fNetifEnabled = false;
    stopRx();
    getOutputQueue()->stop();
    getOutputQueue()->flush();
    getOutputQueue()->setCapacity(0);
    setLinkStatus(kIONetworkLinkValid, 0);
    return kIOReturnSuccess;
}

void RtlWifiDriver::txBeCompleteStatic(void *owner, void *param, IOReturn status, uint32_t) {
    static_cast<RtlWifiDriver *>(owner)->txBeComplete(static_cast<unsigned>(reinterpret_cast<uintptr_t>(param)), status);
}

void RtlWifiDriver::txHiCompleteStatic(void *owner, void *param, IOReturn status, uint32_t) {
    static_cast<RtlWifiDriver *>(owner)->txHiComplete(static_cast<unsigned>(reinterpret_cast<uintptr_t>(param)), status);
}

void RtlWifiDriver::txBeComplete(unsigned idx, IOReturn status) {
    fTxBeRing.releaseSlot(idx);
    if (status == kIOReturnSuccess) { fNetTxPackets++; if (fNetStats) fNetStats->outputPackets++; fWatchdog.onUsbSuccess(); }
    else { fNetTxErrors++; if (fNetStats) fNetStats->outputErrors++; fUsbErrWindow++; }
    if (IOOutputQueue *q = getOutputQueue()) q->service();
}

void RtlWifiDriver::txHiComplete(unsigned idx, IOReturn status) {
    fTxHiRing.releaseSlot(idx);
    if (status == kIOReturnSuccess) { fNetTxPackets++; if (fNetStats) fNetStats->outputPackets++; fWatchdog.onUsbSuccess(); }
    else { fNetTxErrors++; if (fNetStats) fNetStats->outputErrors++; fUsbErrWindow++; }
    if (IOOutputQueue *q = getOutputQueue()) q->service();
}

// V28 Zero-Copy Dual-Pipe WMM + ARP/DHCP/DNS Fast-Lane TX + USB 512-byte ZLP Guard!
UInt32 RtlWifiDriver::outputPacket(mbuf_t packet, void *) {
    size_t pktlen = 0;
    for (mbuf_t m = packet; m; m = mbuf_next(m)) pktlen += mbuf_len(m);
    if (!fKeysUp || !fHaveAp || pktlen < 14 || pktlen > 1514) {
        freePacket(packet);
        fNetTxErrors++; if (fNetStats) fNetStats->outputErrors++;
        return kIOReturnOutputDropped;
    }

    // Peek only 42 bytes on stack (saves 1.5 KB kernel stack & eliminates double copy!)
    uint8_t hdrPeek[42];
    size_t peekLen = (pktlen < sizeof(hdrPeek)) ? pktlen : sizeof(hdrPeek);
    mbuf_copydata(packet, 0, peekLen, hdrPeek);

    auto cls = rtl8188f::classifyEthHeader(hdrPeek, peekLen);
    bool useHi = cls.highPriorityPipe && (fPipeOut2 != nullptr);
    int slotIdx = useHi ? fTxHiRing.acquireSlot() : fTxBeRing.acquireSlot();
    if (slotIdx < 0) return kIOReturnOutputStall;

    RtlTxSlot &slot = useHi ? fTxHiRing.slot(static_cast<uint32_t>(slotIdx))
                            : fTxBeRing.slot(static_cast<uint32_t>(slotIdx));
    fTxPn++;
    uint16_t hlen = rtl8188f::writeQosDataHeader(
        slot.buf + rtl8188f::kTxDescSize, hdrPeek, fApBssid, &fEfuse[0xd7],
        fIsEncrypted, fTxPn, cls.tid);
    size_t payloadLen = pktlen - 14;
    // Single copy directly from mbuf into pre-mapped DMA buffer!
    mbuf_copydata(packet, 14, payloadLen, slot.buf + rtl8188f::kTxDescSize + hlen);
    uint16_t flen = static_cast<uint16_t>(hlen + payloadLen);

    rtl8188f::TxDescParams tp{};
    tp.frameLen      = flen;
    tp.secType       = fIsEncrypted ? rtl8188f::kTxDw1SecCcmp : 0u;
    tp.tid           = cls.tid;
    tp.isMulticast   = (hdrPeek[0] & 1) != 0;
    tp.isQosData     = true;
    tp.allowAmpdu    = !tp.isMulticast && !cls.isControlFastLane;
    tp.useDriverRate = false;
    tp.macId         = 0;
    rtl8188f::buildTxDesc40(slot.buf, tp);

    uint32_t usbLen = rtl8188f::safeUsbBulkOutLen(rtl8188f::kTxDescSize + flen, slot.buf, rtl8188f::kTxBufSize);

    IOUSBHostCompletion c{};
    c.owner     = this;
    c.action    = useHi ? &RtlWifiDriver::txHiCompleteStatic : &RtlWifiDriver::txBeCompleteStatic;
    c.parameter = reinterpret_cast<void *>(static_cast<uintptr_t>(slotIdx));

    IOUSBHostPipe *pipe = useHi ? fPipeOut2 : fPipeOut3;
    IOReturn r = pipe ? pipe->io(slot.md, usbLen, &c, 0) : kIOReturnNoDevice;
    freePacket(packet);
    if (r != kIOReturnSuccess) {
        if (useHi) fTxHiRing.releaseSlot(static_cast<uint32_t>(slotIdx));
        else       fTxBeRing.releaseSlot(static_cast<uint32_t>(slotIdx));
        fNetTxErrors++; if (fNetStats) fNetStats->outputErrors++; fUsbErrWindow++;
        return kIOReturnOutputDropped;
    }
    return kIOReturnOutputSuccess;
}

void RtlWifiDriver::rxCompleteStatic(void *owner, void *param, IOReturn status, uint32_t bytes) {
    static_cast<RtlWifiDriver *>(owner)->rxComplete(static_cast<unsigned>(reinterpret_cast<uintptr_t>(param)), status, bytes);
}

void RtlWifiDriver::submitRx(unsigned idx) {
    IOUSBHostCompletion c{};
    c.owner     = this;
    c.action    = &RtlWifiDriver::rxCompleteStatic;
    c.parameter = reinterpret_cast<void *>(static_cast<uintptr_t>(idx));
    OSIncrementAtomic(&fRxInflight);
    IOReturn r = fPipeIn ? fPipeIn->io(fRxMd[idx], rtl8188f::kRxBufSize, &c, 0) : kIOReturnNoDevice;
    if (r != kIOReturnSuccess) { OSDecrementAtomic(&fRxInflight); fRxRunning = false; fUsbErrWindow++; }
}

void RtlWifiDriver::startRx() {
    if (fRxRunning || !fPipeIn || !fKeysUp) return;
    for (unsigned i = 0; i < rtl8188f::kRxRingSize; i++) {
        if (!fRxMd[i]) fRxMd[i] = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, rtl8188f::kRxBufSize, 1);
        if (!fRxMd[i]) return;
    }
    fRxErrRun = 0; fRxRunning = true;
    for (unsigned i = 0; i < rtl8188f::kRxRingSize && fRxRunning; i++) submitRx(i);
}

void RtlWifiDriver::stopRx() {
    fRxRunning = false;
    if (fPipeIn) fPipeIn->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
    for (int i = 0; i < 100 && fRxInflight > 0; i++) IOSleep(10);
    fRxReorder.flushAll([this](mbuf_t m) { if (m) freePacket(m); });
}

void RtlWifiDriver::rxComplete(unsigned idx, IOReturn status, uint32_t bytes) {
    if (status == kIOReturnSuccess && fRxRunning) {
        fRxErrRun = 0; fWatchdog.onUsbSuccess();
        handleRx(static_cast<const uint8_t *>(fRxMd[idx]->getBytesNoCopy()), bytes);
    } else if (status != kIOReturnSuccess) { fRxErrRun++; fUsbErrWindow++; }
    bool dead = (status == kIOReturnAborted || status == kIOReturnNoDevice ||
                 status == kIOReturnNotResponding || fRxErrRun > 100);
    if (!fRxRunning || dead) {
        if (dead && fRxRunning) fRxRunning = false;
        OSDecrementAtomic(&fRxInflight);
        return;
    }
    OSDecrementAtomic(&fRxInflight);
    submitRx(idx);
}

void RtlWifiDriver::handleRx(const uint8_t *buf, uint32_t len) {
    uint32_t pos = 0, queued = 0;
    while (pos + 24 <= len) {
        uint32_t d0 = rtl8188f::getLe32(buf + pos);
        uint32_t d3 = rtl8188f::getLe32(buf + pos + 12);
        uint32_t pktlen  = d0 & 0x3fffu;
        uint32_t drvinfo = ((d0 >> 16) & 0xfu) * 8u;
        uint32_t shift   = (d0 >> 24) & 3u;
        uint32_t off     = pos + 24u + drvinfo + shift;
        uint32_t next    = pos + ((pktlen + drvinfo + shift + 24u + 7u) & ~7u);
        if (pktlen == 0 || off + pktlen > len) break;

        // Decode real hardware PHY RSSI from drvinfo (REG_RX_DRVINFO_SZ=4 -> 32 bytes)
        if (drvinfo >= 16 && ((d0 >> 26) & 1u) && pos + 24u + drvinfo <= len) {
            fLastRssiDbm = rtl8188f::extractPhyRssiDbm(buf + pos + 24u, drvinfo, d3 & 0x7Fu);
        }
        if (!((d0 >> 14) & 1u) && !((d0 >> 15) & 1u)) {
            if (rxFrame(buf + off, pktlen, d0)) queued++;
        }
        pos = next;
    }
    if (queued > 0 && fNetif) fNetif->flushInputQueue();
}

bool RtlWifiDriver::rxFrame(const uint8_t *f, uint32_t n, uint32_t d0) {
    if (n < 24 || !fNetif || !fNetifEnabled) return false;
    uint8_t fc0 = f[0], fc1 = f[1];

    if (fc0 == 0x80 && memcmp(f + 16, fApBssid, 6) == 0) {
        fBeaconSeen = true; fWatchdog.onBeaconReceived(); return false;
    }
    if (rtl8188f::isDeauthOrDisassoc(fc0) && memcmp(f + 10, fApBssid, 6) == 0) {
        fKeysUp = false; fDriverState = kRtlStateRecovering;
        setLinkStatus(kIONetworkLinkValid, 0); return false;
    }

    if (n < 32 || ((fc0 >> 2) & 3) != 2 || (fc0 & 0x40) || (fc1 & 3) != 2) return false;
    if (memcmp(f + 10, fApBssid, 6) != 0 || memcmp(f + 16, &fEfuse[0xd7], 6) == 0) return false;

    bool isQos = (fc0 & 0x80) != 0;
    uint32_t hl = isQos ? 26u : 24u;
    bool isAmsdu = isQos && ((f[24] & 0x80) != 0);
    bool prot = (fc1 & 0x40) != 0;
    uint32_t tail = 0;
    if (prot) {
        uint32_t sec = (d0 >> 20) & 7u, swdec = (d0 >> 27) & 1u;
        if (sec == 0 || swdec) { fNetRxDropped++; if (fNetStats) fNetStats->inputErrors++; return false; }
        bool group = (f[4] & 1) != 0;
        tail = ((group ? fGtkCipher : 4) == 2) ? 12u : 8u;
    } else if (fIsEncrypted) {
        return false; // Reject unencrypted data frames on WPA2 sessions
    }

    uint32_t p = hl + (prot ? 8u : 0u);
    if (p + tail >= n) return false;
    uint32_t bodyLen = n - p - tail;

    // A-MSDU De-Aggregation for modern Wi-Fi 5/6/7 routers!
    if (isAmsdu) {
        rtl8188f::AmsduSubframe subs[16];
        uint32_t sc = rtl8188f::splitAmsdu(f + p, bodyLen, subs, 16);
        bool any = false;
        for (uint32_t i = 0; i < sc; ++i) {
            uint32_t pl = subs[i].payloadLen > 1500 ? 1500 : subs[i].payloadLen;
            uint32_t elen = 14u + pl;
            mbuf_t m = allocatePacket(elen);
            if (!m) { fNetRxDropped++; continue; }
            uint8_t *dst = static_cast<uint8_t *>(mbuf_data(m));
            memcpy(dst, subs[i].da, 6);
            memcpy(dst + 6, subs[i].sa, 6);
            dst[12] = static_cast<uint8_t>(subs[i].etherType >> 8);
            dst[13] = static_cast<uint8_t>(subs[i].etherType);
            memcpy(dst + 14, subs[i].payload, pl);
            mbuf_setlen(m, elen); mbuf_pkthdr_setlen(m, elen);
            fNetif->inputPacket(m, elen, IONetworkInterface::kInputOptionQueuePacket);
            fNetRxPackets++; fAmsduSubframes++; if (fNetStats) fNetStats->inputPackets++;
            any = true;
        }
        return any;
    }

    if (bodyLen < 8 || f[p] != 0xaa || f[p + 1] != 0xaa || f[p + 2] != 0x03) return false;
    uint16_t et = (uint16_t)((f[p + 6] << 8) | f[p + 7]);
    if (et == 0x888e) {
        if (fIsEncrypted) handleGtkRekey(f + p + 8, bodyLen - 8);
        return false;
    }

    uint32_t pl = bodyLen - 8u;
    if (pl > 1500) pl = 1500;
    uint32_t elen = 14u + pl;
    mbuf_t m = allocatePacket(elen);
    if (!m) { fNetRxDropped++; if (fNetStats) fNetStats->inputErrors++; return false; }

    // Zero-copy direct write into mbuf_data()!
    uint8_t *dst = static_cast<uint8_t *>(mbuf_data(m));
    memcpy(dst, f + 4, 6);
    memcpy(dst + 6, f + 16, 6);
    dst[12] = static_cast<uint8_t>(et >> 8); dst[13] = static_cast<uint8_t>(et);
    memcpy(dst + 14, f + p + 8, pl);
    mbuf_setlen(m, elen); mbuf_pkthdr_setlen(m, elen);

    // 64-slot A-MPDU sequence reorder window for unicast QoS data
    if (isQos && (f[4] & 1) == 0) {
        uint16_t seq = static_cast<uint16_t>((f[22] | (f[23] << 8)) >> 4);
        bool emitted = false;
        bool kept = fRxReorder.insert(seq, m, [this, &emitted](mbuf_t ready) {
            fNetif->inputPacket(ready, (UInt32)mbuf_pkthdr_len(ready), IONetworkInterface::kInputOptionQueuePacket);
            fNetRxPackets++; if (fNetStats) fNetStats->inputPackets++;
            emitted = true;
        });
        if (!kept) freePacket(m);
        return emitted;
    }

    fNetif->inputPacket(m, elen, IONetworkInterface::kInputOptionQueuePacket);
    fNetRxPackets++; if (fNetStats) fNetStats->inputPackets++;
    return true;
}

void RtlWifiDriver::onWatchdogTick(IOTimerEventSource *sender) {
    // Flush any aged out-of-order RX reorder slots
    if (fRxReorder.storedCount() > 0 && fNetif) {
        fRxReorder.flushAll([this](mbuf_t ready) {
            fNetif->inputPacket(ready, (UInt32)mbuf_pkthdr_len(ready), IONetworkInterface::kInputOptionQueuePacket);
            fNetRxPackets++; if (fNetStats) fNetStats->inputPackets++;
        });
        fNetif->flushInputQueue();
    }

    bool stalled = (fTxBeRing.inflight() >= (int32_t)(rtl8188f::kTxBeRingSize - 2));
    auto act = fWatchdog.tick(fKeysUp, fBeaconSeen, fUsbErrWindow, stalled);
    fBeaconSeen = false; fUsbErrWindow = 0;

    if (fKeysUp) {
        wr8(rtl8188f::kRegOfdm0XaAgc1, fDig.updateFromRssi(fLastRssiDbm));
    }
    if (act == rtl8188f::kRecoveryClearPipeStall) {
        if (fPipeOut3) fPipeOut3->clearStall(false);
        if (fPipeOut2) fPipeOut2->clearStall(false);
        if (fPipeIn)   fPipeIn->clearStall(false);
        fUsbStallRecoveries++;
    } else if (act == rtl8188f::kRecoverySoftReassoc && fTargetSsidLen > 0) {
        stopRx(); fKeysUp = false; connectToCurrentTarget();
    }
    if (sender) sender->setTimeoutMS(2000);
}

// ---------- IOUserClient runtime API (serialized via IOCommandGate!) ----------

IOReturn RtlWifiDriver::ucGetStatus(RtlWifiStatus *out) {
    memset(out, 0, sizeof(*out));
    out->abiVersion     = RTL_WIFI_ABI_VERSION;
    out->state          = fDriverState;
    out->channel        = fApChannel;
    out->rssiDbm        = fLastRssiDbm;
    out->linkSpeedMbps  = fKeysUp ? (fHt40Active ? 150u : 72u) : 0u;
    out->txPackets      = fNetTxPackets;
    out->txErrors       = fNetTxErrors;
    out->rxPackets      = fNetRxPackets;
    out->rxErrors       = fNetRxDropped;
    memcpy(out->macAddr, &fEfuse[0xd7], 6);
    memcpy(out->bssid, fApBssid, 6);
    out->ssidLen        = fTargetSsidLen;
    out->ht40Active     = fHt40Active ? 1 : 0;
    out->linkQualityPct = rtl8188f::RtlDigController::rssiToQualityPct(fLastRssiDbm);
    out->digGain        = fDig.currentGain();
    memcpy(out->ssid, fTargetSsid, fTargetSsidLen);
    return kIOReturnSuccess;
}

IOReturn RtlWifiDriver::ucStartScan() {
    auto fn = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        auto *d = static_cast<RtlWifiDriver *>(owner);
        bool wasRunning = d->fRxRunning;
        if (wasRunning) d->stopRx();
        d->setBssidFilter(false);
        d->fDriverState = kRtlStateScanning;
        d->scanAll();
        d->fDriverState = d->fKeysUp ? kRtlStateConnected : kRtlStateDisconnected;
        if (d->fKeysUp) d->setBssidFilter(true);
        if (wasRunning && d->fKeysUp) d->startRx();
        return kIOReturnSuccess;
    };
    return getCommandGate() ? getCommandGate()->runAction(fn) : fn(this, nullptr, nullptr, nullptr, nullptr);
}

IOReturn RtlWifiDriver::ucGetScanResults(RtlWifiScanList *out) {
    memcpy(out, &fScanList, sizeof(*out));
    return kIOReturnSuccess;
}

IOReturn RtlWifiDriver::ucConnect(const RtlWifiConnectReq *req) {
    auto fn = [](OSObject *owner, void *arg0, void *, void *, void *) -> IOReturn {
        auto *d = static_cast<RtlWifiDriver *>(owner);
        const auto *r = static_cast<const RtlWifiConnectReq *>(arg0);
        d->stopRx();
        d->fKeysUp = false;
        memset(d->fTargetSsid, 0, sizeof(d->fTargetSsid));
        d->fTargetSsidLen = r->ssidLen;
        memcpy(d->fTargetSsid, r->ssid, r->ssidLen);
        d->fHt40Active  = (r->preferHt40 != 0);
        d->fIsEncrypted = (r->security == kRtlSecWpa2Psk);
        if (d->fIsEncrypted && r->passLen >= 8 && r->passLen <= RTL_WIFI_MAX_PASS_LEN) {
            uint8_t pHash[20];
            rtlcrypto::Sha1 s; rtlcrypto::sha1_init(s);
            rtlcrypto::sha1_update(s, reinterpret_cast<const uint8_t *>(r->passphrase), r->passLen);
            rtlcrypto::sha1_final(s, pHash);
            if (!d->fPmkCache.lookup(d->fTargetSsid, d->fTargetSsidLen, pHash, d->fPmk)) {
                char pass[RTL_WIFI_MAX_PASS_LEN + 1];
                memset(pass, 0, sizeof(pass));
                memcpy(pass, r->passphrase, r->passLen);
                rtlcrypto::pbkdf2_pmk(pass, reinterpret_cast<const uint8_t *>(d->fTargetSsid), d->fTargetSsidLen, d->fPmk);
                rtlcrypto::secure_zero(pass, sizeof(pass));
                d->fPmkCache.store(d->fTargetSsid, d->fTargetSsidLen, pHash, d->fPmk);
            }
            d->fHavePmk = true;
        } else {
            d->fHavePmk = false;
        }
        return d->connectToCurrentTarget() ? kIOReturnSuccess : kIOReturnNotReady;
    };
    return getCommandGate()
        ? getCommandGate()->runAction(fn, const_cast<RtlWifiConnectReq *>(req))
        : fn(this, const_cast<RtlWifiConnectReq *>(req), nullptr, nullptr, nullptr);
}

IOReturn RtlWifiDriver::ucDisconnect() {
    auto fn = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        auto *d = static_cast<RtlWifiDriver *>(owner);
        d->stopRx();
        d->fKeysUp = false; d->fHaveAp = false;
        d->fDriverState = kRtlStateDisconnected;
        d->setBssidFilter(false);
        d->sendH2c(rtl8188f::buildH2cMediaStatus(false, 0));
        d->setLinkStatus(kIONetworkLinkValid, 0);
        return kIOReturnSuccess;
    };
    return getCommandGate() ? getCommandGate()->runAction(fn) : fn(this, nullptr, nullptr, nullptr, nullptr);
}

IOReturn RtlWifiDriver::ucGetDiagnostics(RtlWifiDiagnostics *out) {
    memset(out, 0, sizeof(*out));
    out->abiVersion         = RTL_WIFI_ABI_VERSION;
    out->txRingInflightBe   = (uint32_t)fTxBeRing.inflight();
    out->txRingInflightVo   = (uint32_t)fTxHiRing.inflight();
    out->rxRingInflight     = (uint32_t)fRxInflight;
    out->watchdogResets     = fWatchdog.resetCount();
    out->gtkRekeyCount      = fGtkRekeyCount;
    out->usbStallRecoveries = fUsbStallRecoveries;
    out->thermalDelta       = fThermal.lastDelta();
    out->amsduSubframes     = fAmsduSubframes;
    out->reorderFlushes     = fRxReorder.flushCount();
    out->pmkCacheHits       = fPmkCache.hits();
    out->roamCount          = fRoamCount;
    return kIOReturnSuccess;
}

bool RtlWifiDriver::willTerminate(IOService *provider, IOOptionBits options) {
    fNetifEnabled = false;
    fTxBeRing.setStopped(true); fTxHiRing.setStopped(true);
    stopRx();
    return super::willTerminate(provider, options);
}

bool RtlWifiDriver::didTerminate(IOService *provider, IOOptionBits options, bool *defer) {
    return super::didTerminate(provider, options, defer);
}

void RtlWifiDriver::stop(IOService *provider) {
    fNetifEnabled = false;
    PMstop();
    if (fWatchdogTimer) {
        fWatchdogTimer->cancelTimeout();
        if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fWatchdogTimer);
        fWatchdogTimer->release(); fWatchdogTimer = nullptr;
    }
    fTxBeRing.setStopped(true); fTxHiRing.setStopped(true);
    if (fPipeOut3) fPipeOut3->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
    if (fPipeOut2) fPipeOut2->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
    stopRx();
    for (uint32_t i = 0; i < rtl8188f::kRxRingSize; i++) {
        if (fRxMd[i]) { fRxMd[i]->release(); fRxMd[i] = nullptr; }
    }
    fTxBeRing.destroy(); fTxHiRing.destroy();
    fPmkCache.clear();
    if (fTxFrame)   { IOFree(fTxFrame, 1600); fTxFrame = nullptr; }
    if (fPipeIn)    { fPipeIn->release();    fPipeIn = nullptr; }
    if (fPipeOut2)  { fPipeOut2->release();  fPipeOut2 = nullptr; }
    if (fPipeOut3)  { fPipeOut3->release();  fPipeOut3 = nullptr; }
    if (fNetif)     { fNetif->release();     fNetif = nullptr; }
    if (fInterface) { fInterface->close(this); fInterface = nullptr; }
    IOLog("RtlWifi: V28 Ultimate stopped cleanly\n");
    super::stop(provider);
}
