# macOS 的 ACP6x PDM 内置麦克风驱动

[English](README.md) | **简体中文**

给 Ryzen 笔记本上那颗挂在 **AMD ACP6x / ACP6.3（Pink Sardine 世代）** 上的 **PDM 数字麦克风阵列**写的 macOS 驱动。分两层：

- **`ACPCapture.kext`** —— 一个很小的 PCI kext，只负责配置 ACP 的 DMA 引擎，把 PCM 搬进内存；
- **`ACPMicPlugin.driver`** —— 一个 input-only 的 HAL 插件（AudioServerPlugin），把取到的数据做 DSP 后暴露成普通的 CoreAudio 输入设备。

在 **HP OMEN 16-ap1xxx**（主板 `8F06`，Ryzen 9 9955HX，RTX 5070 Laptop，macOS 26.7.3）上实测通过。

## 支持状态

| 功能 | 状态 |
|---|---|
| 从 PDM 阵列采集 48 kHz / 2ch / S32_LE | ✅ 可用 |
| 作为系统输入设备（语音备忘录、QuickTime、Zoom、浏览器…） | ✅ 可用 |
| Siri 使用内置麦克风 | ✅ 可用（需要 `'imic'` 数据源控件，见下文） |
| macOS 听写 / 输入法语音 | ✅ 可用 |
| 硬件 AEC / 波束成形 | ❌ 未实现 |
| 睡眠唤醒后自动重连 | ⚠️ 唤醒后需要重新打开设备（`StopIO` / `StartIO` 会重新上膛） |

不需要苹果开发者账号，不需要 Xcode，也不需要 DriverKit 授权：全部用 Command Line Tools + `clang` 就能编。

## 为什么需要自己写

很多 Ryzen 笔记本的模拟 codec（比如 Realtek ALC245）**根本没有接麦克风**——内置麦克风是一组 PDM 阵列，挂在 AMD Audio CoProcessor 下面。Windows 走 ACPDM 端点驱动它，Linux 走 `snd_pci_acp6x` + `dmic-codec`；macOS 两条都没有，于是 codec 的输入节点存在、但录出来是纯静音。

更麻烦的是，这类机器的 ACPI 经常**根本没声明 PDM 设备**（没有 `_WOV`、没有 `acp-audio-device-type`），所以 Linux 那边才需要一个针对本主板的 quirk：[`acp6x-omen-dkms`](https://github.com/NevetScar/acp6x-omen-dkms) 用 `DMI_BOARD_NAME = "8F06"` 直接跳过 ACPI 检查。

本驱动更进一步：**完全不看 ACPI**，直接和寄存器打交道。

## 先判断你的机器行不行

满足这三条基本就能用：

1. ACP 的 PCI 设备是 `pci1022,15e2`：
   ```sh
   ioreg -rc IOPCIDevice -l | grep -B2 -A2 '15e2'   # 或 lspci -nn | grep -i audio
   ```
2. `ACP_PIN_CONFIG`（偏移 `0x01440`）的读值**不在** `{0,1,2,3,9,15}` 里 —— 这是 BIOS 已把 ACP 配成 PDM 模式的标志；
3. BIOS 里麦克风是能用的（Windows 下录音正常）。

第 2 条由 `probe/` 里的只读探针负责验证，**先跑探针再装采集 kext**，见下面的安装步骤。

## 安装

```sh
# 0. 编译前提：装了 Command Line Tools 即可（xcode-select --install），不需要完整 Xcode
clang --version

# 1. 先诊断（只读）：注入探针 kext
cd probe && ./build.sh
cd .. && sudo ./tools/install-probe.sh install
# 重启后
ioreg -rc ACPProbe -l | grep -E "pinConfig|pgfsmStatus|barLen"
#   pinConfig   = 4        → BIOS 已配成 PDM，可以继续
#   pgfsmStatus = 0        → ACP 已上电，不需要任何电源序列

# 2. 采集 kext
cd capture && ./build.sh
cd .. && sudo ./tools/install-capture.sh install
# 重启

# 3. HAL 插件（马上生效，不用重启）
cd plugin && ./build.sh && sudo ./install.sh
```

两个 `install-*.sh` 会做这些事：备份 `config.plist` → 拷贝 kext 到 `EFI/OC/Kexts/` 并**逐文件校验大小**（FAT 上丢文件是常见事故）→ 在 `Kernel → Add` 里加/删条目 → `plutil -lint` + `ocvalidate` 校验，失败自动回滚。

如果你不用 OpenCore，就自己把 `ACPCapture.kext` 注入内核（需要 `_kmod_info`，见下文）。

验证：

```sh
system_profiler SPAudioDataType | grep -A3 "AMD ACP PDM"
#   应该出现 AMD ACP PDM Microphone（2 通道 / 48000）

ioreg -rc ACPCapture -l | grep -E 'writeBytes|dmaEnable|ticks'
#   writeBytes 在涨、dmaEnable=3、ticks 在涨 → DMA 正常
#   全是 0 → DMA 没起来（见「故障排查」）

cd tools/acpcap && clang -O2 -o acpcap acpcap.c -framework IOKit -framework CoreFoundation
./acpcap --status
./acpcap 5 /tmp/test.wav    # 直接录 5 秒（已做 DC 阻断）
```

## 架构

```
用户态   ACPMicPlugin.driver（HAL 插件，input-only）
           │  IOServiceOpen(ACPCapture) + IOConnectMapMemory64(环) + 标量方法
           ▼
内核     ACPCapture.kext（pci1022,15e2 上的 IOService，只搬数据不做 DSP）
           │  BAR0 MMIO：写页表 → 配环 → 使能 PDM → 轮询线性位置
           ▼
硬件     AMD ACP6x PDM DMA 引擎 → 数字 MEMS 麦克风阵列
```

为什么把 DSP 放在用户态：kext 里写 `IOAudioFamily` 是两千行的活、每次改动都要重启，而且 panic 一次就得回滚 EFI。把"DSP + 设备暴露"交给 HAL 插件后，**调音只改插件、`launchctl kickstart` 一下就能听效果**。

## 关键寄存器

偏移都**相对于 BAR0**（见 `regs/acp63_regs.h`）。注意：Linux 老头文件里写的 `0x1242C04` 是**绝对地址**，`acp6x_readl(base + REG)` 会减掉 `ACP6x_PHY_BASE_ADDRESS (0x1240000)`，实际访问就是 BAR0 + `0x2C04` —— 所以 256 KB 的 BAR 完全够用。

| 偏移 | 寄存器 | 说明 |
|---|---|---|
| `0x01028` | `ACP_PGFSM_STATUS` | 读 `0` = 已上电，不需要电源序列 |
| `0x01440` | `ACP_PIN_CONFIG` | 不在 `{0,1,2,3,9,15}` ⇒ PDM 模式 |
| `0x02C04` | `ACP_WOV_PDM_ENABLE` | 写 `1` |
| `0x02C08` | `ACP_WOV_PDM_DMA_ENABLE` | 写 `1`，然后轮询 bit1 |
| `0x02C0C` | `ACP_WOV_RX_RINGBUFADDR` | 环的物理地址 |
| `0x02C14` | `ACP_WOV_RX_LINKPOSITIONCNTR` | 环内位置 |
| `0x02C18/1C` | `ACP_WOV_RX_LINEARPOSITIONCNTR` | 绝对字节计数（算增量用这个） |
| `0x02C24` | `ACP_WOV_PDM_FIFO_FLUSH` | 使能前写一次，去掉开机爆音 |
| `0x02C28` | `ACP_WOV_PDM_NO_OF_CHANNELS` | 写 `0`（TWO_CH）；固件留的是 `1` |
| `0x02C2C` | `ACP_WOV_PDM_DECIMATION_FACTOR` | 写 `2` |
| `0x02C5C` | `ACP_WOV_MISC_CTRL` | 增益在 bit 4:3 |
| `0x02C60` | `ACP_WOV_CLK_CTRL` | 写 `7` |
| `0x0010000` | `ACP_SCRATCH_REG_0` | 页表入口（low / high\|BIT31），每个 4 KB 页 8 字节 |

DMA 缓冲区不能直接给物理地址：引擎会走 ACP SRAM 窗口里的**页表**，由 kext 一页一页填。

完整的寄存器 dump、启动序列和"三个被推翻的判断"写在 [`docs/DESIGN.md`](docs/DESIGN.md)。

## 三道坎（写 kext 时最容易卡住的地方）

### 1. 没有 Xcode 也要有 `_kmod_info`

`ld -kext` 在只有 Command Line Tools 的机器上**不会**生成 `_kmod_info`。没有它，OpenCore 只打一行：

```
OC: Prelinked injection ACPCapture.kext (...) - Invalid Parameter
```

真实原因是 `Kext->Context.VirtualKmod == 0` → 内部返回 `EFI_UNSUPPORTED`，被上层折成了 `EFI_INVALID_PARAMETER`（报错信息骗人）。

解法是按 `KMOD_INFO_64_V1` 的布局手写一个（见 `capture/kmod_info.cpp`）：

```c
#pragma pack(4)
struct KmodInfo {           /* 196 字节，0xC4 */
  void *next;               /* 0   */
  uint32_t info_version;    /* 8   = 1 */
  uint32_t id;              /* 12  = 0xffffffff */
  char name[64];            /* 16  */
  char version[64];         /* 80  */
  int32_t reference_count;  /* 144 = -1 */
  void *reference_list;     /* 148 */
  uint64_t address;         /* 156 */
  uint64_t size;            /* 164 */
  uint64_t hdr_size;        /* 172 */
  void *start;              /* 180 */
  void *stop;               /* 188 */
};
__attribute__((used, section("__DATA,__data")))
KmodInfo kmod_info = { /* … */ };
```

**源码里的变量名必须写 `kmod_info`** —— 写 `_kmod_info` 会变成 Mach-O 符号 `__kmod_info`（多一个下划线）。

### 2. `Info.plist` 依赖写错，报的还是同一条错

缺 `com.apple.iokit.IOPCIFamily`、`com.apple.kpi.unsupported`，或者 `com.apple.kpi.*` 版本写成 `8.0.0`，OpenCore 依然只报 `Invalid Parameter`。最后是拿一个"能正常注入"的 kext 的 plist 当模板才拆开。

### 3. `start()` 里写硬件 = 开机断电

把页表写进 `start()`，开机在进度条处直接黑屏断电，**不会留 panic 文件**（AMD 平台上这是机器检查，不是软件崩溃，所以查不到任何崩溃日志）。

现在 `start()` 只做三件事：`IOService::start`、取 BAR 长度、`registerService()`。所有寄存器写入由用户态方法触发（`--init` / `--arm` / `--ring`）。

## Siri 的陷阱

**Siri 会拒绝一个不承认自己是"内置麦克风"的设备，哪怕它录得再好。**

`+[AVVCAudioDeviceManager IsDeviceBuiltIn:]` 要求 transport 是 `'bltn'`，且输入作用域的 `kAudioDevicePropertyDataSources`（`'ssc#'`）里包含 `'imic'`；`CSSiriRecordingInfo` 对 `'bltn'` 设备还额外要求 `'imic'` / `'emic'`。USB、蓝牙、Line 这几类 transport **不做这个检查** —— 这就是为什么那些麦克风一直能用。

坑在于：这两个属性**不能由设备直接应答**。coreaudiod 是从设备的 `kAudioObjectPropertyControlList` 里找一个 `kAudioDataSourceControlClassID`（`'dsrc'`）控件来**合成**它们的。直接回设备属性只会拿到 `'who?'`（`kAudioHardwareUnknownPropertyError`，2003332927）。

所以插件必须多暴露一个控件对象（`kObjectID_DataSource_Input = 6`）：

| 属性 | 值 |
|---|---|
| `BaseClass` | `'slct'` |
| `Class` | `'dsrc'` |
| `AvailableItems` | 1 项，`'imic'` |
| `CurrentItem` | `'imic'`（可写） |
| `ItemKind` | `'dsrc'` |
| 设备的 `ControlList` | `4, 5, 6` |

注意 `ACPMic_ControlDataSize` 里 ControlList 的大小也要跟着从 2 项改成 3 项（`3 * sizeof(AudioObjectID)`）；只改 getter 不改 size，会出现 `size=8` 却返回 3 个的诡异状态。

改完之后 `AudioObjectGetPropertyData('ssrc')` 返回 `'imic'`，Siri 立刻能用。

## 已知限制

- **没有 AEC、没有波束成形、没有降噪。** 硬件是两只间距约 2–3 cm 的麦克风（实测互相关 0.96、高频去相关），插件把 L/R 直接当立体声交出去。需要 AEC 的话在应用里用 `VoiceProcessingIO`。
- **直流偏置非常大**（本机 L ≈ −0.14 FS、R ≈ −0.42 FS）。必须靠 DC 阻断 + 二阶高通（默认 180 Hz）拉平，否则真实交流只有满量程的 1 % 左右。
- **增益是按经验调的。** 插件用"语音包络 AGC + 迟滞门限"（开门 2.5×、关门 1.4×、hangover 320 ms）。换了麦克风型号就改 `kAGC_TargetRMS`。
- **只处理一路 PDM**，只支持 48 kHz（硬件抽取因子是固定的）。
- **`start()` 必须保持惰性**：在某些机器上从 `start()` 写页表会直接把机器复位。

## 调参

所有 DSP 参数集中在 `plugin/ACPMicPlugin.c` 顶部：

| 参数 | 默认 | 作用 |
|---|---|---|
| `kHighPassHz` | `180` | 二阶高通截止频率；低频轰鸣重就提高 |
| `kPresenceHz` / `kPresenceDb` | `3500` / `7.0` | 齿音与清晰度的 peaking |
| `kAGC_TargetRMS` | `0.10` | 说话段目标电平；嫌小声就调大 |
| `kAGC_MinGain` / `kAGC_MaxGain` | `2.0` / `24.0` | 增益上下限 |
| `kGateSpeechRatio` / `kGateCloseRatio` | `2.5` / `1.4` | 门限的开门/关门倍数 |
| `kGateHoldBlocks` | `30` | 关门 hangover（约 320 ms），治句尾被切 |
| `kSoftKnee` | `0.75` | 软限幅拐点 |

改完 `./build.sh && sudo ./install.sh`，插件会重新加载，不用重启。

## 故障排查

| 现象 | 检查 |
|---|---|
| `ioreg -rc ACPProbe` 看不到 | kext 没被注入。看 OC 日志里有没有 `Prelinked injection ... Invalid Parameter` → 先查 `_kmod_info`，再查 plist 依赖 |
| kext 已加载但 `writeBytes` 一直是 0 | DMA 没起来：`ACP_WOV_PDM_NO_OF_CHANNELS` 是否写了 `0`、`DECIMATION_FACTOR` 是否 `2`、使能前有没有 `FIFO_FLUSH` |
| 设备出现但录到全 0 | 插件没连上 kext（`ioreg -rc ACPCapture` 的 `state`），或者音量/静音被系统拉成 0 |
| 只有底噪/爆音 | 前半段样本没丢（`kStartupSkipFrames`）、或没做 FIFO flush |
| 声音糊、句尾没了 | 见「调参」里的 `kAGC_*` 与 `kGateHoldBlocks` |
| Siri 显示"连接麦克风" | `'imic'` 数据源控件没生效，用 `AudioObjectGetPropertyData('ssrc')` 验证 |
| 开机在进度条处断电 | `start()` 里碰了硬件 |

## 参考与致谢

- [`NevetScar/acp6x-omen-dkms`](https://github.com/NevetScar/acp6x-omen-dkms) —— 同一块主板的 Linux 补丁，DMI 绕过与 `ACP_PIN_CONFIG` 判断的出处
- Linux `sound/soc/amd/ps/ps-pdm-dma.c` 与 `include/sound/acp63_chip_offset_byte.h` —— 寄存器序列
- [`shubhambanekar/LatSOFAudio`](https://github.com/shubhambanekar/LatSOFAudio) —— HAL 插件结构与 Siri `'imic'` 门槛的分析
- [`DexterSLamb/CmlSOFAudio`](https://github.com/DexterSLamb/CmlSOFAudio) —— kext 用 `IODMACommand` + 用户态 `IOConnectMapMemory64` 的写法

## 许可

MIT，见 [`LICENSE`](LICENSE)。
