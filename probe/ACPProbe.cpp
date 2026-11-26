/*
 * ACPProbe.kext —— Phase 0 探路驱动（分级只读）
 *
 * 目的：在 macOS 里确认 ACP（pci1022,15e2）能不能被我们的 kext 拿住、
 *       BAR0 是否可映射、WOV/PDM 寄存器块是否存在。
 *
 * ★ 分级设计（PROBE_STAGE）★
 *   上一版一开机就关机了。最可能的原因：start() 里对 0x00000–0x40000 整窗
 *   做了 65536 次 MMIO 读，ACP 未上电或窗口不响应时，某些平台会直接
 *   机器检查 / 断电，而不是简单返回 0xFFFFFFFF。
 *   所以现在把动作切成 4 级，逐级验证：
 *
 *   PROBE_STAGE 0  完全不碰 MMIO（默认）——只读 PCI 配置空间 + BAR 元数据
 *   PROBE_STAGE 1  只读 2 个寄存器：ACP_PGFSM_STATUS(0x1028)、ACP_PIN_CONFIG(0x1440)
 *   PROBE_STAGE 2  读寄存器表（26 个，见 dumpTable）
 *   PROBE_STAGE 3  表 + 全窗扫描（最危险，64K 次读）
 *
 * 每一级都能单独上机验证；只要某一级又关机，就把数字减 1。
 *
 * 构建：./build.sh（会自动编译 kmod_info.cpp 提供 _kmod_info）
 * 安装：../tools/install-probe.sh install
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "acp63_regs.h"

/* 0 = 只读元数据；1 = 两个寄存器；2 = 寄存器表；3 = 表 + 全窗扫描 */
#ifndef PROBE_STAGE
#define PROBE_STAGE 0
#endif

#define LOG(fmt, ...) IOLog("ACPProbe: " fmt "\n", ##__VA_ARGS__)

/* 把结果同时写进 IORegistry，这样即使 dmesg 拿不到日志，
   也能用 ioreg -rc ACPProbe -l 读到全部数值（不需要 root）。 */
#define PUB32(key, val)  this->setProperty(key, (unsigned long long)(val), 32)
#define PUB64(key, val)  this->setProperty(key, (unsigned long long)(val), 64)

class ACPProbe : public IOService
{
    OSDeclareDefaultStructors(ACPProbe)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    IOPCIDevice    *fPci  = nullptr;
    IOMemoryMap    *fMap  = nullptr;
    volatile UInt8 *fBase = nullptr;
    IOByteCount     fLen  = 0;

    UInt32 rd32(UInt32 off) const;
    void   dumpTable();
    void   scanWindow();
};

OSDefineMetaClassAndStructors(ACPProbe, IOService)

UInt32 ACPProbe::rd32(UInt32 off) const
{
    if (!fBase || off + 4 > fLen) return 0xDEADBEEF;
    return *(volatile UInt32 *)(fBase + off);
}

void ACPProbe::dumpTable()
{
    struct { const char *name; UInt32 off; } table[] = {
        { "ACP_SOFT_RESET",        ACP_SOFT_RESET        },
        { "ACP_CONTROL",           ACP_CONTROL           },
        { "ACP_STATUS",            ACP_STATUS            },
        { "ACP_PGFSM_CONTROL",     ACP_PGFSM_CONTROL     },
        { "ACP_PGFSM_STATUS",      ACP_PGFSM_STATUS      },
        { "ATU_PAGE_SIZE_GRP_1",   ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1 },
        { "ATU_BASE_ADDR_GRP_1",   ACPAXI2AXI_ATU_BASE_ADDR_GRP_1 },
        { "ACP_PIN_CONFIG",        ACP_PIN_CONFIG        },
        { "EXT_INTR_CNTL",         ACP_EXTERNAL_INTR_CNTL },
        { "EXT_INTR_STAT",         ACP_EXTERNAL_INTR_STAT },
        { "SCRATCH_REG_0",         ACP_SCRATCH_REG_0     },
        { "WOV_PDM_ENABLE",        ACP_WOV_PDM_ENABLE    },
        { "WOV_PDM_DMA_ENABLE",    ACP_WOV_PDM_DMA_ENABLE },
        { "WOV_RX_RINGBUFADDR",    ACP_WOV_RX_RINGBUFADDR },
        { "WOV_RX_RINGBUFSIZE",    ACP_WOV_RX_RINGBUFSIZE },
        { "WOV_RX_LINKPOSITION",   ACP_WOV_RX_LINKPOSITIONCNTR },
        { "WOV_RX_LINEAR_HI",      ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH },
        { "WOV_RX_LINEAR_LO",      ACP_WOV_RX_LINEARPOSITIONCNTR_LOW },
        { "WOV_RX_WATERMARK",      ACP_WOV_RX_INTR_WATERMARK_SIZE },
        { "WOV_PDM_FIFO_FLUSH",    ACP_WOV_PDM_FIFO_FLUSH },
        { "WOV_PDM_CHANNELS",      ACP_WOV_PDM_NO_OF_CHANNELS },
        { "WOV_PDM_DECIMATION",    ACP_WOV_PDM_DECIMATION_FACTOR },
        { "WOV_MISC_CTRL",         ACP_WOV_MISC_CTRL     },
        { "WOV_CLK_CTRL",          ACP_WOV_CLK_CTRL      },
        { "WOV_ERROR_STATUS",      ACP_WOV_ERROR_STATUS_REGISTER },
        { "PDM_CLKDIV",            ACP_PDM_CLKDIV        },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        UInt32 v = rd32(table[i].off);
        char key[64];
        snprintf(key, sizeof(key), "reg_%s", table[i].name);
        setProperty(key, (unsigned long long)v, 32);
        LOG("  [0x%05X] %-22s = 0x%08X", table[i].off, table[i].name, v);
    }
}

void ACPProbe::scanWindow()
{
    UInt32 nonzero = 0, allff = 0;
    UInt32 firstOff = 0, lastOff = 0;

    for (UInt32 off = 0; off + 4 <= fLen; off += 4) {
        UInt32 v = rd32(off);
        if (v == 0xFFFFFFFF) { allff++; continue; }
        if (v == 0) continue;
        nonzero++;
        if (!firstOff) firstOff = off;
        lastOff = off;
    }
    LOG("scan 0..0x%X: nonzero=%u  all-FF=%u  first=0x%X last=0x%X",
        (unsigned)fLen, (unsigned)nonzero, (unsigned)allff,
        (unsigned)firstOff, (unsigned)lastOff);
}

bool ACPProbe::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) { LOG("provider is not IOPCIDevice"); return false; }

    LOG("stage=%d  vendor=0x%04X device=0x%04X rev=0x%02X",
        PROBE_STAGE,
        (unsigned)fPci->configRead16(kIOPCIConfigVendorID),
        (unsigned)fPci->configRead16(kIOPCIConfigDeviceID),
        (unsigned)fPci->configRead8(kIOPCIConfigRevisionID));

    LOG("classcode=0x%06X  command=0x%04X",
        (unsigned)(fPci->configRead32(0x08) >> 8),
        (unsigned)fPci->configRead16(kIOPCIConfigCommand));

    IODeviceMemory *mem = fPci->getDeviceMemoryWithIndex(0);
    if (!mem) {
        LOG("no MMIO BAR0 -- 设备没有 memory BAR，后面的阶段都做不了");
        return false;
    }

    fLen = mem->getLength();
    LOG("BAR0 phys=0x%llX len=0x%llX",
        (unsigned long long)mem->getPhysicalAddress(),
        (unsigned long long)fLen);

#if PROBE_STAGE >= 1
    /* 从这里开始才碰 MMIO。先显式打开 memory decode。 */
    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);

    fMap = fPci->mapDeviceMemoryWithIndex(0, kIOMapAnywhere);
    if (!fMap) { LOG("mapDeviceMemoryWithIndex failed -- 停在这里"); return true; }
    fBase = (volatile UInt8 *)fMap->getVirtualAddress();
    LOG("mmap virt=%p", (void *)fBase);

    /* 先读 PIN_CONFIG（Linux 在 probe 阶段就读它，风险最低），
       立刻发布，再读 PGFSM_STATUS。万一第二次读触发异常断电，
       至少第一条已经落到 IORegistry 里。 */
    UInt32 pincfg = rd32(ACP_PIN_CONFIG);
    PUB32("pinConfig", pincfg);
    LOG("PIN_CONFIG   = 0x%08X  (PDM 模式时通常不在 {0,1,2,3,9,15})", pincfg);

    UInt32 pgfsm = rd32(ACP_PGFSM_STATUS);
    PUB32("pgfsmStatus", pgfsm);
    LOG("PGFSM_STATUS = 0x%08X  (0x%X=on, 0x%X=off)",
        pgfsm, (unsigned)ACP_POWERED_ON, (unsigned)ACP_POWERED_OFF);
#endif

#if PROBE_STAGE >= 2
    dumpTable();
#endif

#if PROBE_STAGE >= 3
    scanWindow();
#endif

    setProperty("probeVersion", "0.1.1");
    PUB32("stage", PROBE_STAGE);
    PUB32("vendor", fPci->configRead16(kIOPCIConfigVendorID));
    PUB32("device", fPci->configRead16(kIOPCIConfigDeviceID));
    PUB32("revision", fPci->configRead8(kIOPCIConfigRevisionID));
    PUB32("classcode", fPci->configRead32(0x08) >> 8);
    PUB64("barPhys", mem->getPhysicalAddress());
    PUB64("barLen", fLen);

    LOG("stage %d done", PROBE_STAGE);
    return true;
}

void ACPProbe::stop(IOService *provider)
{
    if (fMap) { fMap->release(); fMap = nullptr; fBase = nullptr; }
    fPci = nullptr;
    IOService::stop(provider);
}
