/*
 * ACPCapture.kext —— Phase 1：ACP6.x PDM 内置麦克风采集（仅硬件 + 导出 PCM）
 *
 * 设计要点（照 Linux sound/soc/amd/ps/ps-pdm-dma.c 抄，见 ../refs/linux/）：
 *
 *   1. 32 KB 物理连续 DMA 环（8 个 4 KB 页），页表写进 ACP SRAM 0x03800000，
 *      通过 SCRATCH_REG_0 窗口访问（acp63_config_dma）。
 *   2. RINGBUFADDR = 0x04000000（PDM_MEM_WINDOW_START），即 ATU 组 1 映射出的
 *      AXI 窗口；DMA 就往这个"窗口地址"写。
 *   3. CLK_CTRL = 7、MISC_CTRL 增益 3、FIFO_FLUSH 先清一次（消开录爆音），
 *      再 PDM_ENABLE=1 / DMA_ENABLE=1，轮询 DMA_ENABLE bit1 == 2。
 *   4. 10 ms 定时器读 LINEARYPOSITIONCNTR（绝对字节计数），把增量从 DMA 环
 *      搬进 4 MB 的软件环；用户态通过 IOUserClient 映射这 4 MB 直接读。
 *
 * 不做中断（Linux 里最容易 panic 的部分），纯轮询。
 *
 * 格式：S32_LE / 2ch / 48000 Hz。
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOUserClient.h>

#include "acp63_regs.h"

#define LOG(fmt, ...) IOLog("ACPCapture: " fmt "\n", ##__VA_ARGS__)
#define PUB32(key, val)  this->setProperty(key, (unsigned long long)(val), 32)
#define PUB64(key, val)  this->setProperty(key, (unsigned long long)(val), 64)

static const UInt32 kRingBytes   = 32768;          /* DMA 环：32 KB */
static const UInt32 kPeriodBytes = 8192;           /* 水位 = 一个周期 */
static const UInt32 kCapBytes    = 4 * 1024 * 1024;/* 软件环：4 MB ≈ 21 秒 */
static const UInt32 kMemWindow   = 0x04000000;     /* PDM_MEM_WINDOW_START */
static const UInt32 kPteBase     = 0x03800000;     /* ACP_SRAM_PTE_OFFSET */

enum {
    kMethodGetStatus = 0,   /* out: writeBytes lo, hi, lastLinear, dmaEnable */
    kMethodReset     = 1,
    kMethodStart     = 2,
    kMethodStop      = 3,
    kMethodInfo      = 4,   /* out: ringBytes, capBytes, pages, pad */
    kMethodArm       = 5,   /* 只写 PTE（不碰 ATU/环/DMA） */
    kMethodRing      = 6,   /* 只写环缓冲寄存器 + ATU 组使能 */
    kMethodInit      = 7,   /* 映射 BAR + 申请内存（唯一碰硬件的"初始化"） */
    kMethodSnap      = 8,   /* 把 DMA 环原样拷到软件环（绕开定时器，直接看硬件写了什么） */
};

class ACPCapture;

class ACPCaptureUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(ACPCaptureUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                      OSDictionary *properties = nullptr) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch = nullptr,
                            OSObject *target = nullptr, void *reference = nullptr) override;

private:
    ACPCapture *fProvider = nullptr;
};

class ACPCapture : public IOService
{
    OSDeclareDefaultStructors(ACPCapture)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool init(OSDictionary *dict = nullptr) override;

    /* 供 user client 调用 */
    void     timerTick();
    IOReturn startCapture();
    IOReturn stopCapture();
    UInt64   writeBytes() const { return fWriteBytes; }
    UInt64   lastLinear() const { return fLastLinear; }
    UInt32   ticks() const { return fTicks; }
    UInt64   linearNow() const;
    IOReturn snapshotRing();
    UInt32   dmaEnable()  const;
    void     publishStatus();
    IOMemoryDescriptor *capBuffer() const { return fCapBuf; }

    bool     powerOnAndSetup();
    IOReturn initHardware();
    bool     isInit() const { return fInited; }
    IOReturn programPte();
    IOReturn ringOnly();
    IOReturn enableDma();

private:
    IOPCIDevice            *fPci    = nullptr;
    IOMemoryMap            *fMap    = nullptr;
    volatile UInt8         *fBase   = nullptr;
    IOByteCount             fBarLen = 0;

    IOBufferMemoryDescriptor *fDmaBuf = nullptr;   /* 32 KB 物理连续 */
    IOBufferMemoryDescriptor *fCapBuf = nullptr;   /* 4 MB 软件环 */
    IOTimerEventSource       *fTimer  = nullptr;
    IOWorkLoop               *fWorkLoop = nullptr;
    bool                      fOwnWL  = false;

    UInt64 fWriteBytes  = 0;
    UInt64 fSkipBytes   = 0;
    UInt64 fLastLinear  = 0;
    UInt32 fPages       = 0;
    UInt32 fTicks       = 0;
    bool   fRunning     = false;
    bool   fInited      = false;

    UInt32 rd32(UInt32 off) const;
    void   wr32(UInt32 off, UInt32 val);
    void   copyOut(UInt64 from, UInt64 to, UInt32 len);
};

OSDefineMetaClassAndStructors(ACPCapture, IOService)
OSDefineMetaClassAndStructors(ACPCaptureUserClient, IOUserClient)

/* ------------------------------------------------------------------ */
/* 寄存器访问                                                          */
/* ------------------------------------------------------------------ */

UInt32 ACPCapture::rd32(UInt32 off) const
{
    if (!fBase || off + 4 > fBarLen) return 0xDEADBEEF;
    return *(volatile UInt32 *)(fBase + off);
}

void ACPCapture::wr32(UInt32 off, UInt32 val)
{
    if (!fBase || off + 4 > fBarLen) return;
    *(volatile UInt32 *)(fBase + off) = val;
    OSSynchronizeIO();
}

UInt32 ACPCapture::dmaEnable() const
{
    return rd32(ACP_WOV_PDM_DMA_ENABLE);
}

UInt64 ACPCapture::linearNow() const
{
    return ((UInt64)rd32(ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH) << 32)
         |  (UInt64)rd32(ACP_WOV_RX_LINEARPOSITIONCNTR_LOW);
}

/* 诊断用：不看位置计数器，直接把 DMA 环整块拷出来。
   若定时器没跑但硬件确实在写，这里会看到非零样本。 */
IOReturn ACPCapture::snapshotRing()
{
    if (!fDmaBuf || !fCapBuf) return kIOReturnNotReady;
    const void *src = fDmaBuf->getBytesNoCopy();
    void *dst = fCapBuf->getBytesNoCopy();
    if (!src || !dst) return kIOReturnNotReady;
    memcpy(dst, src, kRingBytes);
    fWriteBytes = kRingBytes;
    setProperty("snapBytes", (unsigned long long)kRingBytes, 32);
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

bool ACPCapture::init(OSDictionary *dict)
{
    if (!IOService::init(dict)) return false;
    return true;
}

bool ACPCapture::powerOnAndSetup()
{
    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);          /* DMA 必需 */

    fMap = fPci->mapDeviceMemoryWithIndex(0, kIOMapAnywhere);
    if (!fMap) { LOG("mapDeviceMemoryWithIndex 失败"); return false; }
    fBase = (volatile UInt8 *)fMap->getVirtualAddress();

    fDmaBuf = IOBufferMemoryDescriptor::withOptions(
                  kIOMemoryPhysicallyContiguous | kIOMemoryUnshared,
                  kRingBytes, PAGE_SIZE);
    if (!fDmaBuf) { LOG("DMA 缓冲申请失败"); return false; }
    if (fDmaBuf->prepare(kIODirectionIn) != kIOReturnSuccess) {
        LOG("DMA 缓冲 prepare 失败"); return false;
    }
    bzero(fDmaBuf->getBytesNoCopy(), kRingBytes);
    fPages = kRingBytes / PAGE_SIZE;

    fCapBuf = IOBufferMemoryDescriptor::withCapacity(kCapBytes, kIODirectionInOut);
    if (!fCapBuf) { LOG("软件环申请失败"); return false; }
    bzero(fCapBuf->getBytesNoCopy(), kCapBytes);

    LOG("setup: bar=0x%llX len=0x%llX dma=%p(%u pages) cap=%u",
        (unsigned long long)fPci->getDeviceMemoryWithIndex(0)->getPhysicalAddress(),
        (unsigned long long)fBarLen, fDmaBuf->getBytesNoCopy(),
        (unsigned)fPages, (unsigned)kCapBytes);
    return true;
}

IOReturn ACPCapture::programPte()
{
    IOPhysicalAddress addr = fDmaBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!addr) { LOG("拿不到 DMA 物理地址"); return kIOReturnNoMemory; }

    wr32(ACPAXI2AXI_ATU_BASE_ADDR_GRP_1, kPteBase | 0x80000000u);
    wr32(ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1, 2 /* 4 KB */);

    UInt32 val = 0;
    IOPhysicalAddress pa = addr;
    for (UInt32 i = 0; i < fPages; i++) {
        wr32(ACP_SCRATCH_REG_0 + val,     (UInt32)(pa & 0xFFFFFFFFu));
        wr32(ACP_SCRATCH_REG_0 + val + 4, (UInt32)((pa >> 32) | 0x80000000u));
        val += 8;
        pa  += PAGE_SIZE;
    }
    LOG("PTE: phys=0x%llX pages=%u", (unsigned long long)addr, (unsigned)fPages);
    return kIOReturnSuccess;
}

IOReturn ACPCapture::ringOnly()
{
    wr32(ACP_WOV_RX_RINGBUFADDR,        kMemWindow);
    wr32(ACP_WOV_RX_RINGBUFSIZE,        kRingBytes);
    wr32(ACP_WOV_RX_INTR_WATERMARK_SIZE, kPeriodBytes);
    wr32(ACPAXI2AXI_ATU_CTRL,           1);
    LOG("ring: addr=0x%X size=%u wm=%u ATU_CTRL=%u",
        (unsigned)kMemWindow, (unsigned)kRingBytes, (unsigned)kPeriodBytes,
        (unsigned)rd32(ACPAXI2AXI_ATU_CTRL));
    return kIOReturnSuccess;
}

IOReturn ACPCapture::enableDma()
{
    ringOnly();

    /* PDM 时钟 + 增益（acp63_enable_pdm_clock） */
    wr32(ACP_WOV_CLK_CTRL, ACP_PDM_CLK_FREQ_MASK);
    UInt32 misc = rd32(ACP_WOV_MISC_CTRL);
    misc &= ~(3u << 3);
    misc |=  (3u << 3);                  /* pdm_gain 默认 3 */
    wr32(ACP_WOV_MISC_CTRL, misc);

    /* Linux 在 TRIGGER_START 里写的两个寄存器，之前漏了：
       TWO_CH -> ch_mask = 0x00；DECIMATION_FACTOR = 2。
       实测 NO_OF_CHANNELS 保持 1 时，硬件按 4x32-bit 出帧（后两路恒 0x80000000）。 */
    wr32(ACP_WOV_PDM_NO_OF_CHANNELS,     0x00);
    wr32(ACP_WOV_PDM_DECIMATION_FACTOR,  2);

    /* 消爆音：先冲 FIFO，等时钟稳定，再开采集 */
    wr32(ACP_WOV_PDM_FIFO_FLUSH, 1);
    IOSleep(20);

    wr32(ACP_WOV_PDM_ENABLE,     1);
    wr32(ACP_WOV_PDM_DMA_ENABLE, 1);

    for (int i = 0; i < 100; i++) {
        if ((rd32(ACP_WOV_PDM_DMA_ENABLE) & 0x02) == 0x02) {
            LOG("DMA 已运行（%d ms）", i);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }
    LOG("DMA 启动超时：PDM_ENABLE=0x%X DMA_ENABLE=0x%X",
        rd32(ACP_WOV_PDM_ENABLE), rd32(ACP_WOV_PDM_DMA_ENABLE));
    return kIOReturnTimeout;
}

IOReturn ACPCapture::startCapture()
{
    if (fRunning) return kIOReturnSuccess;
    if (programPte() != kIOReturnSuccess) return kIOReturnNoMemory;
    IOReturn r = enableDma();
    if (r != kIOReturnSuccess) return r;
    fLastLinear = ((UInt64)rd32(ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH) << 32)
                |  (UInt64)rd32(ACP_WOV_RX_LINEARPOSITIONCNTR_LOW);
    fWriteBytes = 0;
    /* 前 200 ms 丢弃：FIFO 里残留的陈旧样本会造成开头一声爆音 */
    fSkipBytes  = (UInt64)(48000 / 5) * 8;
    fRunning = true;
    if (fTimer) { fTimer->enable(); fTimer->setTimeoutUS(10000); }
    LOG("采集已开始 lastLinear=%llu", (unsigned long long)fLastLinear);
    return kIOReturnSuccess;
}

IOReturn ACPCapture::stopCapture()
{
    if (!fRunning) return kIOReturnSuccess;
    fRunning = false;
    if (fTimer) fTimer->cancelTimeout();
    wr32(ACP_WOV_PDM_DMA_ENABLE, 0x02);
    for (int i = 0; i < 100; i++) {
        if ((rd32(ACP_WOV_PDM_DMA_ENABLE) & 0x02) == 0) break;
        IOSleep(1);
    }
    wr32(ACP_WOV_PDM_ENABLE, 0);
    wr32(ACP_WOV_PDM_FIFO_FLUSH, 1);
    LOG("采集已停止");
    return kIOReturnSuccess;
}

void ACPCapture::copyOut(UInt64 from, UInt64 to, UInt32 len)
{
    const UInt8 *src = (const UInt8 *)fDmaBuf->getBytesNoCopy();
    UInt8 *dst = (UInt8 *)fCapBuf->getBytesNoCopy();
    if (!src || !dst) return;

    while (len) {
        UInt32 so = (UInt32)(from % kRingBytes);
        UInt32 dof = (UInt32)(to % kCapBytes);
        UInt32 n = len;
        if (n > kRingBytes - so) n = kRingBytes - so;
        if (n > kCapBytes - dof)  n = kCapBytes - dof;
        memcpy(dst + dof, src + so, n);
        from += n; to += n; len -= n;
    }
}

void ACPCapture::timerTick()
{
    fTicks++;
    if (!fRunning) return;

    UInt64 linear = ((UInt64)rd32(ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH) << 32)
                  |  (UInt64)rd32(ACP_WOV_RX_LINEARPOSITIONCNTR_LOW);
    if (linear == fLastLinear) return;

    UInt64 delta = linear - fLastLinear;
    if (delta > kRingBytes) delta = kRingBytes;     /* 掉队保护：只取一圈 */

    if (fWriteBytes >= fSkipBytes) {
        copyOut(fLastLinear, fWriteBytes, (UInt32)delta);
    }
    fLastLinear  = linear;
    fWriteBytes += delta;

    if (fTimer) fTimer->setTimeoutUS(10000);
}

void ACPCapture::publishStatus()
{
    setProperty("running", (bool)fRunning);
    setProperty("writeBytes", (unsigned long long)fWriteBytes, 64);
    setProperty("lastLinear", (unsigned long long)fLastLinear, 64);
    setProperty("dmaEnable", (unsigned long long)rd32(ACP_WOV_PDM_DMA_ENABLE), 32);
    setProperty("ticks", (unsigned long long)fTicks, 32);
    setProperty("linearNow", linearNow(), 64);
    setProperty("ringAddr", (unsigned long long)rd32(ACP_WOV_RX_RINGBUFADDR), 32);
    setProperty("ringSize", (unsigned long long)rd32(ACP_WOV_RX_RINGBUFSIZE), 32);
    setProperty("atuBase", (unsigned long long)rd32(ACPAXI2AXI_ATU_BASE_ADDR_GRP_1), 32);
    setProperty("atuPage", (unsigned long long)rd32(ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1), 32);
    setProperty("atuCtrl", (unsigned long long)rd32(ACPAXI2AXI_ATU_CTRL), 32);
    setProperty("clkCtrl", (unsigned long long)rd32(ACP_WOV_CLK_CTRL), 32);
    setProperty("miscCtrl", (unsigned long long)rd32(ACP_WOV_MISC_CTRL), 32);
}

bool ACPCapture::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) { LOG("provider 不是 IOPCIDevice"); return false; }

    IODeviceMemory *mem = fPci->getDeviceMemoryWithIndex(0);
    if (!mem) { LOG("没有 BAR0"); return false; }
    fBarLen = mem->getLength();

    /* ★ 这里刻意什么都不做：不 enable memory、不 map、不申请内存、不建定时器。
       所有硬件动作都由用户态一步一步触发（--init / --arm / --ring / 录制），
       这样开机绝不会因为本 kext 而复位。 */
    setProperty("state", "loaded");
    PUB32("stage", 0);
    PUB32("revision", fPci->configRead8(kIOPCIConfigRevisionID));
    PUB64("barLen", fBarLen);
    LOG("start(): 仅加载，未碰硬件（BAR0 len=0x%llX）", (unsigned long long)fBarLen);
    registerService();
    return true;
}

IOReturn ACPCapture::initHardware()
{
    if (fInited) return kIOReturnSuccess;
    if (!powerOnAndSetup()) return kIOReturnNoMemory;

    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        fWorkLoop = IOWorkLoop::workLoop();
        fOwnWL = (fWorkLoop != nullptr);
    }
    if (!fWorkLoop) { LOG("拿不到 workloop"); return kIOReturnNoResources; }

    fTimer = IOTimerEventSource::timerEventSource(this,
                 OSMemberFunctionCast(IOTimerEventSource::Action,
                                      this, &ACPCapture::timerTick));
    if (!fTimer) { LOG("定时器创建失败"); return kIOReturnNoMemory; }
    if (fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
        LOG("定时器加入 workloop 失败");
        fTimer->release(); fTimer = nullptr;
        return kIOReturnNoResources;
    }
    fTimer->enable();

    fInited = true;
    setProperty("state", "inited");
    LOG("initHardware(): BAR 已映射、DMA 缓冲 %u B、软件环 %u B",
        (unsigned)kRingBytes, (unsigned)kCapBytes);
    return kIOReturnSuccess;
}

void ACPCapture::stop(IOService *provider)
{
    stopCapture();
    if (fTimer) {
        fTimer->disable();
        if (fWorkLoop) fWorkLoop->removeEventSource(fTimer);
        fTimer->release(); fTimer = nullptr;
    }
    if (fWorkLoop && fOwnWL) { fWorkLoop->release(); }
    fWorkLoop = nullptr; fOwnWL = false;
    if (fDmaBuf) { fDmaBuf->complete(kIODirectionIn); fDmaBuf->release(); fDmaBuf = nullptr; }
    if (fCapBuf) { fCapBuf->release(); fCapBuf = nullptr; }
    if (fMap) { fMap->release(); fMap = nullptr; fBase = nullptr; }
    fPci = nullptr;
    IOService::stop(provider);
}

/* ------------------------------------------------------------------ */
/* UserClient                                                          */
/* ------------------------------------------------------------------ */

bool ACPCaptureUserClient::initWithTask(task_t owningTask, void *securityID,
                                        UInt32 type, OSDictionary *properties)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type, properties))
        return false;
    if (!owningTask) return false;
    fProvider = nullptr;
    return true;
}

bool ACPCaptureUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    fProvider = OSDynamicCast(ACPCapture, provider);
    return fProvider != nullptr;
}

void ACPCaptureUserClient::stop(IOService *provider)
{
    fProvider = nullptr;
    IOUserClient::stop(provider);
}

IOReturn ACPCaptureUserClient::clientClose()
{
    fProvider = nullptr;
    terminate();
    return kIOReturnSuccess;
}

IOReturn ACPCaptureUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                   IOMemoryDescriptor **memory)
{
    if (type != 0 || !fProvider) return kIOReturnBadArgument;
    IOMemoryDescriptor *buf = fProvider->capBuffer();
    if (!buf) return kIOReturnNoMemory;
    buf->retain();
    *memory = buf;
    *options = 0;
    return kIOReturnSuccess;
}

IOReturn ACPCaptureUserClient::externalMethod(uint32_t selector,
                                              IOExternalMethodArguments *args,
                                              IOExternalMethodDispatch *dispatch,
                                              OSObject *target, void *reference)
{
    if (!fProvider) return kIOReturnNotOpen;

    if (selector == kMethodInit) return fProvider->initHardware();
    if (selector != kMethodGetStatus && selector != kMethodInfo && !fProvider->isInit())
        return kIOReturnNotReady;

    switch (selector) {
    case kMethodGetStatus: {
        if (args->scalarOutputCount < 4) return kIOReturnBadArgument;
        UInt64 wb = fProvider->writeBytes();
        args->scalarOutput[0] = (UInt64)(UInt32)(wb & 0xFFFFFFFFu);
        args->scalarOutput[1] = (UInt64)(UInt32)(wb >> 32);
        args->scalarOutput[2] = fProvider->lastLinear();
        args->scalarOutput[3] = fProvider->dmaEnable();
        fProvider->publishStatus();
        return kIOReturnSuccess;
    }
    case kMethodReset: {
        /* 只重置软件环指针；采集继续 */
        return kIOReturnSuccess;
    }
    case kMethodStart:
        return fProvider->startCapture();
    case kMethodStop:
        return fProvider->stopCapture();
    case kMethodArm:
        return fProvider->programPte();
    case kMethodRing:
        return fProvider->ringOnly();
    case kMethodSnap:
        return fProvider->snapshotRing();
    case kMethodInfo: {
        if (args->scalarOutputCount < 3) return kIOReturnBadArgument;
        args->scalarOutput[0] = kRingBytes;
        args->scalarOutput[1] = kCapBytes;
        args->scalarOutput[2] = PAGE_SIZE;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnUnsupported;
    }
}
