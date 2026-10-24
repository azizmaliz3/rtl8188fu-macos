#include "RtlWifiDriver.hpp"
#include "rtl8188fufw.h"          // generated: rtl8188fufw_bin[], rtl8188fufw_bin_len
#include "RtlWifiTables.h"         // generated: kRtl8188fMacTable[]
#include "RtlWifiPhyTables.h"      // generated: PHY, AGC and RF tables
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
    uint8_t aggCtrl = rd8(REG_TRXDMA_CTRL);
    aggCtrl &= ~BIT(2);                                // TRXDMA_CTRL_RXDMA_AGG_EN
    uint32_t aggRx = rd32(0x0280);                     // REG_RXDMA_AGG_PG_TH
    aggRx &= ~BIT(31);
    aggRx &= ~0xFF0Fu;
    uint8_t rxdmaMode = rd8(0x0290);
    rxdmaMode &= ~BIT(1);
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

// ---------- lifecycle ----------

bool RtlWifiDriver::start(IOService *provider) {
    fInterface = nullptr;
    fPipeIn = fPipeOut2 = fPipeOut3 = nullptr;
    if (!super::start(provider)) return false;
    setProperty("start-called", kOSBooleanTrue);
    setProperty("build-tag", "v12");
    fInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!fInterface) { IOLog("RtlWifi: provider is not IOUSBHostInterface\n"); return false; }
    if (!fInterface->open(this)) { IOLog("RtlWifi: open failed\n"); fInterface = nullptr; return false; }

    // Bulk pipes from the configuration descriptor: IN 0x81, OUT 0x02, OUT 0x03
    fPipeIn   = fInterface->copyPipe(0x81);
    fPipeOut2 = fInterface->copyPipe(0x02);
    fPipeOut3 = fInterface->copyPipe(0x03);
    setProperty("pipe-in-0x81", fPipeIn ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("pipe-out-0x02", fPipeOut2 ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("pipe-out-0x03", fPipeOut3 ? kOSBooleanTrue : kOSBooleanFalse);

    uint32_t sysCfg = rd32(0x00F0);
    fChipCut = (uint8_t)((sysCfg & 0xf000) >> 12);   // SYS_CFG_CHIP_VERSION_MASK
    setProperty("sys-cfg", (uint64_t)sysCfg, 32);
    setProperty("chip-cut", (uint64_t)fChipCut, 8);

    bool chainOk = false;
    IOReturn r = powerOn();
    setProperty("power-on-status", (uint64_t)(uint32_t)r, 32);
    setProperty("fsmco-after-power-on", (uint64_t)rd32(REG_APS_FSMCO), 32);
    if (r != kIOReturnSuccess) return true;   // stay attached so properties can be read

    // Queue setup, in the reference order (before the firmware download)
    configEndpoints((fPipeOut2 ? 1 : 0) + (fPipeOut3 ? 1 : 0));
    setProperty("ep-tx-count", (uint64_t)fEpTxCount, 8);
    initQueueReservedPage();
    r = initQueuePriority();
    setProperty("queue-priority-status", (uint64_t)(uint32_t)r, 32);
    setProperty("rqpn-npq-readback", (uint64_t)rd32(REG_RQPN_NPQ), 32);
    setProperty("rqpn-readback", (uint64_t)rd32(REG_RQPN), 32);
    setProperty("trxdma-ctrl-readback", (uint64_t)rd16(REG_TRXDMA_CTRL), 16);

    wr16(REG_TRXFF_BNDY + 2, 0x3f7f);         // rtl8188fu_fops.trxff_boundary
    setProperty("trxff-bndy-hi-readback", (uint64_t)rd16(REG_TRXFF_BNDY + 2), 16);

    r = downloadFirmware();
    setProperty("fw-dl-status", (uint64_t)(uint32_t)r, 32);
    setProperty("mcu-fw-dl-reg-after-dl", (uint64_t)rd32(REG_MCU_FW_DL), 32);
    if (r != kIOReturnSuccess) return true;

    r = startFirmware();
    setProperty("fw-start-status", (uint64_t)(uint32_t)r, 32);
    setProperty("mcu-fw-dl-reg-final", (uint64_t)rd32(REG_MCU_FW_DL), 32);

    if (r == kIOReturnSuccess) {
        r = initMac();
        setProperty("init-mac-status", (uint64_t)(uint32_t)r, 32);
        if (r == kIOReturnSuccess) {
            r = initPhyBb();
            setProperty("init-phy-bb-status", (uint64_t)(uint32_t)r, 32);
            if (r == kIOReturnSuccess) {
                r = initPhyRf();
                setProperty("init-phy-rf-status", (uint64_t)(uint32_t)r, 32);
                if (r == kIOReturnSuccess) {
                    r = postRfInit();
                    setProperty("post-rf-init-status", (uint64_t)(uint32_t)r, 32);
                    if (r == kIOReturnSuccess) {
                        r = initMacTail();
                        setProperty("mac-tail-status", (uint64_t)(uint32_t)r, 32);
                        chainOk = (r == kIOReturnSuccess);
                    }
                }
            }
        }
    }

    r = readEfuse();
    bool efuseOk = (r == kIOReturnSuccess);
    setProperty("efuse-status", (uint64_t)(uint32_t)r, 32);
    setProperty("efuse-map", fEfuse, sizeof(fEfuse));
    uint16_t rtlId = (uint16_t)(fEfuse[0] | (fEfuse[1] << 8));
    setProperty("efuse-rtl-id", (uint64_t)rtlId, 16);      // expected 0x8129 = 33065
    char mac[20];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             fEfuse[0xd7], fEfuse[0xd8], fEfuse[0xd9],
             fEfuse[0xda], fEfuse[0xdb], fEfuse[0xdc]);
    setProperty("mac-address-str", mac);

    if (chainOk && efuseOk && rtlId == 0x8129) {
        r = initCalib();
        setProperty("calib-status", (uint64_t)(uint32_t)r, 32);
        if (r == kIOReturnSuccess) {
            initStatistics();
            r = setChannel20(6);
            setProperty("set-channel-status", (uint64_t)(uint32_t)r, 32);
        }
    }
    return true;
}

bool RtlWifiDriver::willTerminate(IOService *provider, IOOptionBits options) {
    setProperty("will-terminate", (uint64_t)options, 32);
    return super::willTerminate(provider, options);
}

bool RtlWifiDriver::didTerminate(IOService *provider, IOOptionBits options, bool *defer) {
    setProperty("did-terminate", (uint64_t)options, 32);
    return super::didTerminate(provider, options, defer);
}

void RtlWifiDriver::stop(IOService *provider) {
    setProperty("stop-called", kOSBooleanTrue);
    if (fPipeIn)   { fPipeIn->release();   fPipeIn = nullptr; }
    if (fPipeOut2) { fPipeOut2->release(); fPipeOut2 = nullptr; }
    if (fPipeOut3) { fPipeOut3->release(); fPipeOut3 = nullptr; }
    if (fInterface) { fInterface->close(this); fInterface = nullptr; }
    IOLog("RtlWifi: stopped\n");
    super::stop(provider);
}
