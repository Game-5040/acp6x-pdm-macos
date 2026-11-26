# ACPMicPlugin — 把 ACPCapture kext 采到的 PCM 变成系统输入设备

这是一个 **AudioServerPlugin（HAL 插件）**，名字叫 `ACPMicPlugin.driver`。
它把 `ACPCapture.kext` 从 AMD ACP（PCI `1022:15e2`）PDM 麦克风采到的
S32_LE / 2ch / 48 kHz 数据，做直流阻断 + 增益后，作为 **float32 / 2ch / 48 kHz 的输入设备**
提供给整个系统。

设备在「系统设置 → 声音 → 输入」里显示为 **AMD ACP PDM Microphone**，只有输入、没有输出。

```
ACPCapture.kext ──4MB 软件环──> ACPMicPlugin.driver ──> coreaudiod ──> 各 App
   (DMA/PDM)        (IOKit)         (HAL 插件)          (AudioObject)
```

## 文件

| 文件 | 作用 |
| --- | --- |
| `ACPMicPlugin.c` | 插件本体（纯 C，单文件）：对象树、属性、IO 回调、IOKit 采集、DSP |
| `Info.plist` | bundle 元数据 + `CFPlugInFactories` / `CFPlugInTypes`（HAL 靠它加载） |
| `build.sh` | 直接调用 `/usr/bin/clang` 编译、校验 plist、ad-hoc 签名 |
| `install.sh` | 装到 `/Library/Audio/Plug-Ins/HAL/` 并重启 `coreaudiod`；`uninstall` 卸载 |
| `build/` | 构建产物（`build.sh` 每次重建，可随时删） |

## 构建

```bash
cd plugin
./build.sh
```

只依赖 Command Line Tools（不需要完整 Xcode，也不用 `xcodebuild`）。
产物：`build/ACPMicPlugin.driver/`，已做 ad-hoc 签名。

## 安装 / 卸载

```bash
cd plugin
sudo ./install.sh              # 安装 + 重启 coreaudiod
sudo ./install.sh uninstall    # 卸载 + 重启 coreaudiod
```

安装后打开「系统设置 → 声音 → 输入」，选择 **AMD ACP PDM Microphone**。
第一次使用时系统可能会弹出麦克风权限询问，允许即可。

## 实现要点

### 对象树

| ID | 对象 | 说明 |
| --- | --- | --- |
| 1 | PlugIn | 设备列表、TranslateUIDToDevice、ResourceBundle |
| 2 | Device | 名字/UID/采样率/流列表/缓冲区大小/零时间戳周期 |
| 3 | Stream (Input) | 2ch float32 packed，`kAudioStreamTerminalTypeMicrophone` |
| 4 | Volume (Input, Main) | 标量 0…1 ↔ dB −96…0，默认 1.0 |
| 5 | Mute (Input, Main) | 布尔 |

没有输出流、没有 Box：`kAudioDevicePropertyStreams` 在 output 作用域返回 0 个。

### 与 kext 的交互（`ACPMic_StartCaptureLocked`）

```
IOServiceGetMatchingService("ACPCapture")
  → IOServiceOpen(type 0)
  → selector 4  Info                     // 拿 DMA 环大小 / page
  → selector 7  Init                     // 必须先做：软件环在 Init 里分配
  → IOConnectMapMemory64(type 0)         // 映射 4 MB 软件环
  → selector 5  Arm → 6 Ring → 2 Start   // 顺序固定
```

`StartIO` 的**第一个** client 才真正打开硬件；最后一个 client 的 `StopIO` 会
`Stop`(3) → 解映射 → 关闭连接，不长期占着设备。`Start` 之后 kext 会把
`writeBytes` 归零并丢掉前 200 ms，所以插件也把读位置归零。

### 取数与 DSP（`ACPMic_FillInputBufferLocked`）

`DoIOOperation(ReadInput)` 里用 selector 0 取 `writeBytes`，与上次读到的位置相减得到
增量，从环里按整帧读（处理环回绕），然后：

1. `x = s32 / 2^31`；
2. 一阶直流阻断 `y[n] = x[n] − x[n−1] + 0.9995·y[n−1]`（信号有约 −0.15 / −0.43 FS 的直流偏置）；
3. 每个声道的第一帧只用来对齐基准、输出 0（否则就是一声爆音）；
4. 增益 `6.0 × 音量`（静音时为 0），限幅到 ±1，NaN 归零，denormal 压成 0。

### 鲁棒性

- **kext 没加载**：`IOServiceGetMatchingService` 返回 0，日志里说明原因，设备照常出现、输出静音；
  不会崩溃，也不会反复刷日志。
- **`writeBytes` 不增长**：先攒 2048 帧预缓冲，不够就整周期输出静音（不消费数据）；
  连续 200 个周期仍无数据才打印一条日志。
- **积压太多**：超过 8192 帧就丢最旧的，保证延迟不会无限增长。
- **`writeBytes` 回退**（kext 被重新 Start）：重新对齐读位置并重置 DSP 状态。

### 时间戳

`GetZeroTimeStamp` 用 `mach_absolute_time()` 做锚点：`Initialize` 与每次 `StartIO`
重新锚定，`sampleTime` 按 `ZeroTimeStampPeriod = 16384` 帧一档前进，
`hostTime` 用 `mach_timebase_info` 换算的 host ticks 累加，seed 恒为 1。
这样时间轴连续、单调，不依赖 kext 的写指针（kext 的 `writeBytes` 只用于取数）。

## 已验证

在本机（macOS 26.7.3 / x86_64 / Apple clang 21）用一个临时测试宿主直接 `dlopen`
插件二进制并按 HAL 的方式调用接口表，验证过：

- `build.sh` 零错误零警告，ad-hoc 签名通过 `codesign --verify --strict`；
- `CFPlugInCreate` + `CFPlugInInstanceCreate`（走 `Info.plist` 里的工厂 UUID）能加载并 `Initialize`；
- 设备名/UID/采样率/流格式（48000、2ch、8 B/frame、float）/流配置/音量/静音/零时间戳周期都可读；
- 音量写入能读回一致；`WillDoIOOperation(ReadInput)` 为 true/in-place；
- `StartIO` → `DoIOOperation` 连续 80 个周期拿到的都是真实采集数据（非静音、峰值 < 1、无爆音）；
- `StopIO` 之后 `DoIOOperation` 输出全 0。

## 待用户验证

1. 用 `install.sh` 真正装进系统后，coreaudiod 是否正常加载、设备是否出现在声音设置里；
2. 用 QuickTime / `ffmpeg -f avfoundation` 录音，确认人声清晰、音量合适（增益 6.0 在
   `ACPMicPlugin.c` 的 `kBaseGain`，嫌小可调到 8，嫌大调到 4）；
3. 系统时间轴与真实 DMA 时钟的长期漂移：目前按任务要求用 host clock 锚定，
   若长时间录音后出现 `kIOReturnIsoTooOld` / 卡顿，需要改成用 kext 的写指针做硬件锚定。
