/*
 * ACPMicPlugin.c
 *
 * AMD ACP PDM 麦克风的 AudioServerPlugIn（HAL 用户态插件，仅输入）。
 *
 * 内核侧 ACPCapture.kext 已经能通过 IOUserClient 把 PDM 采集到的 PCM 写进一块
 * 4 MB 的软件环缓冲，这个插件做的就是把那块环缓冲包装成一个系统输入设备。
 *
 * 为什么要单独写插件而不是 app：HAL 插件由 coreaudiod 在独立的受沙盒进程里加载，
 * 它能直接打开 IOKit 服务，于是"采集 → 系统麦克风"全程不需要任何中间守护进程。
 *
 * 与内核驱动的约定（必须与 capture/ACPCapture.cpp 完全一致）：
 *   - 服务名          "ACPCapture"
 *   - 用户客户端类型  ACPCaptureUserClient（Info.plist 的
 *                     AudioServerPlugIn_IOKitUserClients 里必须列出）
 *   - selector 0 GetStatus：out[0..1] = 累计写入字节数 lo/hi，out[2] = lastLinear，
 *                          out[3] = dmaEnable。writeBytes 单调递增，对 4 MB 取模
 *                          就是环形缓冲里的写入位置。
 *   - selector 4 Info：out[0] = DMA 环大小(32768)，out[1] = 软件环大小(4194304)，
 *                     out[2] = page(4096)。
 *   - selector 7 Init      映射 BAR、申请 DMA/软件环（必须先调用）
 *   - selector 5 Arm       写 PTE
 *   - selector 6 Ring      写环寄存器
 *   - selector 2 Start     使能 DMA（Start 后 writeBytes 会归零重新计数）
 *   - selector 3 Stop      停止采集
 *
 * 数据格式：S32_LE、2 声道交错、48000 Hz（每帧 8 字节）。
 * 原始信号有非常大的直流偏置（左约 -0.15、右约 -0.43 满量程），真实声音只占 1% 量级，
 * 所以必须先做一阶 DC 阻断再放大，否则听感上只有"砰"的一声和噪声。
 *
 * 线程模型：
 *   - gPlugIn_StateMutex  保护引用计数与 host 指针（查询接口时用）
 *   - gDevice_TimeMutex   只保护零时间戳的锚点，避免 IO 线程等 IOKit
 *   - gDevice_IOMutex     保护 IOKit 连接、环形缓冲读位置、DSP 状态、音量/静音
 * 加锁顺序固定为 IOMutex → TimeMutex 之外互不嵌套，避免死锁。
 */

#include <math.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>      /* 只取常量（kAudioDevicePropertyStreamConfiguration 等） */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#pragma mark - 对象 ID / 常量

/* 对象数量很少，直接静态分配 ID，不需要在 Initialize 里创建设备 */
enum
{
    kObjectID_PlugIn                = kAudioObjectPlugInObject,  /* == 1 */
    kObjectID_Device                = 2,
    kObjectID_Stream_Input          = 3,
    kObjectID_Volume_Input_Master   = 4,
    kObjectID_Mute_Input_Master     = 5,
    /* 数据源控件：HAL 的 'ssc#'/'ssrc' 是由控件列表里的 kAudioDataSourceControlClassID
     * 控件"合成"出来的，插件不能直接应答 'ssc#'，必须挂一个数据源控件。 */
    kObjectID_DataSource_Input      = 6
};

/* ACPCapture 用户客户端 selector，顺序与 capture/ACPCapture.cpp 的 enum 一致 */
enum
{
    kACPSel_GetStatus = 0,
    kACPSel_Start     = 2,
    kACPSel_Stop      = 3,
    kACPSel_Info      = 4,
    kACPSel_Arm       = 5,
    kACPSel_Ring      = 6,
    kACPSel_Init      = 7
};

static const UInt32  kDevice_SampleRate          = 48000;
static const UInt32  kDevice_Channels            = 2;
static const UInt32  kDevice_BytesPerFrame       = 8;      /* S32_LE × 2 */
static const UInt32  kDevice_MinBufferFrames     = 32;
static const UInt32  kDevice_MaxBufferFrames     = 4096;
static const UInt32  kDevice_DefaultBufferFrames = 512;

/* 零时间戳周期：告诉 HAL 每 16384 帧我们给一次锚点，HAL 自己按采样率插值。
 * 取 16384（约 341 ms）是 Apple 示例里的常用值，足够稳又不会太稀疏。 */
static const UInt32  kDevice_ZeroTimeStampPeriod = 16384;

/* 音量控制范围：-96 dB 视为静音，0 dB 为满幅。 */
static const Float32 kVolume_MinDB = -96.0f;
static const Float32 kVolume_MaxDB = 0.0f;

/* 高通拐点。一阶 3.8 Hz 只能挡住直流，挡不住风扇/空调在 50–300 Hz 的轰鸣，
 * 实测安静时能量几乎全在 0–500 Hz（-57 dB 左右），而 2–8 kHz 只有 -78 dB，
 * 说明"吵"的是低频隆隆声而不是电噪。所以换成 90 Hz 的二阶巴特沃斯：
 * 男声基频最低约 85 Hz，90 Hz 拐点对语音影响很小，对风扇声是 -12 dB/oct 的压制。 */
static const Float32 kHighPassHz   = 180.0f;  /* 笔记本麦克风在 100–300 Hz 能量过强，是"闷"的主要来源 */
static const Float32 kPresenceHz   = 3500.0f; /* 辅音清晰度所在频段 */
static const Float32 kPresenceQ    = 1.0f;
static const Float32 kPresenceDb   = 7.0f;    /* +7 dB presence，提升字头可辨度 */
static const Float32 kHighPassQ    = 0.7071f;

/* DC 阻断器极点。0.9995 对应约 3.8 Hz 拐点，远低于人声基频，
 * 又能把缓慢漂移的直流偏置滤干净。任务里已验证该系数听感正常。 */
static const Float32 kDCBlockPole = 0.9995f;

/* 注意：不再用固定增益，改由下面的 AGC 动态决定（见 kAGC_*）。 */

/* 预缓冲 / 积压上限（单位：帧）。
 * 内核定时器每 10 ms 才把 DMA 环拷进软件环，一次约 480 帧；而 HAL 通常每 10.67 ms
 * 来取 512 帧，两者节奏不一致。若"有多少给多少"，每个周期都会短暂断粮而插入静音。
 * 因此先攒 2048 帧（约 43 ms）再开始出数据，此后读指针始终留一截余额，避免爆音；
 * 积压超过 8192 帧（约 170 ms）说明我们落后了，直接丢掉旧数据追回来。 */
static const UInt64  kPrebufferFrames  = 2048;
static const UInt64  kMaxBacklogFrames = 8192;

/* 每次开始采集后先丢 100 ms、再花 20 ms 淡入。
 * 实测这一步能把启动瞬间那一两帧的瞬态（峰值冲到 0.98 那种）压掉。 */
static const UInt32  kStartupSkipFrames = 4800;   /* 100 ms @ 48 kHz */
static const UInt32  kStartupFadeFrames = 960;    /*  20 ms @ 48 kHz */

/* 软限幅的拐点：低于它原样通过，高于它平滑压到 1.0，而不是硬切出方波。 */
static const Float32 kSoftKnee = 0.75f;

/* 自动增益（AGC）。
 * 固定增益的问题：安静的人声只有 0.02 RMS，大声说话又顶到满量程。
 * 这里按块（约 10 ms）估一次 RMS，把增益缓慢拉到"输出 RMS ≈ 0.12"：
 *   - 信号变大时快速压低（attack），避免削波；
 *   - 信号变小时慢慢抬起（release），避免把底噪一起放大、也避免"呼吸感"。
 * 增益本身还夹在 [2, 12] 之间，防止静音时无限抬高。 */
static const Float32 kAGC_TargetRMS  = 0.10f;   /* 留 ~2 dB 峰值余量，避免长期蹭软限幅 */
static const Float32 kAGC_MinGain    = 2.0f;
static const Float32 kAGC_MaxGain    = 24.0f;
static const Float32 kAGC_AttackCoef = 0.35f;   /* 每 10 ms 向目标靠 35% */
static const Float32 kAGC_ReleaseCoef = 0.010f; /* 抬起更慢，避免句尾被"抽"下去 */

static Float32 gAGC_Gain = 6.0f;

/* 噪声底估计与门限。
 * 只做 AGC 会有个要命的副作用：没人说话时它照样把增益往上拱，
 * 把风扇声、底噪一起放大到"目标 RMS"，听起来就是持续的沙沙声。
 * 所以这里再补三件事：
 *   1) 缓慢跟踪噪声底（下降快、上升极慢），
 *   2) 语音判定：明显高于噪声底才算说话，只有说话时才更新 AGC，
 *   3) 非语音时把输出衰减到一个很低但非零的电平（expander），避免完全切断的突兀感。 */
static const Float32 kGateSpeechRatio   = 2.5f;    /* 开口阈值：高于噪声底 2.5 倍 */
static const Float32 kGateCloseRatio    = 1.4f;    /* 关闭阈值：低于噪声底 1.4 倍才关（迟滞，防止尾音被切） */
static const UInt32  kGateHoldBlocks    = 30;      /* 语音结束后再保持 30 块（约 320 ms）才开始关 */
static const Float32 kGateMinRMS        = 0.004f;  /* 绝对下限，防极安静环境误开 */
static const Float32 kGateIdleGain      = 0.03f;   /* 静音时保留 -30 dB */
static const Float32 kGateNoiseCeiling  = 0.050f;  /* 噪声被放大后的上限（-26 dBFS） */

static Float32 gNoiseFloor = 0.004f;
static Float32 gGateGain   = 1.0f;
static Boolean gGateOpen   = false;
static UInt32  gGateHold   = 0;
static Float32 gSpeechEnv  = 0.02f;   /* 慢速跟踪的"说话电平"，AGC 基于它而不是逐块 RMS */

/* 设备/插件元数据 */
#define kDevice_UID_String           "com.shang.driver.ACPMicPlugin:ACPMic"
#define kDevice_Name_String          "AMD ACP PDM Microphone"
#define kDevice_Manufacturer_String  "shang"
#define kDevice_ModelUID_String      "ACPMic"
#define kPlugIn_BundleID_String      "com.shang.driver.ACPMicPlugin"

/* Siri 的麦克风门槛（来源：LatSOFAudio 项目对 AVFAudio / CoreSpeech 的反汇编结论）：
 *  - +[AVVCAudioDeviceManager IsDeviceBuiltIn:] 只在「transport == 'bltn'」且
 *    输入作用域 kAudioDevicePropertyDataSources('ssc#') 的第一项是 'imic' 时为真；
 *  - CoreSpeech 的 CSSiriRecordingInfo 按 transport 分类路由：'bltn' 设备必须
 *    报出 'imic' 或 'emic' 数据源，否则路由为 nil，UI 就显示 Connect a microphone，
 *    即便录音路径完全正常；'usb '/'blue'/'line' 不做数据源检查。
 * 所以这里把我们的插件设备伪装成「内置麦克风 + 'imic' 数据源」。 */
#define kACPImic_DataSourceIMic      'imic'   /* 'imic' 内置麦克风 */
#define kACPImic_DataSourceEMic      'emic'   /* 'emic' 外置麦克风 */

#pragma mark - 接口表（先声明回调，再静态初始化）

static HRESULT  ACPMic_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG    ACPMic_AddRef(void *inDriver);
static ULONG    ACPMic_Release(void *inDriver);
static OSStatus ACPMic_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus ACPMic_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                    const AudioServerPlugInClientInfo *inClientInfo,
                                    AudioObjectID *outDeviceObjectID);
static OSStatus ACPMic_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus ACPMic_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                       const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus ACPMic_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                          const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus ACPMic_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                        AudioObjectID inDeviceObjectID,
                                                        UInt64 inChangeAction, void *inChangeInfo);
static OSStatus ACPMic_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      UInt64 inChangeAction, void *inChangeInfo);
static Boolean  ACPMic_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                   pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus ACPMic_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                          pid_t inClientProcessID,
                                          const AudioObjectPropertyAddress *inAddress,
                                          Boolean *outIsSettable);
static OSStatus ACPMic_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                           pid_t inClientProcessID,
                                           const AudioObjectPropertyAddress *inAddress,
                                           UInt32 inQualifierDataSize, const void *inQualifierData,
                                           UInt32 *outDataSize);
static OSStatus ACPMic_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize, const void *inQualifierData,
                                       UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus ACPMic_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize, const void *inQualifierData,
                                       UInt32 inDataSize, const void *inData);
static OSStatus ACPMic_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                               UInt32 inClientID);
static OSStatus ACPMic_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                              UInt32 inClientID);
static OSStatus ACPMic_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                        UInt32 inClientID, Float64 *outSampleTime,
                                        UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus ACPMic_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                         UInt32 inClientID, UInt32 inOperationID,
                                         Boolean *outWillDo, Boolean *outWillDoInPlace);
static OSStatus ACPMic_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                        UInt32 inClientID, UInt32 inOperationID,
                                        UInt32 inIOBufferFrameSize,
                                        const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus ACPMic_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     AudioObjectID inStreamObjectID, UInt32 inClientID,
                                     UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                     const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                     void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus ACPMic_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID, UInt32 inOperationID,
                                      UInt32 inIOBufferFrameSize,
                                      const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

static AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface =
{
    ._reserved                      = NULL,
    .QueryInterface                 = ACPMic_QueryInterface,
    .AddRef                         = ACPMic_AddRef,
    .Release                        = ACPMic_Release,
    .Initialize                     = ACPMic_Initialize,
    .CreateDevice                   = ACPMic_CreateDevice,
    .DestroyDevice                  = ACPMic_DestroyDevice,
    .AddDeviceClient                = ACPMic_AddDeviceClient,
    .RemoveDeviceClient             = ACPMic_RemoveDeviceClient,
    .PerformDeviceConfigurationChange = ACPMic_PerformDeviceConfigurationChange,
    .AbortDeviceConfigurationChange = ACPMic_AbortDeviceConfigurationChange,
    .HasProperty                    = ACPMic_HasProperty,
    .IsPropertySettable             = ACPMic_IsPropertySettable,
    .GetPropertyDataSize            = ACPMic_GetPropertyDataSize,
    .GetPropertyData                = ACPMic_GetPropertyData,
    .SetPropertyData                = ACPMic_SetPropertyData,
    .StartIO                        = ACPMic_StartIO,
    .StopIO                         = ACPMic_StopIO,
    .GetZeroTimeStamp               = ACPMic_GetZeroTimeStamp,
    .WillDoIOOperation              = ACPMic_WillDoIOOperation,
    .BeginIOOperation               = ACPMic_BeginIOOperation,
    .DoIOOperation                  = ACPMic_DoIOOperation,
    .EndIOOperation                 = ACPMic_EndIOOperation
};

/* CFPlugIn 拿到的是 "指向接口指针的指针"（驱动引用） */
static AudioServerPlugInDriverInterface *gAudioServerPlugInDriverInterfacePtr = &gAudioServerPlugInDriverInterface;
static AudioServerPlugInDriverRef        gAudioServerPlugInDriverRef = &gAudioServerPlugInDriverInterfacePtr;

#pragma mark - 全局状态

static pthread_mutex_t gPlugIn_StateMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gDevice_TimeMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gDevice_IOMutex   = PTHREAD_MUTEX_INITIALIZER;

static UInt32                       gPlugIn_RefCount = 1;
static AudioServerPlugInHostRef     gPlugIn_Host = NULL;

static UInt64   gDevice_IOIsRunning          = 0;          /* 有多少个 client 正在跑 IO */
static UInt32   gDevice_BufferFrameSize      = kDevice_DefaultBufferFrames;
static Float64  gDevice_HostTicksPerFrame    = 0.0;
static Float64  gDevice_AnchorSampleTime     = 0.0;
static UInt64   gDevice_AnchorHostTime       = 0;
static UInt64   gDevice_ZeroSeed             = 1;

/* 硬件校准时钟（思路来自 LatSOFAudioPlugin.c 的 hardware-disciplined clock）。
 * 纯主机时钟的问题：PDM 的 48 kHz 来自板载时钟，和 host 时钟不是同一个源，
 * 跑久了会累积出几百个样本的偏差，HAL 就认为每个 IO 周期都"迟到"
 * （kIOReturnIsoTooOld），表现为爆音/卡顿，最后 coreaudiod 过载。
 * 这里改成读 kext 的累计写入位置当"真实时间"，主机时钟只用于插值。 */
static Boolean  gHWClk_Primed                 = false;
static UInt64   gHWClk_Wraps                  = 0;   /* 已过去的零时间戳周期数 */
static UInt64   gHWClk_WrapHostTime           = 0;   /* 上次周期边界对应的主机时间 */
static UInt64   gHWClk_LastPos                = 0;   /* 上次读到的周期内相位（帧） */

static Float32  gVolume_Input_Master_Value   = 1.0f;       /* 线性幅度，1.0 = 0 dB */
static Boolean  gMute_Input_Master_Value     = false;

/* IOKit 采集状态（gDevice_IOMutex 保护） */
static io_connect_t      gCapture_Connect           = 0;
static uint8_t          *gCapture_Ring              = NULL;
static mach_vm_size_t    gCapture_RingSize          = 0;
static UInt64            gCapture_WriteBytes        = 0;   /* 最近一次 GetStatus 的累计写入 */
static UInt64            gCapture_ReadBytes         = 0;   /* 我们已经消费到的累计位置 */
static Boolean           gCapture_Active            = false;
static Boolean           gCapture_Primed            = false;
static UInt64            gCapture_StarvedCycles     = 0;
static Boolean           gCapture_LoggedNoService   = false;
static Boolean           gCapture_LoggedStarved     = false;
static Boolean           gCapture_LoggedBacklog     = false;
/* 每次 StartIO 后先丢掉开头一段样本（PDM 启动瞬间的残余跳变），再淡入。
 * 丢 100 ms 是为了让 DC 阻断器彻底收敛：极点 0.9995 的时间常数约 2000 个样本，
 * 100 ms = 4800 个样本，足够收敛到 1e-3 以内。 */
static UInt32            gCapture_SkipFrames        = 0;
/* 剩余还需要淡入的帧数：增益从 0 线性升到 1，避免"突然出现"的咔哒声 */
static UInt32            gCapture_FadeFrames        = 0;
static UInt64            gProbe_Calls               = 0;   /* 被 coreaudiod 要求取数据的次数 */
static UInt64            gProbe_Frames              = 0;
static UInt64            gProbe_SilentNotReady      = 0;
static UInt64            gProbe_SilentStarved       = 0;
static Float32           gProbe_Peak                = 0.0f;
static UInt64            gProbe_Dumped              = 0;

/* 二阶高通（biquad）系数，采样率固定 48 kHz，直接算一次 */
static Float32 gHP_B0 = 1.0f, gHP_B1 = 0.0f, gHP_B2 = 0.0f, gHP_A1 = 0.0f, gHP_A2 = 0.0f;
static Boolean gHP_Ready = false;

/* 二阶高通的历史状态（每声道各一组），跨 IO 周期必须连续 */
static Float32 gPK_B0 = 1.0f, gPK_B1 = 0.0f, gPK_B2 = 0.0f, gPK_A1 = 0.0f, gPK_A2 = 0.0f;
static bool    gPK_Ready = false;
static Float32 gPK_X1[2] = { 0.0f, 0.0f };
static Float32 gPK_X2[2] = { 0.0f, 0.0f };
static Float32 gPK_Y1[2] = { 0.0f, 0.0f };
static Float32 gPK_Y2[2] = { 0.0f, 0.0f };

static Float32 gHP_X1[2] = { 0.0f, 0.0f };
static Float32 gHP_X2[2] = { 0.0f, 0.0f };
static Float32 gHP_Y1[2] = { 0.0f, 0.0f };
static Float32 gHP_Y2[2] = { 0.0f, 0.0f };

/* DC 阻断器状态（每声道一组），跨 IO 周期必须连续 */
static Float32 gDC_X1[2] = { 0.0f, 0.0f };
static Float32 gDC_Y1[2] = { 0.0f, 0.0f };
/* 是否已经用第一帧做过"预热"：直接把 x1 设成第一帧、输出 0，
 * 否则 x1 初值为 0 而信号带着巨大直流偏置，第一帧就会被算成满量程的阶跃，
 * 听起来是"砰"的一声爆音。 */
static Boolean gDC_Primed[2] = { false, false };

static void ACPMic_ResetDCBlocker(void)
{
    for (UInt32 ch = 0; ch < 2; ch++)
    {
        gDC_X1[ch] = 0.0f;
        gDC_Y1[ch] = 0.0f;
        gDC_Primed[ch] = false;
        gHP_X1[ch] = 0.0f;
        gPK_X1[ch] = 0.0f;
        gHP_X2[ch] = 0.0f;
        gHP_Y1[ch] = 0.0f;
        gHP_Y2[ch] = 0.0f;
    }
}

#pragma mark - 日志

/* 统一走 os_log，方便在 Console.app 里按进程过滤；
 * 不用 printf，因为 HAL 进程的 stdout 会被 launchd 丢掉。 */
static void ACPMic_Log(const char *inFormat, ...) __attribute__((format(printf, 1, 2)));
static void ACPMic_Log(const char *inFormat, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, inFormat);
    vsnprintf(buf, sizeof(buf), inFormat, ap);
    va_end(ap);
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "ACPMic: %{public}s", buf);
}

#pragma mark - 时间换算


/* 诊断用：把插件内部状态写进 /Users/Shared/acpmic-hal-status.txt，
 * 便于在没有 log 权限时判断"谁在用我们的设备、有没有数据"。 */
static void ACPMic_ProbeEnter(UInt32 inFrames)
{
    gProbe_Calls++;
    gProbe_Frames += inFrames;

    if (gProbe_Calls % 50 != 0) return;

    /* 同时写 stderr：进 coreaudiod 的日志流，用
     *   log show --last 2m --predicate 'process == "coreaudiod"' | grep ACPMic
     * 就能看到；文件因沙箱写不进去时这条依然有效。 */
    fprintf(stderr, "ACPMic: probe calls=%llu frames=%llu conn=%u active=%d wb=%llu rb=%llu "
                    "starved=%llu notready=%llu gain=%.2f gate=%.3f peak=%.4f\n",
            (unsigned long long)gProbe_Calls, (unsigned long long)gProbe_Frames,
            (unsigned)gCapture_Connect, gCapture_Active ? 1 : 0,
            (unsigned long long)gCapture_WriteBytes, (unsigned long long)gCapture_ReadBytes,
            (unsigned long long)gCapture_StarvedCycles, (unsigned long long)gProbe_SilentNotReady,
            (double)gAGC_Gain, (double)gGateGain, (double)gProbe_Peak);

    FILE *f = fopen("/Users/Shared/acpmic-hal-status.txt", "w");
    if (f == NULL) return;

    fprintf(f, "ACPMic HAL 状态  (每 50 次取数刷新一次)\n");
    fprintf(f, "取数次数        : %llu  累计帧 %llu\n",
            (unsigned long long)gProbe_Calls, (unsigned long long)gProbe_Frames);
    fprintf(f, "kext 连接       : %s (handle %u)\n", gCapture_Connect ? "已连接" : "未连接",
            (unsigned)gCapture_Connect);
    fprintf(f, "采集激活        : %s\n", gCapture_Active ? "是" : "否");
    fprintf(f, "环地址/大小     : %p / %llu\n", (void *)gCapture_Ring,
            (unsigned long long)gCapture_RingSize);
    fprintf(f, "writeBytes      : %llu\n", (unsigned long long)gCapture_WriteBytes);
    fprintf(f, "readBytes       : %llu\n", (unsigned long long)gCapture_ReadBytes);
    fprintf(f, "数据不足周期    : %llu (历史 %llu)\n",
            (unsigned long long)gCapture_StarvedCycles, (unsigned long long)gProbe_SilentStarved);
    fprintf(f, "未就绪周期      : %llu\n", (unsigned long long)gProbe_SilentNotReady);
    fprintf(f, "静音/音量       : %s / %.2f\n", gMute_Input_Master_Value ? "静音" : "不静音",
            (double)gVolume_Input_Master_Value);
    fprintf(f, "增益 AGC/门     : %.2f / %.3f\n", (double)gAGC_Gain, (double)gGateGain);
    fprintf(f, "噪声底/语音包络 : %.6f / %.6f\n", (double)gNoiseFloor, (double)gSpeechEnv);
    fprintf(f, "输出峰值        : %.6f\n", (double)gProbe_Peak);
    fclose(f);
}

static void ACPMic_UpdateHostClock(void)
{
    mach_timebase_info_data_t tb = { 0, 0 };
    if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0)
    {
        /* 极端情况下退回 1:1，至少不会除零 */
        gDevice_HostTicksPerFrame = 1.0e9 / (Float64)kDevice_SampleRate;
        return;
    }
    /* mach_absolute_time 单位换算成纳秒，再除以采样率得到"每帧多少个 host tick" */
    Float64 hostClockFrequency = 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
    gDevice_HostTicksPerFrame = hostClockFrequency / (Float64)kDevice_SampleRate;
}

/* 按当前锚点算出 (sampleTime, hostTime)。调用者需持有 gDevice_TimeMutex。 */
static kern_return_t ACPMic_FetchStatusLocked(UInt64 *outWriteBytes);   /* 定义在后面 */

static void ACPMic_ComputeTimeStampLocked(UInt64 inNow, Float64 *outSampleTime, UInt64 *outHostTime)
{
    UInt64 ticksPerPeriod = (UInt64)llround(gDevice_HostTicksPerFrame * (Float64)kDevice_ZeroTimeStampPeriod);
    if (ticksPerPeriod == 0)
    {
        ticksPerPeriod = 1;
    }

    /* ---- 优先用硬件位置校准 ----
     * writeBytes 是 kext 累计写入软件环的字节数，除以每帧字节数就是"已经产生了多少帧"，
     * 对零时间戳周期取模就是当前相位。它由 PDM 硬件时钟驱动，不受 host 时钟漂移影响。 */
    if (gCapture_Active && gCapture_Connect != 0)
    {
        UInt64 writeBytes = 0;
        if (ACPMic_FetchStatusLocked(&writeBytes) == KERN_SUCCESS)
        {
            UInt64 posFrames = (writeBytes / kDevice_BytesPerFrame) % kDevice_ZeroTimeStampPeriod;

            /* 把"观察到相位的这一刻"倒推回周期边界 */
            UInt64 back = (UInt64)((Float64)posFrames * gDevice_HostTicksPerFrame);
            UInt64 observed = (inNow > back) ? (inNow - back) : inNow;

            if (!gHWClk_Primed)
            {
                gHWClk_WrapHostTime = observed;
                gHWClk_Primed = true;
            }
            else if (posFrames < gHWClk_LastPos)
            {
                /* 相位回绕 = 新周期开始。用 1/8 的权重向观测值修正，
                 * 吸收 syscall 抖动，同时不让速率跟着抖。 */
                gHWClk_Wraps++;
                Float64 predicted = (Float64)gHWClk_WrapHostTime + (Float64)ticksPerPeriod;
                Float64 error     = (Float64)observed - predicted;
                gHWClk_WrapHostTime = (UInt64)(predicted + error / 8.0);
            }
            gHWClk_LastPos = posFrames;

            *outSampleTime = (Float64)(gHWClk_Wraps * (UInt64)kDevice_ZeroTimeStampPeriod);
            *outHostTime   = gHWClk_WrapHostTime;
            return;
        }
    }

    /* ---- 兜底：kext 没连上时退回纯主机时钟 ---- */
    UInt64 elapsed = (inNow > gDevice_AnchorHostTime) ? (inNow - gDevice_AnchorHostTime) : 0;
    UInt64 periods = elapsed / ticksPerPeriod;

    *outSampleTime = gDevice_AnchorSampleTime + (Float64)periods * (Float64)kDevice_ZeroTimeStampPeriod;
    *outHostTime   = gDevice_AnchorHostTime + periods * ticksPerPeriod;
}

#pragma mark - 与内核驱动交互

/* 调用无输出参数的 selector */
static kern_return_t ACPMic_CallNoOutput(io_connect_t inConnect, uint32_t inSelector)
{
    uint32_t outCount = 0;
    return IOConnectCallScalarMethod(inConnect, inSelector, NULL, 0, NULL, &outCount);
}

/* 读取累计写入字节数。调用者需持有 gDevice_IOMutex。 */
static kern_return_t ACPMic_FetchStatusLocked(UInt64 *outWriteBytes)
{
    if (gCapture_Connect == 0)
    {
        return kIOReturnNotOpen;
    }

    uint64_t values[4] = { 0, 0, 0, 0 };
    uint32_t count = 4;
    kern_return_t kr = IOConnectCallScalarMethod(gCapture_Connect, kACPSel_GetStatus, NULL, 0, values, &count);
    if (kr != KERN_SUCCESS || count < 4)
    {
        return (kr == KERN_SUCCESS) ? kIOReturnError : kr;
    }

    *outWriteBytes = (UInt64)values[0] | ((UInt64)values[1] << 32);
    return KERN_SUCCESS;
}

/* 关闭连接并解映射。调用者需持有 gDevice_IOMutex。 */
static void ACPMic_CloseCaptureLocked(Boolean inStopFirst)
{
    if (gCapture_Connect != 0)
    {
        if (inStopFirst && gCapture_Active)
        {
            (void)ACPMic_CallNoOutput(gCapture_Connect, kACPSel_Stop);
        }
        if (gCapture_Ring != NULL)
        {
            (void)IOConnectUnmapMemory64(gCapture_Connect, 0, mach_task_self(),
                                         (mach_vm_address_t)(uintptr_t)gCapture_Ring);
        }
        (void)IOServiceClose(gCapture_Connect);
    }

    gCapture_Connect    = 0;
    gCapture_Ring       = NULL;
    gCapture_RingSize   = 0;
    gCapture_WriteBytes = 0;
    gCapture_ReadBytes  = 0;
    gCapture_Active     = false;
    gCapture_Primed     = false;
    gCapture_StarvedCycles = 0;
    gHWClk_Primed       = false;
    gHWClk_Wraps        = 0;
    gHWClk_LastPos      = 0;
}

/* 找服务 → 打开用户客户端 → Info → Init → 映射软件环 → Arm → Ring → Start。
 * 返回 0 表示开始采集；返回 -1 表示内核驱动不在或初始化失败（此时设备照常存在，
 * 只是输出静音，这是故意的：kext 没加载时删掉设备会让用户的录音软件报错）。 */
static int ACPMic_StartCaptureLocked(void)
{
    if (gCapture_Active)
    {
        return 0;
    }

    if (gCapture_Connect == 0)
    {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                           IOServiceMatching("ACPCapture"));
        if (service == 0)
        {
            if (!gCapture_LoggedNoService)
            {
                gCapture_LoggedNoService = true;
                ACPMic_Log("未找到 IOService \"ACPCapture\"（内核 kext 未加载？）。"
                           "设备保留并输出静音，安装 kext 后重新启动 coreaudiod 即可恢复。");
            }
            return -1;
        }

        io_connect_t connect = 0;
        kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || connect == 0)
        {
            ACPMic_Log("IOServiceOpen 失败：0x%x（kext 的 IOUserClient 拒绝连接？）", kr);
            return -1;
        }
        gCapture_Connect = connect;
        ACPMic_Log("已连接 ACPCapture，准备初始化采集");
    }

    /* 4 Info：确认软件环大小与页面大小，顺便验证用户客户端可用 */
    uint64_t info[3] = { 0, 0, 0 };
    uint32_t infoCount = 3;
    kern_return_t kr = IOConnectCallScalarMethod(gCapture_Connect, kACPSel_Info, NULL, 0, info, &infoCount);
    if (kr != KERN_SUCCESS || infoCount < 2)
    {
        ACPMic_Log("Info 调用失败：0x%x（kext 版本不匹配？）", kr);
        ACPMic_CloseCaptureLocked(false);
        return -1;
    }

    /* 7 Init：必须最先调用，否则 kext 的其它方法都返回 kIOReturnNotReady，
     * 而且软件环 fCapBuf 是在 Init 里分配的，所以 MapMemory 必须排在它后面。 */
    kr = ACPMic_CallNoOutput(gCapture_Connect, kACPSel_Init);
    if (kr != KERN_SUCCESS)
    {
        ACPMic_Log("Init 失败：0x%x", kr);
        ACPMic_CloseCaptureLocked(false);
        return -1;
    }

    /* 映射 type 0 的 4 MB 软件环 */
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    kr = IOConnectMapMemory64(gCapture_Connect, 0, mach_task_self(), &address, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || address == 0 || size == 0)
    {
        ACPMic_Log("映射软件环失败：0x%x", kr);
        ACPMic_CloseCaptureLocked(false);
        return -1;
    }

    gCapture_Ring     = (uint8_t *)(uintptr_t)address;
    gCapture_RingSize = size;

    /* 5 Arm → 6 Ring → 2 Start，顺序不能变 */
    kr = ACPMic_CallNoOutput(gCapture_Connect, kACPSel_Arm);
    if (kr != KERN_SUCCESS)
    {
        ACPMic_Log("Arm 失败：0x%x", kr);
        ACPMic_CloseCaptureLocked(false);
        return -1;
    }
    kr = ACPMic_CallNoOutput(gCapture_Connect, kACPSel_Ring);
    if (kr != KERN_SUCCESS)
    {
        ACPMic_Log("Ring 失败：0x%x", kr);
        ACPMic_CloseCaptureLocked(false);
        return -1;
    }
    kr = ACPMic_CallNoOutput(gCapture_Connect, kACPSel_Start);
    if (kr != KERN_SUCCESS)
    {
        ACPMic_Log("Start 失败：0x%x", kr);
        ACPMic_CloseCaptureLocked(false);
        return -1;
    }

    /* Start 之后 kext 会把 writeBytes 归零并丢掉前 200 ms，读位置也必须归零；
     * 否则会拿旧的读位置去减新的 writeBytes，算出巨大的差值。 */
    gCapture_WriteBytes = 0;
    gCapture_ReadBytes  = 0;
    gCapture_Primed     = false;
    gCapture_Active     = true;
    gCapture_StarvedCycles = 0;
    gCapture_LoggedStarved = false;
    gCapture_LoggedBacklog = false;
    ACPMic_ResetDCBlocker();
    gCapture_SkipFrames = kStartupSkipFrames;
    gCapture_FadeFrames = kStartupFadeFrames;

    /* 新的一次采集 = 新的时间轴：重新用硬件位置校准 */
    gHWClk_Primed   = false;
    gHWClk_Wraps    = 0;
    gHWClk_LastPos  = 0;
    gGateOpen       = false;
    gGateHold       = 0;
    gGateGain       = 1.0f;
    gSpeechEnv      = 0.02f;

    ACPMic_Log("采集已启动：软件环 %u 字节，DMA 环 %u 字节，page %u",
               (unsigned)size, (unsigned)info[0], (unsigned)info[2]);
    return 0;
}

#pragma mark - DSP

/* 软限幅：|x| <= knee 时原样通过；超过 knee 用 tanh 平滑逼近 1.0。
 * 硬切会在过载时产生大量高频谐波（听起来是刺耳的"炸"），软限幅只是温和压缩。 */
static inline Float32 ACPMic_Clip(Float32 inValue)
{
    if (inValue != inValue) return 0.0f;          /* NaN 保护：一旦出现会污染整条链路 */

    Float32 sign = (inValue < 0.0f) ? -1.0f : 1.0f;
    Float32 mag  = (inValue < 0.0f) ? -inValue : inValue;
    if (mag <= kSoftKnee) return inValue;

    /* knee 之上把剩余 0.25 的余量用 tanh 曲线吃掉 */
    Float32 over = (mag - kSoftKnee) / (1.0f - kSoftKnee);
    Float32 soft = kSoftKnee + (1.0f - kSoftKnee) * tanhf(over);
    return sign * soft;
}

/* 把 S32 原始样本转成 float，做 DC 阻断 → 增益 → 限幅。
 * 注意 DC 阻断必须作用在"未乘增益"的信号上，状态跨 buffer 连续。 */
/* RBJ 二阶高通系数（48 kHz 固定采样率） */
static void ACPMic_InitHighPass(void)
{
    const Float32 fs = 48000.0f;
    Float32 w0    = 2.0f * (Float32)M_PI * kHighPassHz / fs;
    Float32 cosw0 = cosf(w0);
    Float32 sinw0 = sinf(w0);
    Float32 alpha = sinw0 / (2.0f * kHighPassQ);

    Float32 a0 = 1.0f + alpha;
    gHP_B0 = (1.0f + cosw0) / 2.0f / a0;
    gHP_B1 = -(1.0f + cosw0) / a0;
    gHP_B2 = gHP_B0;
    gHP_A1 = -2.0f * cosw0 / a0;
    gHP_A2 = (1.0f - alpha) / a0;
    gHP_Ready = true;
}

/* RBJ peaking（presence 提升） */
static void ACPMic_InitPresence(void)
{
    const Float32 fs = 48000.0f;
    Float32 A     = powf(10.0f, kPresenceDb / 40.0f);
    Float32 w0    = 2.0f * (Float32)M_PI * kPresenceHz / fs;
    Float32 cosw0 = cosf(w0);
    Float32 sinw0 = sinf(w0);
    Float32 alpha = sinw0 / (2.0f * kPresenceQ);

    Float32 a0 = 1.0f + alpha / A;
    gPK_B0 = (1.0f + alpha * A) / a0;
    gPK_B1 = (-2.0f * cosw0) / a0;
    gPK_B2 = (1.0f - alpha * A) / a0;
    gPK_A1 = (-2.0f * cosw0) / a0;
    gPK_A2 = (1.0f - alpha / A) / a0;
    gPK_Ready = true;
}

static inline Float32 ACPMic_ProcessSample(UInt32 inChannel, int32_t inSample, Float32 inGain)
{
    /* 除以 2^31 而不是 2^31-1，这样 -2^31 也能落在 [-1, 1) */
    Float32 x = (Float32)inSample * (1.0f / 2147483648.0f);

    /* 每个声道的第一帧只用来对齐直流基准，不输出 */
    if (!gDC_Primed[inChannel])
    {
        gDC_X1[inChannel] = 0.0f;      /* 一阶高通的输入是 hp，不是原始 x */
        gDC_Y1[inChannel] = 0.0f;
        gDC_Primed[inChannel] = true;
        return 0.0f;
    }

    /* 二阶高通（90 Hz），先把风扇/空调那层低频隆隆声削掉，
     * 再交给一阶 DC 阻断兜底（防止残余直流把 AGC 骗到最大增益）。 */
    if (!gHP_Ready)
    {
        ACPMic_InitHighPass();
    }
    Float32 hp = gHP_B0 * x + gHP_B1 * gHP_X1[inChannel] + gHP_B2 * gHP_X2[inChannel]
               - gHP_A1 * gHP_Y1[inChannel] - gHP_A2 * gHP_Y2[inChannel];
    gHP_X2[inChannel] = gHP_X1[inChannel];
    gHP_X1[inChannel] = x;
    gHP_Y2[inChannel] = gHP_Y1[inChannel];
    gHP_Y1[inChannel] = hp;

    /* 一阶高通：y[n] = in[n] - in[n-1] + R * y[n-1]
     * 注意这里的 in 必须是二阶高通的输出 hp，不能是原始 x：
     * 一阶高通的传输函数里 1/(1-R) 是个大增益（R=0.9995 时是 2000），
     * 如果前面留着原始信号的直流，稳态输出会变成 -2000×直流 的一个常数偏置。 */
    Float32 y = hp - gDC_X1[inChannel] + kDCBlockPole * gDC_Y1[inChannel];

    /* 把极小的值压成 0，避免 x86 上的 denormal 让 IO 线程变慢（听不见但很吃 CPU） */
    if (y > -1.0e-20f && y < 1.0e-20f)
    {
        y = 0.0f;
    }

    gDC_X1[inChannel] = hp;
    gDC_Y1[inChannel] = y;

    /* presence：+4 dB @ 3 kHz，把字头/辅音抬起来，
     * 否则 100–300 Hz 的厅堂感会盖住清晰度（听感就是"模糊"）。 */
    if (!gPK_Ready)
    {
        ACPMic_InitPresence();
    }
    Float32 pk = gPK_B0 * y + gPK_B1 * gPK_X1[inChannel] + gPK_B2 * gPK_X2[inChannel]
               - gPK_A1 * gPK_Y1[inChannel] - gPK_A2 * gPK_Y2[inChannel];
    gPK_X2[inChannel] = gPK_X1[inChannel];
    gPK_X1[inChannel] = y;
    gPK_Y2[inChannel] = gPK_Y1[inChannel];
    gPK_Y1[inChannel] = pk;

    return ACPMic_Clip(pk * inGain);
}

#pragma mark - 填充输入缓冲

/* 一次 IO 周期：从 writeBytes 增量里取数据填进 HAL 给的 float32 交错缓冲。
 * 调用者需持有 gDevice_IOMutex。 */
static void ACPMic_FillInputBufferLocked(UInt32 inFrames, Float32 *outBuffer)
{
    const size_t silenceBytes = (size_t)inFrames * kDevice_Channels * sizeof(Float32);

    ACPMic_ProbeEnter(inFrames);

    if (gCapture_Ring == NULL || gCapture_Connect == 0 || !gCapture_Active)
    {
        memset(outBuffer, 0, silenceBytes);
        gProbe_SilentNotReady++;
        return;
    }

    UInt64 writeBytes = 0;
    if (ACPMic_FetchStatusLocked(&writeBytes) != KERN_SUCCESS)
    {
        memset(outBuffer, 0, silenceBytes);
        gProbe_SilentNotReady++;
        return;
    }

    /* writeBytes 变小说明 kext 被重新 Start 过（它会把计数归零），重新对齐，
     * 并且清掉 DC 状态，否则新旧信号拼接处会有一下爆音。 */
    if (writeBytes < gCapture_ReadBytes)
    {
        gCapture_ReadBytes = writeBytes;
        gCapture_Primed = false;
        ACPMic_ResetDCBlocker();
        /* kext 重新 Start 过，等于一次新采集：同样要丢掉开头那段不稳定的样本 */
        gCapture_SkipFrames = kStartupSkipFrames;
        gCapture_FadeFrames = kStartupFadeFrames;
    }

    const UInt64 needBytes = (UInt64)inFrames * kDevice_BytesPerFrame;
    UInt64 available = writeBytes - gCapture_ReadBytes;

    /* 积压太多：丢掉最旧的一段，只保留一个预缓冲的量，避免延迟无限增长 */
    if (available > kMaxBacklogFrames * kDevice_BytesPerFrame)
    {
        UInt64 keep = (kPrebufferFrames > inFrames) ? kPrebufferFrames : (UInt64)inFrames;
        UInt64 drop = available - keep * kDevice_BytesPerFrame;
        drop -= drop % kDevice_BytesPerFrame;      /* 必须按整帧丢，否则左右声道会错位 */
        gCapture_ReadBytes += drop;
        available -= drop;
        if (!gCapture_LoggedBacklog)
        {
            gCapture_LoggedBacklog = true;
            ACPMic_Log("采集积压 %llu 帧，已丢弃最旧的 %llu 帧以追上实时",
                       (unsigned long long)((available + drop) / kDevice_BytesPerFrame),
                       (unsigned long long)(drop / kDevice_BytesPerFrame));
        }
    }

    /* 预缓冲：刚启动或刚断流时先攒够一截再出数据，避免每个周期都插入静音 */
    if (!gCapture_Primed)
    {
        UInt64 primeBytes = kPrebufferFrames * kDevice_BytesPerFrame;
        if (primeBytes < needBytes)
        {
            primeBytes = needBytes;
        }
        if (available < primeBytes)
        {
            memset(outBuffer, 0, silenceBytes);
            gCapture_StarvedCycles++;
            gProbe_SilentStarved++;
            return;
        }
        gCapture_Primed = true;
        ACPMic_Log("预缓冲就绪：%llu 帧可用", (unsigned long long)(available / kDevice_BytesPerFrame));
    }

    /* 真的断粮：本周期整块静音，但不清空已有数据，下个周期补上。
     * 这样只在 kext 卡住时才会出现静音，而不会周期性掉样本。 */
    if (available < needBytes)
    {
        memset(outBuffer, 0, silenceBytes);
        gCapture_StarvedCycles++;
        gProbe_SilentStarved++;
        if (gCapture_StarvedCycles > 200 && !gCapture_LoggedStarved)
        {
            gCapture_LoggedStarved = true;
            ACPMic_Log("连续 %llu 个周期数据不足（writeBytes=%llu, read=%llu），输出静音",
                       (unsigned long long)gCapture_StarvedCycles,
                       (unsigned long long)writeBytes,
                       (unsigned long long)gCapture_ReadBytes);
        }
        return;
    }
    gCapture_StarvedCycles = 0;

    /* 先按"单位增益"填进去（只做 DC 阻断），AGC 与限幅在本函数末尾统一做。
     * 这样同一块数据只需扫一遍就能算出这一块的 RMS，不必额外缓冲。 */
    const Float32 unity = 1.0f;

    UInt32 framesLeft = inFrames;
    Float32 *dst = outBuffer;
    Float32 *procStart = NULL;      /* 第一个真正参与 DSP 的样本位置（跳过静音之后） */
    UInt32   procFrames = 0;

    while (framesLeft > 0)
    {
        UInt32 offset = (UInt32)(gCapture_ReadBytes % gCapture_RingSize);

        /* 环尾可能把一个 buffer 截断，先算这一段能连续读多少帧 */
        UInt32 contiguous = (UInt32)((gCapture_RingSize - offset) / kDevice_BytesPerFrame);
        if (contiguous == 0)
        {
            gCapture_ReadBytes += kDevice_BytesPerFrame;   /* 理论上不会发生，防御一下 */
            continue;
        }

        UInt32 frames = (framesLeft < contiguous) ? framesLeft : contiguous;
        const int32_t *src = (const int32_t *)(const void *)(gCapture_Ring + offset);

        UInt32 i = 0;

        /* 1) 开头那段样本直接丢掉（只输出静音），只推进读指针。
         *    它们包含 PDM 刚使能时的残余跳变，正是偶发爆音的来源。 */
        if (gCapture_SkipFrames > 0)
        {
            UInt32 skip = (frames < gCapture_SkipFrames) ? frames : gCapture_SkipFrames;
            memset(dst, 0, (size_t)skip * kDevice_Channels * sizeof(Float32));
            gCapture_SkipFrames -= skip;
            i += skip;
            dst += skip * kDevice_Channels;
        }

        /* 2) 剩下的正常输出，前 20 ms 用一个线性斜坡淡入 */
        for (; i < frames; i++)
        {
            Float32 fade = 1.0f;
            if (gCapture_FadeFrames > 0)
            {
                fade = 1.0f - ((Float32)gCapture_FadeFrames / (Float32)kStartupFadeFrames);
                gCapture_FadeFrames--;
            }

            if (!procStart) procStart = dst;

            dst[0] = ACPMic_ProcessSample(0, src[i * 2 + 0], unity * fade);
            dst[1] = ACPMic_ProcessSample(1, src[i * 2 + 1], unity * fade);
            dst += 2;
            procFrames++;
        }

        gCapture_ReadBytes += (UInt64)frames * kDevice_BytesPerFrame;
        /* dst 已经在上面逐帧推进过了，这里不能再加一次 */
        framesLeft -= frames;
    }

    /* ---- AGC + 音量 + 软限幅 ----
     * 用本块的实际 RMS 去调整增益：这一块是"测量块"，先按 unity 填好再整体缩放，
     * 所以不会有"半块旧增益、半块新增益"的咔哒声。 */
    if (procStart != NULL && procFrames > 0)
    {
        const UInt32 n = procFrames * kDevice_Channels;

        double ss = 0.0;
        for (UInt32 i = 0; i < n; i++)
        {
            double v = (double)procStart[i];
            ss += v * v;
        }
        Float32 rms = (Float32)sqrt(ss / (double)n);

        /* 噪声底：掉下去要快（真实环境变安静了要立刻跟上），
         * 涨上去要极慢（避免把一句话的尾音当成新噪声底往上抬）。 */
        if (rms < gNoiseFloor)
        {
            gNoiseFloor += (rms - gNoiseFloor) * 0.20f;
        }
        else
        {
            gNoiseFloor += (rms - gNoiseFloor) * 0.0008f;
        }

        /* 迟滞比较：开门用 2.5×噪声底，关门要等到 1.4×噪声底，
         * 否则话音一落、RMS 掉到阈值下面，门立刻开始关，尾音就被切掉了。 */
        Float32 openThresh  = gNoiseFloor * kGateSpeechRatio;
        Float32 closeThresh = gNoiseFloor * kGateCloseRatio;
        if (closeThresh < kGateMinRMS * 0.5f) closeThresh = kGateMinRMS * 0.5f;

        Boolean speech;
        if (!gGateOpen)
        {
            speech = (rms > openThresh) && (rms > kGateMinRMS);
            if (speech) gGateHold = kGateHoldBlocks;
        }
        else
        {
            if (rms > closeThresh)
            {
                speech     = true;
                gGateHold  = kGateHoldBlocks;      /* 还有声音就续期 */
            }
            else if (gGateHold > 0)
            {
                speech = true;                     /* 保持期内继续放行 */
                gGateHold--;
            }
            else
            {
                speech = false;
            }
        }

        if (speech && !gGateOpen)
        {
            gGateOpen = true;
        }
        else if (!speech && gGateOpen && gGateHold == 0)
        {
            gGateOpen = false;
        }

        /* AGC 不用逐块 RMS，而是跟踪一个"说话电平包络"：
         * 遇到更响的立刻跟上去，之后以约 7 秒的时间常数慢慢回落。
         * 这样句尾即使变轻，增益也不会跟着抬（不会抽），
         * 换个人说话或换房间时，几秒内就能重新标定。
         * 逐块 RMS 驱动的 AGC 会在音节之间反复上下，是"模糊"和"抽气"的根源。 */
        if (speech && rms > gSpeechEnv)
        {
            gSpeechEnv = rms;
        }
        else
        {
            gSpeechEnv *= 0.999f;
            if (gSpeechEnv < 1.0e-5f) gSpeechEnv = 1.0e-5f;
        }

        {
            Float32 desired = kAGC_TargetRMS / gSpeechEnv;
            if (desired < kAGC_MinGain) desired = kAGC_MinGain;
            if (desired > kAGC_MaxGain) desired = kAGC_MaxGain;

            /* 往下压快一些（防削波），往上抬慢一些（防噪声被抬起来） */
            Float32 coef = (desired < gAGC_Gain) ? 0.15f : 0.03f;
            gAGC_Gain += (desired - gAGC_Gain) * coef;
        }

        /* 门限：开口要快（一个块内基本打开），关闭慢一些（约 200 ms），听感自然 */
        /* 开口快（一块内基本全开），关闭慢（约 500 ms 落下），
         * 再叠加前面的 320 ms 保持，尾音就完整了。 */
        Float32 gateTarget = speech ? 1.0f : kGateIdleGain;
        gGateGain += (gateTarget - gGateGain) * (speech ? 0.5f : 0.02f);

        /* 再给一个"噪声不许被放大超过 ceiling"的硬上限 */
        Float32 gainCeil = kGateNoiseCeiling / ((gNoiseFloor > 1.0e-6f) ? gNoiseFloor : 1.0e-6f);
        Float32 agc = (gAGC_Gain < gainCeil) ? gAGC_Gain : gainCeil;

        Float32 outGain = gMute_Input_Master_Value ? 0.0f : (agc * gGateGain * gVolume_Input_Master_Value);

        for (UInt32 i = 0; i < n; i++)
        {
            procStart[i] = ACPMic_Clip(procStart[i] * outGain);
            Float32 a = (procStart[i] < 0.0f) ? -procStart[i] : procStart[i];
            if (a > gProbe_Peak) gProbe_Peak = a;
        }
    }
}

#pragma mark - 属性辅助

static void ACPMic_SetUInt32Property(UInt32 inValue, UInt32 *outDataSize, void *outData)
{
    *(UInt32 *)outData = inValue;
    *outDataSize = sizeof(UInt32);
}

static void ACPMic_SetStringProperty(CFStringRef inValue, UInt32 *outDataSize, void *outData)
{
    *(CFStringRef *)outData = inValue;
    *outDataSize = sizeof(CFStringRef);
}

static UInt32 ACPMic_ChannelLayoutSize(void)
{
    return (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                    + kDevice_Channels * sizeof(AudioChannelDescription));
}

/* 全局作用域下返回"所有 buffer 之和"，输入 2 声道。 */
static UInt32 ACPMic_StreamConfigurationSize(AudioObjectPropertyScope inScope)
{
    if (inScope == kAudioObjectPropertyScopeInput || inScope == kAudioObjectPropertyScopeGlobal)
    {
        return (UInt32)(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer));
    }
    return (UInt32)offsetof(AudioBufferList, mBuffers);
}

static void ACPMic_FillStreamConfiguration(AudioObjectPropertyScope inScope,
                                           UInt32 *outDataSize, void *outData)
{
    AudioBufferList *list = (AudioBufferList *)outData;

    if (inScope == kAudioObjectPropertyScopeInput || inScope == kAudioObjectPropertyScopeGlobal)
    {
        list->mNumberBuffers = 1;
        list->mBuffers[0].mNumberChannels = kDevice_Channels;
        list->mBuffers[0].mDataByteSize = 0;   /* 只是描述，HAL 不使用 */
        list->mBuffers[0].mData = NULL;
    }
    else
    {
        list->mNumberBuffers = 0;
    }
    *outDataSize = ACPMic_StreamConfigurationSize(inScope);
}

static void ACPMic_FillStreamFormat(AudioStreamBasicDescription *outFormat)
{
    memset(outFormat, 0, sizeof(*outFormat));
    outFormat->mSampleRate       = (Float64)kDevice_SampleRate;
    outFormat->mFormatID         = kAudioFormatLinearPCM;
    outFormat->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    outFormat->mBytesPerPacket   = kDevice_BytesPerFrame;
    outFormat->mFramesPerPacket  = 1;
    outFormat->mBytesPerFrame    = kDevice_BytesPerFrame;
    outFormat->mChannelsPerFrame = kDevice_Channels;
    outFormat->mBitsPerChannel   = 32;
    outFormat->mReserved         = 0;
}

static void ACPMic_FillRangedFormat(AudioStreamRangedDescription *outDescription)
{
    ACPMic_FillStreamFormat(&outDescription->mFormat);
    outDescription->mSampleRateRange.mMinimum = (Float64)kDevice_SampleRate;
    outDescription->mSampleRateRange.mMaximum = (Float64)kDevice_SampleRate;
}

/* 判断 ASBD 是否就是我们要的那一种格式；SetPropertyData 用它做校验 */
static Boolean ACPMic_FormatIsSupported(const AudioStreamBasicDescription *inFormat)
{
    return inFormat->mFormatID == kAudioFormatLinearPCM
        && inFormat->mSampleRate == (Float64)kDevice_SampleRate
        && inFormat->mFormatFlags == (kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian)
        && inFormat->mBytesPerPacket == kDevice_BytesPerFrame
        && inFormat->mFramesPerPacket == 1
        && inFormat->mBytesPerFrame == kDevice_BytesPerFrame
        && inFormat->mChannelsPerFrame == kDevice_Channels
        && inFormat->mBitsPerChannel == 32;
}

static Float32 ACPMic_DecibelsToScalar(Float32 inDecibels)
{
    if (inDecibels <= kVolume_MinDB)
    {
        return 0.0f;
    }
    if (inDecibels > kVolume_MaxDB)
    {
        inDecibels = kVolume_MaxDB;
    }
    return powf(10.0f, inDecibels / 20.0f);
}

static Float32 ACPMic_ScalarToDecibels(Float32 inScalar)
{
    if (inScalar <= 0.0f)
    {
        return kVolume_MinDB;
    }
    Float32 decibels = 20.0f * log10f(inScalar);
    if (decibels < kVolume_MinDB) decibels = kVolume_MinDB;
    if (decibels > kVolume_MaxDB) decibels = kVolume_MaxDB;
    return decibels;
}

static void ACPMic_NotifyPropertyChanged(AudioObjectID inObjectID,
                                         UInt32 inNumberAddresses,
                                         const AudioObjectPropertyAddress *inAddresses)
{
    if (gPlugIn_Host != NULL)
    {
        (void)gPlugIn_Host->PropertiesChanged(gPlugIn_Host, inObjectID, inNumberAddresses, inAddresses);
    }
}

#pragma mark - QueryInterface / 生命周期

static HRESULT ACPMic_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
    (void)inDriver;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL)
    {
        return E_NOINTERFACE;
    }

    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID))
    {
        pthread_mutex_lock(&gPlugIn_StateMutex);
        gPlugIn_RefCount++;
        pthread_mutex_unlock(&gPlugIn_StateMutex);

        if (outInterface != NULL)
        {
            *outInterface = gAudioServerPlugInDriverRef;
        }
        result = S_OK;
    }

    CFRelease(requested);
    return result;
}

static ULONG ACPMic_AddRef(void *inDriver)
{
    (void)inDriver;
    pthread_mutex_lock(&gPlugIn_StateMutex);
    gPlugIn_RefCount++;
    ULONG count = gPlugIn_RefCount;
    pthread_mutex_unlock(&gPlugIn_StateMutex);
    return count;
}

static ULONG ACPMic_Release(void *inDriver)
{
    (void)inDriver;
    pthread_mutex_lock(&gPlugIn_StateMutex);
    if (gPlugIn_RefCount > 0)
    {
        gPlugIn_RefCount--;
    }
    ULONG count = gPlugIn_RefCount;
    pthread_mutex_unlock(&gPlugIn_StateMutex);
    /* 接口表是静态存储，引用计数归零也不 free：宿主可能在卸载时再来一次 Release */
    return count;
}

static OSStatus ACPMic_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    (void)inDriver;
    gPlugIn_Host = inHost;

    ACPMic_UpdateHostClock();

    /* 在 Initialize 里就放好时间锚点，这样 HAL 在 StartIO 之前查询零时间戳
     * 也能得到合法且连续的结果。 */
    pthread_mutex_lock(&gDevice_TimeMutex);
    gDevice_AnchorHostTime = mach_absolute_time();
    gDevice_AnchorSampleTime = 0.0;
    pthread_mutex_unlock(&gDevice_TimeMutex);

    ACPMic_Log("插件已加载（%s，%u Hz / %u ch / float32，输入专用）",
               kDevice_Name_String, (unsigned)kDevice_SampleRate, (unsigned)kDevice_Channels);
    return 0;
}

/* 固定设备列表的插件不支持动态增删设备 */
static OSStatus ACPMic_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                    const AudioServerPlugInClientInfo *inClientInfo,
                                    AudioObjectID *outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ACPMic_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ACPMic_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                       const AudioServerPlugInClientInfo *inClientInfo)
{
    (void)inDriver; (void)inClientInfo;
    return (inDeviceObjectID == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError;
}

static OSStatus ACPMic_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                          const AudioServerPlugInClientInfo *inClientInfo)
{
    (void)inDriver; (void)inClientInfo;
    return (inDeviceObjectID == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError;
}

static OSStatus ACPMic_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                        AudioObjectID inDeviceObjectID,
                                                        UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inDriver; (void)inChangeAction; (void)inChangeInfo;
    return (inDeviceObjectID == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError;
}

static OSStatus ACPMic_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inDriver; (void)inChangeAction; (void)inChangeInfo;
    return (inDeviceObjectID == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError;
}

#pragma mark - HasProperty

static Boolean ACPMic_HasPlugInProperty(const AudioObjectPropertyAddress *inAddress)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioPlugInPropertyBundleID:
            return true;
        default:
            return false;
    }
}

static Boolean ACPMic_HasDeviceProperty(const AudioObjectPropertyAddress *inAddress)
{
    /* 数据源：Siri 的 CoreSpeech 会按 input 作用域查 'ssc#'/'ssrc'，
     * 但 HAL 自己也可能用 global 作用域探测一次，所以两种都认。 */
    if (inAddress->mSelector == kAudioDevicePropertyDataSource
        || inAddress->mSelector == kAudioDevicePropertyDataSources)
    {
        return true;
    }

    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyStreamConfiguration:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyBufferFrameSizeRange:
        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
            return true;
        default:
            return false;
    }
}

static Boolean ACPMic_HasStreamProperty(const AudioObjectPropertyAddress *inAddress)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        default:
            return false;
    }
}

static Boolean ACPMic_HasControlProperty(AudioObjectID inObjectID,
                                         const AudioObjectPropertyAddress *inAddress)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
            return true;
        default:
            break;
    }

    if (inObjectID == kObjectID_Volume_Input_Master)
    {
        switch (inAddress->mSelector)
        {
            case kAudioLevelControlPropertyScalarValue:
            case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyDecibelRange:
            case kAudioLevelControlPropertyConvertScalarToDecibels:
            case kAudioLevelControlPropertyConvertDecibelsToScalar:
                return true;
            default:
                return false;
        }
    }
    else if (inObjectID == kObjectID_Mute_Input_Master)
    {
        return inAddress->mSelector == kAudioBooleanControlPropertyValue;
    }
    else if (inObjectID == kObjectID_DataSource_Input)
    {
        switch (inAddress->mSelector)
        {
            case kAudioSelectorControlPropertyAvailableItems:
            case kAudioSelectorControlPropertyCurrentItem:
            case kAudioSelectorControlPropertyItemName:
            case kAudioSelectorControlPropertyItemKind:
                return true;
            default:
                return false;
        }
    }

    return false;
}

static Boolean ACPMic_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                   pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress)
{
    (void)inDriver;
    (void)inClientProcessID;

    if (inAddress == NULL)
    {
        return false;
    }

    switch (inObjectID)
    {
        case kObjectID_PlugIn:
            return ACPMic_HasPlugInProperty(inAddress);
        case kObjectID_Device:
            return ACPMic_HasDeviceProperty(inAddress);
        case kObjectID_Stream_Input:
            return ACPMic_HasStreamProperty(inAddress);
        case kObjectID_Volume_Input_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_DataSource_Input:
            return ACPMic_HasControlProperty(inObjectID, inAddress);
        default:
            return false;
    }
}

#pragma mark - IsPropertySettable

static OSStatus ACPMic_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                          pid_t inClientProcessID,
                                          const AudioObjectPropertyAddress *inAddress,
                                          Boolean *outIsSettable)
{
    (void)inDriver;
    (void)inClientProcessID;

    if (inAddress == NULL || outIsSettable == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    *outIsSettable = false;

    switch (inObjectID)
    {
        case kObjectID_Device:
            if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate
                || inAddress->mSelector == kAudioDevicePropertyBufferFrameSize)
            {
                *outIsSettable = true;
            }
            return 0;

        case kObjectID_Stream_Input:
            if (inAddress->mSelector == kAudioStreamPropertyIsActive
                || inAddress->mSelector == kAudioStreamPropertyVirtualFormat
                || inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)
            {
                *outIsSettable = true;
            }
            return 0;

        case kObjectID_Volume_Input_Master:
            if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue
                || inAddress->mSelector == kAudioLevelControlPropertyDecibelValue)
            {
                *outIsSettable = true;
            }
            return 0;

        case kObjectID_Mute_Input_Master:
            if (inAddress->mSelector == kAudioBooleanControlPropertyValue)
            {
                *outIsSettable = true;
            }
            return 0;

        case kObjectID_DataSource_Input:
            if (inAddress->mSelector == kAudioSelectorControlPropertyCurrentItem)
            {
                *outIsSettable = true;
            }
            return 0;

        default:
            return kAudioHardwareBadObjectError;
    }
}

#pragma mark - GetPropertyDataSize

static OSStatus ACPMic_PlugInDataSize(const AudioObjectPropertyAddress *inAddress, UInt32 *outDataSize)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioClassID);
            break;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioPlugInPropertyBundleID:
            *outDataSize = sizeof(CFStringRef);
            break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            *outDataSize = sizeof(AudioObjectID);
            break;
        default:
            *outDataSize = 0;
            break;
    }
    return 0;
}

static OSStatus ACPMic_DeviceDataSize(const AudioObjectPropertyAddress *inAddress, UInt32 *outDataSize)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
            *outDataSize = sizeof(UInt32);
            break;

        /* Siri 门槛：输入数据源（单个 + 列表），都答 1 个 UInt32 */
        case kAudioDevicePropertyDataSource:
        case kAudioDevicePropertyDataSources:
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 4 * sizeof(AudioObjectID);      /* 输入流 + 音量 + 静音 + 数据源控件 */
            break;

        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = sizeof(AudioObjectID);
            break;

        /* 只有输入流，所以 global 作用域和 input 作用域给出同一份列表；
         * HAL 两种都会查，只答 input 会让部分 client 认为设备没有流。 */
        case kAudioDevicePropertyStreams:
            *outDataSize = (inAddress->mScope == kAudioObjectPropertyScopeInput
                            || inAddress->mScope == kAudioObjectPropertyScopeGlobal)
                         ? sizeof(AudioObjectID) : 0;
            break;

        case kAudioObjectPropertyControlList:
            *outDataSize = (inAddress->mScope == kAudioObjectPropertyScopeInput
                            || inAddress->mScope == kAudioObjectPropertyScopeGlobal)
                         ? 3 * sizeof(AudioObjectID) : 0;
            break;

        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            break;

        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyBufferFrameSizeRange:
            *outDataSize = sizeof(AudioValueRange);
            break;

        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = kDevice_Channels * sizeof(UInt32);
            break;

        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = ACPMic_ChannelLayoutSize();
            break;

        case kAudioDevicePropertyStreamConfiguration:
            *outDataSize = ACPMic_StreamConfigurationSize(inAddress->mScope);
            break;

        default:
            *outDataSize = 0;
            break;
    }
    return 0;
}

static OSStatus ACPMic_StreamDataSize(const AudioObjectPropertyAddress *inAddress, UInt32 *outDataSize)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioObjectPropertyName:
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            break;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription);
            break;

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = sizeof(AudioStreamRangedDescription);
            break;

        default:
            *outDataSize = 0;
            break;
    }
    return 0;
}

static OSStatus ACPMic_ControlDataSize(AudioObjectID inObjectID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 *outDataSize)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
            *outDataSize = sizeof(UInt32);
            break;

        case kAudioObjectPropertyName:
            *outDataSize = sizeof(CFStringRef);
            break;

        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
            *outDataSize = sizeof(Float32);
            break;

        case kAudioLevelControlPropertyDecibelRange:
            *outDataSize = sizeof(AudioValueRange);
            break;

        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            *outDataSize = sizeof(Float32);
            break;

        case kAudioBooleanControlPropertyValue:
            *outDataSize = (inObjectID == kObjectID_Mute_Input_Master) ? sizeof(UInt32) : 0;
            break;

        case kAudioSelectorControlPropertyAvailableItems:
        case kAudioSelectorControlPropertyCurrentItem:
            *outDataSize = (inObjectID == kObjectID_DataSource_Input) ? sizeof(UInt32) : 0;
            break;

        case kAudioSelectorControlPropertyItemName:
            *outDataSize = (inObjectID == kObjectID_DataSource_Input) ? sizeof(CFStringRef) : 0;
            break;

        case kAudioSelectorControlPropertyItemKind:
            *outDataSize = (inObjectID == kObjectID_DataSource_Input) ? sizeof(AudioClassID) : 0;
            break;

        default:
            *outDataSize = 0;
            break;
    }
    return 0;
}

static OSStatus ACPMic_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                           pid_t inClientProcessID,
                                           const AudioObjectPropertyAddress *inAddress,
                                           UInt32 inQualifierDataSize, const void *inQualifierData,
                                           UInt32 *outDataSize)
{
    (void)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;

    if (inAddress == NULL || outDataSize == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    switch (inObjectID)
    {
        case kObjectID_PlugIn:
            return ACPMic_PlugInDataSize(inAddress, outDataSize);
        case kObjectID_Device:
            return ACPMic_DeviceDataSize(inAddress, outDataSize);
        case kObjectID_Stream_Input:
            return ACPMic_StreamDataSize(inAddress, outDataSize);
        case kObjectID_Volume_Input_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_DataSource_Input:
            return ACPMic_ControlDataSize(inObjectID, inAddress, outDataSize);
        default:
            return kAudioHardwareBadObjectError;
    }
}

#pragma mark - GetPropertyData

static OSStatus ACPMic_GetPlugInPropertyData(const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
            ACPMic_SetUInt32Property(kAudioObjectClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyClass:
            ACPMic_SetUInt32Property(kAudioPlugInClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyOwner:
            ACPMic_SetUInt32Property(kAudioObjectUnknown, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyName:
            ACPMic_SetStringProperty(CFSTR(kDevice_Manufacturer_String " ACP 插件"), outDataSize, outData);
            return 0;
        case kAudioObjectPropertyManufacturer:
            ACPMic_SetStringProperty(CFSTR(kDevice_Manufacturer_String), outDataSize, outData);
            return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (inDataSize < sizeof(AudioObjectID))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            *(AudioObjectID *)outData = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice:
        {
            /* qualifier 是要翻译的 UID；只有我们的设备返回有效 ID */
            AudioObjectID result = kAudioObjectUnknown;
            if (inDataSize >= sizeof(AudioObjectID))
            {
                CFStringRef uid = NULL;
                if (inQualifierDataSize == sizeof(CFStringRef) && inQualifierData != NULL)
                {
                    uid = *(CFStringRef *)inQualifierData;
                }
                if (uid != NULL && CFEqual(uid, CFSTR(kDevice_UID_String)))
                {
                    result = kObjectID_Device;
                }
                *(AudioObjectID *)outData = result;
                *outDataSize = sizeof(AudioObjectID);
                return 0;
            }
            return kAudioHardwareBadPropertySizeError;
        }
        case kAudioPlugInPropertyResourceBundle:
            /* 返回相对路径的空串表示资源就在 bundle 自身里 */
            ACPMic_SetStringProperty(CFSTR(""), outDataSize, outData);
            return 0;
        case kAudioPlugInPropertyBundleID:
            ACPMic_SetStringProperty(CFSTR(kPlugIn_BundleID_String), outDataSize, outData);
            return 0;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus ACPMic_GetDevicePropertyData(const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
            ACPMic_SetUInt32Property(kAudioObjectClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyClass:
            ACPMic_SetUInt32Property(kAudioDeviceClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyOwner:
            ACPMic_SetUInt32Property(kObjectID_PlugIn, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyName:
            ACPMic_SetStringProperty(CFSTR(kDevice_Name_String), outDataSize, outData);
            return 0;
        case kAudioObjectPropertyManufacturer:
            ACPMic_SetStringProperty(CFSTR(kDevice_Manufacturer_String), outDataSize, outData);
            return 0;
        case kAudioDevicePropertyDeviceUID:
            ACPMic_SetStringProperty(CFSTR(kDevice_UID_String), outDataSize, outData);
            return 0;
        case kAudioDevicePropertyModelUID:
            ACPMic_SetStringProperty(CFSTR(kDevice_ModelUID_String), outDataSize, outData);
            return 0;

        case kAudioObjectPropertyOwnedObjects:
        {
            const AudioObjectID objects[4] = {
                kObjectID_Stream_Input,
                kObjectID_Volume_Input_Master,
                kObjectID_Mute_Input_Master,
                kObjectID_DataSource_Input
            };
            UInt32 needed = (UInt32)sizeof(objects);
            if (inDataSize < needed)
            {
                return kAudioHardwareBadPropertySizeError;
            }
            memcpy(outData, objects, needed);
            *outDataSize = needed;
            return 0;
        }

        case kAudioDevicePropertyTransportType:
            ACPMic_SetUInt32Property(kAudioDeviceTransportTypeBuiltIn, outDataSize, outData);
            return 0;

        /* Siri 门槛：把输入数据源报成内置麦克风 'imic'。
         * 'ssc#'（DataSources 列表）和 'ssrc'（当前 DataSource）两处都必须给。 */
        case kAudioDevicePropertyDataSource:
            ACPMic_SetUInt32Property((UInt32)kACPImic_DataSourceIMic, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyDataSources:
        {
            if (inDataSize < sizeof(UInt32))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            ((UInt32 *)outData)[0] = (UInt32)kACPImic_DataSourceIMic;
            *outDataSize = sizeof(UInt32);
            return 0;
        }

        case kAudioDevicePropertyRelatedDevices:
            if (inDataSize < sizeof(AudioObjectID))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            *(AudioObjectID *)outData = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            return 0;

        case kAudioDevicePropertyClockDomain:
            ACPMic_SetUInt32Property(0, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyDeviceIsAlive:
            ACPMic_SetUInt32Property(1, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
            ACPMic_SetUInt32Property(gDevice_IOIsRunning > 0 ? 1 : 0, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            /* 只有输入方向可以做默认设备 */
            ACPMic_SetUInt32Property(inAddress->mScope == kAudioObjectPropertyScopeInput ? 1 : 0,
                                     outDataSize, outData);
            return 0;

        case kAudioDevicePropertyLatency:
            ACPMic_SetUInt32Property(0, outDataSize, outData);
            return 0;

        case kAudioDevicePropertySafetyOffset:
            ACPMic_SetUInt32Property(0, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyIsHidden:
            ACPMic_SetUInt32Property(0, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyZeroTimeStampPeriod:
            ACPMic_SetUInt32Property(kDevice_ZeroTimeStampPeriod, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyClockAlgorithm:
            /* 我们直接给出硬件锚定的时间戳，不做 IIR 平滑 */
            ACPMic_SetUInt32Property(kAudioDeviceClockAlgorithmRaw, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyBufferFrameSize:
            ACPMic_SetUInt32Property(gDevice_BufferFrameSize, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyBufferFrameSizeRange:
        {
            if (inDataSize < sizeof(AudioValueRange))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            AudioValueRange *range = (AudioValueRange *)outData;
            range->mMinimum = kDevice_MinBufferFrames;
            range->mMaximum = kDevice_MaxBufferFrames;
            *outDataSize = sizeof(AudioValueRange);
            return 0;
        }

        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
            ACPMic_SetUInt32Property(0, outDataSize, outData);
            return 0;

        case kAudioDevicePropertyStreams:
            if (inAddress->mScope == kAudioObjectPropertyScopeInput
                || inAddress->mScope == kAudioObjectPropertyScopeGlobal)
            {
                if (inDataSize < sizeof(AudioObjectID))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                *(AudioObjectID *)outData = kObjectID_Stream_Input;
                *outDataSize = sizeof(AudioObjectID);
            }
            else
            {
                *outDataSize = 0;
            }
            return 0;

        case kAudioObjectPropertyControlList:
            if (inAddress->mScope == kAudioObjectPropertyScopeInput
                || inAddress->mScope == kAudioObjectPropertyScopeGlobal)
            {
                if (inDataSize < 3 * sizeof(AudioObjectID))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                AudioObjectID *list = (AudioObjectID *)outData;
                list[0] = kObjectID_Volume_Input_Master;
                list[1] = kObjectID_Mute_Input_Master;
                list[2] = kObjectID_DataSource_Input;
                *outDataSize = 3 * sizeof(AudioObjectID);
            }
            else
            {
                *outDataSize = 0;
            }
            return 0;

        case kAudioDevicePropertyNominalSampleRate:
            if (inDataSize < sizeof(Float64))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            *(Float64 *)outData = (Float64)kDevice_SampleRate;
            *outDataSize = sizeof(Float64);
            return 0;

        case kAudioDevicePropertyAvailableNominalSampleRates:
        {
            if (inDataSize < sizeof(AudioValueRange))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            AudioValueRange *range = (AudioValueRange *)outData;
            range->mMinimum = (Float64)kDevice_SampleRate;
            range->mMaximum = (Float64)kDevice_SampleRate;
            *outDataSize = sizeof(AudioValueRange);
            return 0;
        }

        case kAudioDevicePropertyPreferredChannelsForStereo:
        {
            if (inDataSize < kDevice_Channels * sizeof(UInt32))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            UInt32 *channels = (UInt32 *)outData;
            channels[0] = 1;   /* 左 */
            channels[1] = 2;   /* 右 */
            *outDataSize = kDevice_Channels * sizeof(UInt32);
            return 0;
        }

        case kAudioDevicePropertyPreferredChannelLayout:
        {
            UInt32 needed = ACPMic_ChannelLayoutSize();
            if (inDataSize < needed)
            {
                return kAudioHardwareBadPropertySizeError;
            }
            AudioChannelLayout *layout = (AudioChannelLayout *)outData;
            memset(layout, 0, needed);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mChannelBitmap = 0;
            layout->mNumberChannelDescriptions = kDevice_Channels;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            layout->mChannelDescriptions[0].mChannelFlags = kAudioChannelFlags_AllOff;
            layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            layout->mChannelDescriptions[1].mChannelFlags = kAudioChannelFlags_AllOff;
            *outDataSize = needed;
            return 0;
        }

        case kAudioDevicePropertyStreamConfiguration:
            ACPMic_FillStreamConfiguration(inAddress->mScope, outDataSize, outData);
            return 0;

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus ACPMic_GetStreamPropertyData(const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
            ACPMic_SetUInt32Property(kAudioObjectClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyClass:
            ACPMic_SetUInt32Property(kAudioStreamClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyOwner:
            ACPMic_SetUInt32Property(kObjectID_Device, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyName:
            ACPMic_SetStringProperty(CFSTR("ACPMic Input"), outDataSize, outData);
            return 0;
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            return 0;
        case kAudioStreamPropertyIsActive:
            ACPMic_SetUInt32Property(1, outDataSize, outData);
            return 0;
        case kAudioStreamPropertyDirection:
            ACPMic_SetUInt32Property(1, outDataSize, outData);   /* 1 = 输入 */
            return 0;
        case kAudioStreamPropertyTerminalType:
            ACPMic_SetUInt32Property(kAudioStreamTerminalTypeMicrophone, outDataSize, outData);
            return 0;
        case kAudioStreamPropertyStartingChannel:
            ACPMic_SetUInt32Property(1, outDataSize, outData);
            return 0;
        case kAudioStreamPropertyLatency:
            ACPMic_SetUInt32Property(0, outDataSize, outData);
            return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        {
            if (inDataSize < sizeof(AudioStreamBasicDescription))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            ACPMic_FillStreamFormat((AudioStreamBasicDescription *)outData);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return 0;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
        {
            if (inDataSize < sizeof(AudioStreamRangedDescription))
            {
                return kAudioHardwareBadPropertySizeError;
            }
            ACPMic_FillRangedFormat((AudioStreamRangedDescription *)outData);
            *outDataSize = sizeof(AudioStreamRangedDescription);
            return 0;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus ACPMic_GetControlPropertyData(AudioObjectID inObjectID,
                                              const AudioObjectPropertyAddress *inAddress,
                                              UInt32 inQualifierDataSize, const void *inQualifierData,
                                              UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    switch (inAddress->mSelector)
    {
        case kAudioObjectPropertyBaseClass:
            ACPMic_SetUInt32Property(inObjectID == kObjectID_DataSource_Input
                                     ? kAudioSelectorControlClassID
                                     : kAudioLevelControlClassID, outDataSize, outData);
            return 0;
        case kAudioObjectPropertyClass:
            ACPMic_SetUInt32Property(inObjectID == kObjectID_DataSource_Input
                                     ? kAudioDataSourceControlClassID
                                     : (inObjectID == kObjectID_Volume_Input_Master
                                        ? kAudioVolumeControlClassID
                                        : kAudioMuteControlClassID), outDataSize, outData);
            return 0;
        case kAudioObjectPropertyOwner:
            ACPMic_SetUInt32Property(kObjectID_Device, outDataSize, outData);
            return 0;
        case kAudioControlPropertyScope:
            ACPMic_SetUInt32Property(kAudioObjectPropertyScopeInput, outDataSize, outData);
            return 0;
        case kAudioControlPropertyElement:
            ACPMic_SetUInt32Property(kAudioObjectPropertyElementMain, outDataSize, outData);
            return 0;
        default:
            break;
    }

    if (inObjectID == kObjectID_Volume_Input_Master)
    {
        switch (inAddress->mSelector)
        {
            case kAudioLevelControlPropertyScalarValue:
                if (inDataSize < sizeof(Float32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                *(Float32 *)outData = gVolume_Input_Master_Value;
                *outDataSize = sizeof(Float32);
                return 0;

            case kAudioLevelControlPropertyDecibelValue:
                if (inDataSize < sizeof(Float32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                *(Float32 *)outData = ACPMic_ScalarToDecibels(gVolume_Input_Master_Value);
                *outDataSize = sizeof(Float32);
                return 0;

            case kAudioLevelControlPropertyDecibelRange:
            {
                if (inDataSize < sizeof(AudioValueRange))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                AudioValueRange *range = (AudioValueRange *)outData;
                range->mMinimum = kVolume_MinDB;
                range->mMaximum = kVolume_MaxDB;
                *outDataSize = sizeof(AudioValueRange);
                return 0;
            }

            case kAudioLevelControlPropertyConvertScalarToDecibels:
            {
                if (inQualifierDataSize < sizeof(Float32) || inQualifierData == NULL || inDataSize < sizeof(Float32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                Float32 scalar = *(const Float32 *)inQualifierData;
                *(Float32 *)outData = ACPMic_ScalarToDecibels(scalar);
                *outDataSize = sizeof(Float32);
                return 0;
            }

            case kAudioLevelControlPropertyConvertDecibelsToScalar:
            {
                if (inQualifierDataSize < sizeof(Float32) || inQualifierData == NULL || inDataSize < sizeof(Float32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                Float32 decibels = *(const Float32 *)inQualifierData;
                *(Float32 *)outData = ACPMic_DecibelsToScalar(decibels);
                *outDataSize = sizeof(Float32);
                return 0;
            }

            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (inObjectID == kObjectID_DataSource_Input)
    {
        switch (inAddress->mSelector)
        {
            case kAudioSelectorControlPropertyAvailableItems:
            {
                if (inDataSize < sizeof(UInt32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                ((UInt32 *)outData)[0] = (UInt32)kACPImic_DataSourceIMic;
                *outDataSize = sizeof(UInt32);
                return 0;
            }

            case kAudioSelectorControlPropertyCurrentItem:
                ACPMic_SetUInt32Property((UInt32)kACPImic_DataSourceIMic, outDataSize, outData);
                return 0;

            case kAudioSelectorControlPropertyItemName:
            {
                UInt32 item = 0;
                if (inQualifierDataSize >= sizeof(UInt32) && inQualifierData != NULL)
                {
                    item = *(const UInt32 *)inQualifierData;
                }
                if (item != 0)
                {
                    return kAudioHardwareIllegalOperationError;
                }
                ACPMic_SetStringProperty(CFSTR("Built-in Microphone"), outDataSize, outData);
                return 0;
            }

            case kAudioSelectorControlPropertyItemKind:
                ACPMic_SetUInt32Property(kAudioDataSourceControlClassID, outDataSize, outData);
                return 0;

            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (inObjectID == kObjectID_Mute_Input_Master
        && inAddress->mSelector == kAudioBooleanControlPropertyValue)
    {
        ACPMic_SetUInt32Property(gMute_Input_Master_Value ? 1 : 0, outDataSize, outData);
        return 0;
    }

    return kAudioHardwareUnknownPropertyError;
}

static OSStatus ACPMic_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize, const void *inQualifierData,
                                       UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    (void)inDriver;
    (void)inClientProcessID;

    if (inAddress == NULL || outDataSize == NULL || outData == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    switch (inObjectID)
    {
        case kObjectID_PlugIn:
            return ACPMic_GetPlugInPropertyData(inAddress, inQualifierDataSize, inQualifierData,
                                                inDataSize, outDataSize, outData);

        case kObjectID_Device:
            return ACPMic_GetDevicePropertyData(inAddress, inDataSize, outDataSize, outData);

        case kObjectID_Stream_Input:
            return ACPMic_GetStreamPropertyData(inAddress, inDataSize, outDataSize, outData);

        case kObjectID_Volume_Input_Master:
        case kObjectID_Mute_Input_Master:
        case kObjectID_DataSource_Input:
            return ACPMic_GetControlPropertyData(inObjectID, inAddress, inQualifierDataSize, inQualifierData,
                                                 inDataSize, outDataSize, outData);

        default:
            return kAudioHardwareBadObjectError;
    }
}

#pragma mark - SetPropertyData

static OSStatus ACPMic_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize, const void *inQualifierData,
                                       UInt32 inDataSize, const void *inData)
{
    (void)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;

    if (inAddress == NULL || inData == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    switch (inObjectID)
    {
        case kObjectID_Device:
            if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
            {
                if (inDataSize < sizeof(Float64))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                Float64 rate = *(const Float64 *)inData;
                return (rate == (Float64)kDevice_SampleRate) ? 0 : kAudioDeviceUnsupportedFormatError;
            }
            if (inAddress->mSelector == kAudioDevicePropertyBufferFrameSize)
            {
                if (inDataSize < sizeof(UInt32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                UInt32 frames = *(const UInt32 *)inData;
                if (frames < kDevice_MinBufferFrames) frames = kDevice_MinBufferFrames;
                if (frames > kDevice_MaxBufferFrames) frames = kDevice_MaxBufferFrames;
                gDevice_BufferFrameSize = frames;

                AudioObjectPropertyAddress changed = *inAddress;
                ACPMic_NotifyPropertyChanged(kObjectID_Device, 1, &changed);
                return 0;
            }
            /* 只有一个数据源 'imic'：HAL 写回来时校验值，然后通知一次 */
            if (inAddress->mSelector == kAudioDevicePropertyDataSource)
            {
                if (inDataSize < sizeof(UInt32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                UInt32 requested = *(const UInt32 *)inData;
                if (requested != (UInt32)kACPImic_DataSourceIMic
                    && requested != (UInt32)kACPImic_DataSourceEMic)
                {
                    return kAudioHardwareIllegalOperationError;
                }
                AudioObjectPropertyAddress changed = *inAddress;
                ACPMic_NotifyPropertyChanged(kObjectID_Device, 1, &changed);
                return 0;
            }
            return kAudioHardwareUnsupportedOperationError;

        case kObjectID_DataSource_Input:
            if (inAddress->mSelector == kAudioSelectorControlPropertyCurrentItem)
            {
                if (inDataSize < sizeof(UInt32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                UInt32 requested = *(const UInt32 *)inData;
                if (requested != (UInt32)kACPImic_DataSourceIMic
                    && requested != (UInt32)kACPImic_DataSourceEMic)
                {
                    return kAudioHardwareIllegalOperationError;
                }
                return 0;
            }
            return kAudioHardwareUnsupportedOperationError;

        case kObjectID_Stream_Input:
            if (inAddress->mSelector == kAudioStreamPropertyIsActive)
            {
                if (inDataSize < sizeof(UInt32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                /* 输入流始终是激活的（单一数据源），但允许 HAL 写入，写完通知一下 */
                AudioObjectPropertyAddress changed = *inAddress;
                ACPMic_NotifyPropertyChanged(kObjectID_Stream_Input, 1, &changed);
                return 0;
            }
            if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat
                || inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)
            {
                if (inDataSize < sizeof(AudioStreamBasicDescription))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                const AudioStreamBasicDescription *format =
                    (const AudioStreamBasicDescription *)inData;
                if (!ACPMic_FormatIsSupported(format))
                {
                    return kAudioDeviceUnsupportedFormatError;
                }
                return 0;
            }
            return kAudioHardwareUnsupportedOperationError;

        case kObjectID_Volume_Input_Master:
        {
            Float32 newScalar = gVolume_Input_Master_Value;

            if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue)
            {
                if (inDataSize < sizeof(Float32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                newScalar = *(const Float32 *)inData;
            }
            else if (inAddress->mSelector == kAudioLevelControlPropertyDecibelValue)
            {
                if (inDataSize < sizeof(Float32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                newScalar = ACPMic_DecibelsToScalar(*(const Float32 *)inData);
            }
            else
            {
                return kAudioHardwareUnsupportedOperationError;
            }

            if (newScalar < 0.0f) newScalar = 0.0f;
            if (newScalar > 1.0f) newScalar = 1.0f;
            gVolume_Input_Master_Value = newScalar;

            AudioObjectPropertyAddress changed[2] = {
                *inAddress,
                { .mSelector = kAudioLevelControlPropertyScalarValue,
                  .mScope = kAudioObjectPropertyScopeInput,
                  .mElement = kAudioObjectPropertyElementMain }
            };
            changed[0].mScope = kAudioObjectPropertyScopeInput;
            changed[0].mElement = kAudioObjectPropertyElementMain;
            ACPMic_NotifyPropertyChanged(kObjectID_Volume_Input_Master, 2, changed);
            return 0;
        }

        case kObjectID_Mute_Input_Master:
            if (inAddress->mSelector == kAudioBooleanControlPropertyValue)
            {
                if (inDataSize < sizeof(UInt32))
                {
                    return kAudioHardwareBadPropertySizeError;
                }
                gMute_Input_Master_Value = (*(const UInt32 *)inData) != 0;

                AudioObjectPropertyAddress changed = *inAddress;
                ACPMic_NotifyPropertyChanged(kObjectID_Mute_Input_Master, 1, &changed);
                return 0;
            }
            return kAudioHardwareUnsupportedOperationError;

        default:
            return kAudioHardwareBadObjectError;
    }
}

#pragma mark - IO 生命周期

static OSStatus ACPMic_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                               UInt32 inClientID)
{
    (void)inDriver;
    (void)inClientID;

    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }

    /* 时间轴：把上一个锚点算到当前采样时刻，再加一个周期作为新锚点，
     * 保证多次 Start/Stop 之后 sampleTime 仍然单调递增（HAL 会检查这个）。 */
    UInt64 now = mach_absolute_time();
    pthread_mutex_lock(&gDevice_TimeMutex);
    Float64 currentSampleTime = 0.0;
    UInt64 currentHostTime = 0;
    ACPMic_ComputeTimeStampLocked(now, &currentSampleTime, &currentHostTime);
    gDevice_AnchorSampleTime = currentSampleTime + (Float64)kDevice_ZeroTimeStampPeriod;
    gDevice_AnchorHostTime = now;
    pthread_mutex_unlock(&gDevice_TimeMutex);

    pthread_mutex_lock(&gDevice_IOMutex);
    if (gDevice_IOIsRunning == 0)
    {
        /* 第一个 client：真正去开 kext。失败也没关系，DoIO 会输出静音。 */
        (void)ACPMic_StartCaptureLocked();
    }
    gDevice_IOIsRunning++;
    pthread_mutex_unlock(&gDevice_IOMutex);

    return 0;
}

static OSStatus ACPMic_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                              UInt32 inClientID)
{
    (void)inDriver;
    (void)inClientID;

    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }

    pthread_mutex_lock(&gDevice_IOMutex);
    if (gDevice_IOIsRunning == 0)
    {
        pthread_mutex_unlock(&gDevice_IOMutex);
        return kAudioHardwareIllegalOperationError;
    }
    gDevice_IOIsRunning--;
    if (gDevice_IOIsRunning == 0)
    {
        /* 所有 client 都停了：停 DMA、解映射、关连接，不占着硬件不放 */
        ACPMic_CloseCaptureLocked(true);
        ACPMic_ResetDCBlocker();
    }
    pthread_mutex_unlock(&gDevice_IOMutex);

    return 0;
}

static OSStatus ACPMic_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                        UInt32 inClientID, Float64 *outSampleTime,
                                        UInt64 *outHostTime, UInt64 *outSeed)
{
    (void)inDriver;
    (void)inClientID;

    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }
    if (outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    UInt64 now = mach_absolute_time();

    pthread_mutex_lock(&gDevice_TimeMutex);
    ACPMic_ComputeTimeStampLocked(now, outSampleTime, outHostTime);
    *outSeed = gDevice_ZeroSeed;
    pthread_mutex_unlock(&gDevice_TimeMutex);

    return 0;
}

static OSStatus ACPMic_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                         UInt32 inClientID, UInt32 inOperationID,
                                         Boolean *outWillDo, Boolean *outWillDoInPlace)
{
    (void)inDriver;
    (void)inClientID;

    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }
    if (outWillDo == NULL || outWillDoInPlace == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    /* 设备只有输入流，所以只承诺 ReadInput；HAL 会把 ioMainBuffer 直接交给我们填。 */
    if (inOperationID == kAudioServerPlugInIOOperationReadInput)
    {
        *outWillDo = true;
        *outWillDoInPlace = true;
    }
    else
    {
        *outWillDo = false;
        *outWillDoInPlace = false;
    }
    return 0;
}

static OSStatus ACPMic_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                        UInt32 inClientID, UInt32 inOperationID,
                                        UInt32 inIOBufferFrameSize,
                                        const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inDriver;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;

    return (inDeviceObjectID == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError;
}

static OSStatus ACPMic_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     AudioObjectID inStreamObjectID, UInt32 inClientID,
                                     UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                     const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                     void *ioMainBuffer, void *ioSecondaryBuffer)
{
    (void)inDriver;
    (void)inClientID;
    (void)inIOCycleInfo;
    (void)ioSecondaryBuffer;

    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }
    if (inStreamObjectID != kObjectID_Stream_Input)
    {
        return kAudioHardwareBadStreamError;
    }
    if (inOperationID != kAudioServerPlugInIOOperationReadInput)
    {
        return kAudioHardwareUnsupportedOperationError;
    }
    if (ioMainBuffer == NULL || inIOBufferFrameSize == 0)
    {
        return kAudioHardwareIllegalOperationError;
    }

    pthread_mutex_lock(&gDevice_IOMutex);
    ACPMic_FillInputBufferLocked(inIOBufferFrameSize, (Float32 *)ioMainBuffer);
    pthread_mutex_unlock(&gDevice_IOMutex);

    return 0;
}

static OSStatus ACPMic_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID, UInt32 inOperationID,
                                      UInt32 inIOBufferFrameSize,
                                      const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inDriver;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;

    return (inDeviceObjectID == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError;
}

#pragma mark - CFPlugIn 工厂

/* 名字必须与 Info.plist 里 CFPlugInFactories 的值一致。
 * 返回的是驱动引用（指向接口表指针的指针），不是接口表本身。 */
__attribute__((visibility("default")))
void *ACPMic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;

    if (inRequestedTypeUUID != NULL && CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
    {
        return gAudioServerPlugInDriverRef;
    }
    return NULL;
}
