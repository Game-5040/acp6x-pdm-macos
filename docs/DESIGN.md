# AMD ACP6x PDM 内置麦克风 · macOS 驱动可行性判断与实施包

> 写给"准备动手写驱动"的人。这不是教程，是一份**作战地图**：哪些是事实、哪些是猜测、
> 哪些必须先验证、以及验证完每一条之后下一步该写什么。
>
> 机器：HP OMEN 16-ap1xxx（主板 8F06），Ryzen 9 9955HX，RTX 5070 Laptop，
> macOS 26.7.3 (25G342)，NullMoth 3.1，WTLWireless 0.15.0。
> 采集时间：2026-11-24。本目录里所有数字都来自这台机器或 Linux 源码原文。

---

## 0. 一页结论

**Phase 0A 已完成并通过（2026-11-25）：这台机器的 PDM 麦克风在 Linux 上录到了声音。**
用的是 `NevetScar/acp6x-omen-dkms`（补丁版 Yellow Carp 驱动，带了 HP 主板 `8F06` 的 DMI quirk），
`arecord` 五秒录音有明确信号、人耳可辨。唯一副作用是开始时有一下极短的爆破音（成因与规避见 §17）。

**所以现在可以写驱动了。** 原始三条结论仍然成立，只是状态变了：

1. **最大的障碍不存在。** 早期"PDM 寄存器在 BAR 之外 18 MB"的判断是**错的**（被老的
   `acp6x_chip_offset_byte.h` 的绝对地址写法误导）。ACP6.3 头文件用的是相对偏移
   `ACP_WOV_PDM_ENABLE = 0x2C04`，本机 BAR0 有 **256 KB**，装得下；Linux 的
   `acp6x_readl()` 又把 `0x1242C04 - 0x1240000` 算成同一个数，两个来源互相印证。→ §4。
2. **BIOS 没有声明这块麦克风。** DSDT 里既没有 `_WOV`，也没有带 `acp-audio-device-type`
   的 DMIC 子设备。现在有了实证：正是这个缺失让 Linux 必须靠 DMI quirk 才肯注册声卡
   （否则 `_WOV = 0` → `-ENODEV`）。macOS 侧照旧"跳过检测、直接编程寄存器"。→ §5.2。
3. **"先用 Linux 验硬件"这一步已经做完了。** 硬件链路完整，剩下的纯粹是移植。→ §17。

---

## 1. 本机事实

### 1.1 硬件

| 项 | 值 | 来源 |
|---|---|---|
| 机型 | HP OMEN 16-ap1xxx，主板 8F06 | DSDT / SMBIOS |
| CPU | AMD Ryzen 9 9955HX（Zen5 Fire Range，16 核） | — |
| 内置麦克风 | 双阵列，**不在 ALC245 上**（§2 有实测） | — |
| 声卡 codec | Realtek ALC245（`0x10EC0245`） | HD Audio |
| ACP | PCI `1022:15E2`，revision **0x62**，subsystem `103c:068f` | ioreg |
| ACP BAR0 | **0xFC980000，长度 0x40000（256 KB）** | ioreg `IODeviceMemory` |
| HD Audio | PCI `1022:15E3`，BAR0 `0xFC9C0000`，长度 0x8000 | ioreg |
| ACPI 辅助设备 | `AMDI0063` @ `0xFEC13000`，0x200 字节 + GPIO 中断（DSDT `Device (HID0)`） | DSDT 5343–5376 |

`refs/ioreg-acp.txt`、`refs/dsdt-acp-pci-device.txt`、`refs/dsdt-amdi0060-0063.txt` 是原始抓取。

### 1.2 当前 macOS 音频状态

- 输出：`Realtek ALC245 (Analog)`，由 **NMXAudio 1.0.0**（NullMoth 自带）驱动，正常。
- 输入：`system_profiler SPAudioDataType` 里 **确实有一个** `Realtek ALC245 (Analog)` 输入节点
  （2 声道，48 kHz，且是系统默认输入设备）。**但它是哑的**——见 §2 的实测。
- `MtkWlan`/蓝牙与此无关。

> 注意这个陷阱：设备列表里有"输入"，很容易以为"驱动有 bug、录不到音"。
> 实际上那是 NMXAudio 按 HDA 拓扑建出来的节点，物理上没接线。

---

## 2. 为什么 macOS 现在没有内置麦克风：四条独立证据

### 证据 1：ALC245 输入实测是静音（今天测的）

用 AVFoundation 从默认输入设备（`Realtek ALC245 (Analog)`）采 4 秒，显式指定
Float32/48k/2ch，统计峰值与 RMS：

```
device: Realtek ALC245 (Analog)
samples=382976  peak=0.000275  rms=0.000057
判定：静音（几乎没有信号）
```

`peak 2.75e-4` / `rms 5.7e-5` 是 ADC 空闲噪声的量级，不是人声。房间里有人说话时
正常麦克风的 peak 应该在 0.05–0.5。**结论：ALC245 的输入通道存在但没有信号源。**

### 证据 2：BIOS 把 codec 的麦克风引脚关了

早前用 `alc-verb` 直接读 ALC245 的 pin widget：

```
0x12 = 0x411111f0
```

`0x411111f0` 的解码是 **Pin Complex，不可连接（no physical connection），默认设备 = 无**。
也就是说主板根本没把内置麦克风接到 codec 上。

### 证据 3：AppleALC 的 layout 里没有 ConfigData

AppleALC 的 layout 11（本机用的）不含 `ConfigData`，它假设 BIOS 已经配好引脚。
BIOS 又恰恰把 0x12 关了。两边都不管 → 这条路上永远不会有麦克风。

### 证据 4：AMDMicrophoneContinuity 明确拒绝本机

NullMoth 生态里那个 AMD 麦克风项目（`AMDMicrophoneContinuity`）二进制里只有一句：

```
"%s: Only Renoir is supported at the moment"
```

它检查 `revisionId == 0x1`（Renoir = ACP 3.1）。本机 ACP revision 是 **0x62**，
直接被挡在门外。这条路线不是"没适配好"，是**根本没写**。

**小结：内置麦克风的信号不在 ALC245 上；它走 AMD ACP 的 PDM（数字麦克风）通道。**
Windows 上能看到"麦克风阵列（AMD Audio Device）"就是这个原因。

---

## 3. Windows/Linux 是怎么拿到它的

- **Windows**：AMD 的 ACP 驱动把 PDM 采集成一个标准音频端点，2 声道 48 kHz。
- **Linux**：`snd_pci_ps` / `snd_acp_pci` 驱动栈：
  1. ACP PCI 功能（`15E2`）probe，`pci_enable_device` + `pci_set_master`；
  2. 读 ACPI 判断有没有 PDM（`_WOV` + DMIC 子设备属性）与 SoundWire；
  3. 有 PDM 就注册 platform device `acp_ps_pdm_dma`（DMA/寄存器）+ `dmic-codec`（codec）；
  4. 用户态通过 ALSA 打开 `hw:CARD,0`，`arecord -f S32_LE -r48000 -c2` 就能录。

关键参数（Linux 硬性约束，我们照抄最省事）：

| 参数 | 值 |
|---|---|
| 格式 | `S32_LE`（32 位小端整型） |
| 声道 | 2 |
| 采样率 | 48000（唯一支持） |
| 周期 | 4 个，每个 4096–8192 字节 |
| 缓冲 | 32768 字节 |

---

## 4. 关键更正：寄存器就在 BAR0 里

### 4.1 之前错在哪

两条流传的头文件，编号体系不一样：

| 头文件 | 风格 | `ACP_WOV_PDM_ENABLE` |
|---|---|---|
| 老 `acp6x_chip_offset_byte.h`（ACP6.1/6.2 时代） | **绝对物理地址**，基址 `0x1240000` | `0x1242C04` |
| 新 `include/sound/acp63_chip_offset_byte.h`（ACP6.3/7.x） | **相对 BAR0 的偏移** | `0x0002C04` |

老文件里所有寄存器都长成 `0x12xxxxx`，看上去像"要 18 MB 窗口"。新文件里同样的寄存器
是 `0x2C04`。ACP6.3 的 PCI 驱动也是这么用的：

```c
/* sound/soc/amd/ps/pci-ps.c */
addr = pci_resource_start(pci, 0);
adata->acp63_base = devm_ioremap(&pci->dev, addr, pci_resource_len(pci, 0));
...
adata->reg_range = ACP63_REG_END - ACP63_REG_START;   /* 0x1C000 */
```

`ACP63_REG_END - ACP63_REG_START = 0x125C000 - 0x1240000 = 0x1C000`（112 KB）。
也就是说 **整个 ACP6.3 寄存器区只有 112 KB**，BAR 256 KB 绰绰有余。

### 4.2 我们真正要碰的偏移

| 偏移 | 用途 | 在 256 KB BAR 内？ |
|---|---|---|
| `0x01000` / `0x01024` / `0x01028` | ACP 软复位 / PGFSM 上电控制与状态 | ✅ |
| `0x00C00`–`0x00C40` | ACPAXI2AXI ATU（DMA 地址翻译） | ✅ |
| `0x01440` | `ACP_PIN_CONFIG` | ✅ |
| `0x01A04` / `0x01A0C` | 外部中断控制 / 状态 | ✅ |
| `0x010000` | `ACP_SCRATCH_REG_0`（写 PTE 表的窗口） | ✅ |
| `0x02C04`–`0x02C6C` | **WOV / PDM 全部寄存器** | ✅ |

**一句话：窗口问题不存在。** 现在的不确定性只剩"这块硬件在不在、能不能上电"。

---

## 5. 还没确认的三件事（决定项目成败）

### A. PDM 控制器到底在不在（最致命）

如果 ACP 的 PDM 模块被 BIOS 关掉、或者这颗 SoC 的 ACP 里压根没有 PDM，
读 `0x2C04` 会得到 `0xFFFFFFFF`（总线无响应）或全 0（模块没上电）。

**判据**（Phase 0B 探针输出）：

| 读到的值 | 含义 | 下一步 |
|---|---|---|
| `PDM_ENABLE`/`DMA_ENABLE` = `0xFFFFFFFF`，扫描全 FF | BAR 映射了但模块不响应 | 试 PGFSM 上电；仍 FF → 大概率没有 PDM，转 Phase 0A |
| 全 0，但 `MISC_CTRL`/`CLK_CTRL` 可读可写 | 模块在，没使能 | 正常，进 Phase 1 |
| `MISC_CTRL = 0x00000018` 之类有增益位 | **PDM 在且配置过** | 立刻进 Phase 1 |

### B. BIOS 没声明（已知，可绕过）

DSDT 里：

- `Device (ACP) { Name (_ADR, 0x05) }` —— **空的**，没有子设备；
- 全 DSDT 搜 `_WOV` = **0 处**；搜 `acp-audio-device-type` = **0 处**；
- 有一个 `AMDI0063` 设备（`0xFEC13000`，512 字节 + GPIO 中断，由 EC 变量 `HD0E` 控制启停），
  在 macOS 的 ioreg 里**根本不存在**（被 `_STA` 关掉或未枚举）。

Linux 的判定逻辑（`ps/pci-ps.c:412-460`）要求上面两样都存在，所以 Linux 会认为没有 PDM。
我们的方案是"跳过 ACPI 判定、直接写寄存器"，因此这条**不阻断**，但代价是：
**没有现成参照物，第一个能出声的程序必须自己写**。

### C. 时钟与供电

PDM 要工作，至少需要：
1. ACP 上电：`PGFSM_CONTROL = 0xFFFFFFFF`，轮询 `PGFSM_STATUS & 3 == 0`（Linux 的 `acp_power_on`）；
2. PDM 时钟：`ACP_WOV_CLK_CTRL = ACP_PDM_CLK_FREQ_MASK (7)`；
3. 增益：`ACP_WOV_MISC_CTRL` 的 bit4:3（0–3，Linux 默认 3）。

这三步任何一个不对，录到的就是全 0。Phase 1 要能分别打日志。

---

## 6. 寄存器地图（相对 BAR0）

完整定义见 `regs/acp63_regs.h`，这里只列语义。

### 6.1 电源 / 复位 / ATU

| 寄存器 | 偏移 | 说明 |
|---|---|---|
| `ACP_SOFT_RESET` | `0x01000` | 软复位（bit0/bit16 成对） |
| `ACP_CONTROL` / `ACP_STATUS` | `0x01004` / `0x01008` | 运行控制/状态 |
| `ACP_PGFSM_CONTROL` | `0x01024` | 写 `0xFFFFFFFF` = 上电，`0` = 断电 |
| `ACP_PGFSM_STATUS` | `0x01028` | `0` 已上电，`1` 上电中，`2` 已断电 |
| `ATU_PAGE_SIZE_GRP_1` | `0x00C00` | 写 `2` = 4 KB 页 |
| `ATU_BASE_ADDR_GRP_1` | `0x00C04` | 写 `0x03800000 \| BIT(31)` |
| `ATU_CTRL` | `0x00C40` | 写 `1` 使能翻译 |
| `ACP_SCRATCH_REG_0` | `0x010000` | 页表入口（每页 8 字节） |
| `ACP_PIN_CONFIG` | `0x01440` | 引脚配置（DMIC 相关） |

### 6.2 WOV / PDM

| 寄存器 | 偏移 | 读写 | 说明 |
|---|---|---|---|
| `WOV_PDM_ENABLE` | `0x02C04` | RW | bit0 = PDM 使能 |
| `WOV_PDM_DMA_ENABLE` | `0x02C08` | RW | bit0 = 启动 DMA；bit1 = **正在运行**（`ACP_PDM_DMA_EN_STATUS`） |
| `WOV_RX_RINGBUFADDR` | `0x02C0C` | RW | DMA 环形缓冲物理地址 |
| `WOV_RX_RINGBUFSIZE` | `0x02C10` | RW | 环形缓冲字节数 |
| `WOV_RX_LINKPOSITIONCNTR` | `0x02C14` | R | DMA 当前在 ring 里的位置 |
| `WOV_RX_LINEARPOSITIONCNTR_HIGH/LOW` | `0x02C18` / `0x02C1C` | R | **累计写入字节数**（轮询用这个） |
| `WOV_RX_INTR_WATERMARK_SIZE` | `0x02C20` | RW | 中断水位（轮询方案里只影响内部行为） |
| `WOV_PDM_FIFO_FLUSH` | `0x02C24` | W | 写 1 清 FIFO |
| `WOV_PDM_NO_OF_CHANNELS` | `0x02C28` | RW | 2 |
| `WOV_PDM_DECIMATION_FACTOR` | `0x02C2C` | RW | 2 |
| `WOV_MISC_CTRL` | `0x02C5C` | RW | **bit4:3 = 增益（0–3）** |
| `WOV_CLK_CTRL` | `0x02C60` | RW | 写 7 开 PDM 时钟 |
| `WOV_ERROR_STATUS_REGISTER` | `0x02C68` | R | 出错时读它 |
| `PDM_CLKDIV` | `0x02C6C` | RW | 时钟分频（Linux 不用） |

---

## 7. 启动序列（照抄 Linux，带判据）

Linux 原文在 `refs/linux/ps-pdm-dma.c` 第 45–180 行。翻译成我们的顺序：

```
前提：BAR0 已映射；ACP 已上电（PGFSM_STATUS & 3 == 0）

1) 配置 ring
   W_RINGBUFADDR   = ring_phys
   W_RINGBUFSIZE   = ring_bytes            # 32768
   W_INTR_WATERMARK= period_bytes          # 4096 或 8192
   W_ATU_CTRL      = 1

2) 页表（DMA 真正能不能跑看这一步，见 §8）
   W_ATU_BASE_ADDR_GRP_1 = ACP_SRAM_PTE_OFFSET | BIT(31)   # 0x03800000 | 0x80000000
   W_ATU_PAGE_SIZE_GRP_1 = 2                                # 4 KB
   for i in 0..num_pages-1:
       W_SCRATCH(0 + i*8)     = low32(ring_phys + i*4096)
       W_SCRATCH(0 + i*8 + 4) = high32(ring_phys + i*4096) | BIT(31)

3) 时钟 + 增益
   W_CLK_CTRL = 7
   W_MISC_CTRL = (W_MISC_CTRL & ~0x18) | (gain << 3)       # gain 默认 3

4) 声道 / 抽样
   W_PDM_NO_OF_CHANNELS    = 2
   W_PDM_DECIMATION_FACTOR = 2

5) 启动
   W_PDM_ENABLE     = 1
   W_PDM_DMA_ENABLE = 1
   timeout = 20000 次，每次等 5 µs：
       if (R_PDM_DMA_ENABLE & 0x02) == 2: 成功
   超时 → 读 ERROR_STATUS，打印后放弃

6) 停止（顺序反过来）
   W_PDM_DMA_ENABLE = 2        # 请求停止
   等 (R & 0x02) == 0
   W_PDM_ENABLE = 0
   W_PDM_FIFO_FLUSH = 1
```

**每一步都要打日志**：Phase 1 里第一次能录音之前，90% 的时间会花在"第 5 步超时"上，
而超时的原因通常在第 2 步页表。

---

## 8. DMA 与 PTE 表（最难的一环）

ACP 的 DMA 控制器**不直接认系统物理地址**。它通过两组东西访问内存：

1. **ATU（AXI2AXI 翻译单元）**：告诉 ACP"页表在哪、页多大"；
2. **页表（放在 ACP 的 SRAM 窗口里）**：逐页列出每 4 KB 页对应的系统物理地址。

写页表的方式很怪：往 `ACP_SCRATCH_REG_0` 这个 1 MB 窗口里按 8 字节一组写，
每组的低 4 字节是页物理地址低 32 位，高 4 字节是**高位 | BIT(31)**（那一位是"有效"）。

```c
/* Linux 原文语义，sound/soc/amd/ps/ps-pdm-dma.c:157-180 */
writel(ACP_SRAM_PTE_OFFSET | BIT(31), base + ACPAXI2AXI_ATU_BASE_ADDR_GRP_1);
writel(PAGE_SIZE_4K_ENABLE,           base + ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1);
u32 val = 0;
for (page = 0; page < num_pages; page++) {
    writel(low32(addr),  base + ACP_SCRATCH_REG_0 + val);
    writel(high32(addr) | BIT(31), base + ACP_SCRATCH_REG_0 + val + 4);
    val += 8;
    addr += 4096;
}
```

要点：

- **缓冲必须是物理连续的**（或者你给每一页都建表项——Linux 就是逐页建表）。
  macOS 上 `IOBufferMemoryDescriptor::withOptions(kIODirectionIn, size, 4096)`
  拿到的就是连续物理内存；`getPhysicalSegment()` 取物理地址。
- 32768 字节 = 8 页 → 8 个表项，64 字节，很小。
- 写完表要 `writel` 一次读回（AMD 的写是 posted，不读回可能还没落地就启动了 DMA）。
- 环形缓冲的地址给的是**物理地址**，不是虚拟地址。

**判据**：如果第 5 步能等到 `DMA_EN_STATUS = 2`，说明页表基本对了；
这时 `LINEARPOSITIONCNTR` 会随时间长，`FIFO_FLUSH` 前后读数会跳变。

---

## 9. 三个减法：为什么 macOS 上 500 行就够

Linux 那个驱动 4000+ 行，是因为它要服务 ALSA/ASoC 框架、要处理中断、要应付
SoundWire、要做 PM runtime。我们不需要。

### 减法 1：轮询，不写中断

不碰 `ACP_EXTERNAL_INTR_CNTL`、不申请中断、不写 ISR。
kext 起一个 10–20 ms 的 `IOTimerEventSource`，每次读
`LINEARPOSITIONCNTR_HIGH/LOW` 算出新增字节数，凑够一个周期就把数据搬走。

- 好处：省掉整块最容易 panic 的代码；中断共享/电平触发/丢失都能绕开。
- 代价：多 20 ms 延迟，麦克风场景无所谓。

### 减法 2：kext 只做硬件，音频进用户态

| 层 | 职责 | 行数量级 |
|---|---|---|
| kext | PCI 匹配、BAR 映射、上电、ring + 页表、轮询、`IOUserClient` 暴露 mmap/start/stop | **400–600 行** |
| 用户态 daemon | 从 kext 拿 PCM，喂给 BlackHole（现成的 AudioServerPlugin 虚拟设备） | 150–250 行 |

不写 `IOAudioFamily`（那是 2000+ 行、每次重启才能迭代、panic 风险最高的一层）。
用 BlackHole 就不用自己写 AudioServerPlugin——它本身就是一个系统的虚拟音频设备，
daemon 往里写，系统里就会出现一个"麦克风"。

### 减法 3：跳过 ACPI 检测

`_WOV`、`acp-audio-device-type`、DMIC 子设备这些是给 Linux 的自动探测用的。
macOS 侧我们知道自己要什么，直接写寄存器，不需要"判断有没有麦克风"这一步。
这条同时解决了 §5B 的 BIOS 未声明问题。

---

## 10. macOS 侧架构

```
        ┌──────────────────────────────┐
        │  用户态 daemon (acpmicd)      │
        │  - 打开 kext 的 user client   │
        │  - mmap ring 或 read 周期数据  │
        │  - S32 → Float32              │
        │  - 写入 BlackHole 的输入流     │
        └───────────▲──────────────────┘
                    │ IOUserClient (mmap + 3 个方法)
        ┌───────────┴──────────────────┐
        │  kext (ACP6xPDM)              │
        │  - start(): 上电/映射/建ring   │
        │  - 轮询位置计数                │
        │  - 暴露 ring + 指针            │
        └───────────▲──────────────────┘
                    │ MMIO
              ACP BAR0 (0x40000)
```

`IOUserClient` 最小接口：

| 方法 | 作用 |
|---|---|
| `open` | 取 `IOCommandGate` 引用、校验权限 |
| `mapRing` (`IOConnectMapMemory`) | 把 kext 的 ring 内存映射给 daemon（type 0） |
| `startCapture` / `stopCapture` | 走下方 §7 的启动/停止序列 |
| `getStatus` (structOut) | 返回当前写指针、累计字节、错误码 |

**为什么不干脆让 kext 直接输出音频？** 因为 `IOAudioFamily` 在 macOS 26 上属于
"能不用就不用"的旧框架，且每改一行都要重启验证；用户态迭代快 10 倍。

---

## 11. 分阶段计划与判据（含 token 预算）

### Phase 0A：Linux 死刑判决 ✅ 已完成（2026-11-25 通过，结果见 §17）

做一个 Ubuntu 启动盘，启动到 live 环境，然后：

```bash
# 1. 看内核认出什么
sudo dmesg | grep -iE "acp|pdm|dmic|soundwire"
cat /proc/asound/cards
arecord -l

# 2. 如果没列出 PDM 设备，强行看寄存器（需要 root + /dev/mem 或 acp 驱动）
#    最简单的方式：看 ACP 是否绑定了驱动
lspci -nnk -d 1022:15e2
ls /sys/bus/platform/devices | grep -i pdm

# 3. 如果有采集设备，录 3 秒并看电平
arecord -D hw:1,0 -f S32_LE -r 48000 -c 2 -d 3 /tmp/mic.wav
python3 - <<'PY'
import wave,struct,math
w=wave.open('/tmp/mic.wav'); n=w.getnframes(); d=w.readframes(n)
if w.getsampwidth()==4:
    v=struct.unpack('<%di'%(len(d)//4), d)
    peak=max(abs(x) for x in v)/2**31
    rms=math.sqrt(sum(x*x for x in v)/len(v))/2**31
    print("peak",round(peak,5),"rms",round(rms,5))
PY
```

| 结果 | 含义 | 决定 |
|---|---|---|
| 有 PDM 设备且录到声音 | 硬件链路完整 | **全速进入 Phase 1**，macOS 只是移植 |
| 有设备但全静音 | 麦克风可能没接 / 需要 BIOS 开关 | 先排查硬件，暂缓 |
| 没有 PDM 设备（大概率） | BIOS 未声明 | 用 macOS 探针强测（Phase 0B） |
| `lspci` 显示 no driver | 同 Linux 判定逻辑 | 同上 |

**这一步能省掉两周的盲目工作。** 它不消耗 token，只消耗 30 分钟。

### Phase 0B：macOS 探针（半天，~80k–150k token）

本目录 `probe/` 已备好一个只读 kext：映射 BAR0，打印 §6 那张表，扫描整个窗口统计
非零/全 FF 数量，可选 PGFSM 上电（`DO_POWER_ON`）。

判据见 §5A 的表格。

### Phase 1：DMA ring + 导出 WAV（约 1 周，~600k–1M token）

- kext 里建 32 KB 连续缓冲 + 页表 + 启动序列；
- 轮询 `LINEARPOSITIONCNTR`，把 ring 里的数据按"读指针"抄到用户态；
- 用户态写 WAV（`S32_LE`，2ch，48k），用 Audacity/QuickTime 听。
- **判据：能听到自己的声音。** 到这一步项目就算"技术上可行"被证明了。

### Phase 2：接进系统，变成"麦克风"（3–5 天，~300k token）

- 装 BlackHole，daemon 以 `kAudioUnitProperty_StreamFormat` 匹配 S32→Float32；
- LaunchDaemon 常驻，插拔/睡眠后重建；
- 判据：FaceTime/Zoom/Photo Booth 的输入设备里出现它，且能录到声音。

### Phase 3：打磨（1–2 周，~300k–800k token）

- 增益映射、双耳一致性、噪声底、时钟漂移（长时间录音的累积误差）、
- 睡眠唤醒恢复、错误恢复（DMA 超时重试）、CPU 占用。

**总计 1.5M–3.5M token，3–5 周**（假设 Phase 0 顺利）。

---

## 12. 风险清单（按致命度排序）

| # | 风险 | 概率 | 应对 |
|---|---|---|---|
| 1 | ~~麦克风不在 ACP PDM 上~~ | **已排除** | Phase 0A 已录到声音，硬件链路确认（§17） |
| 2 | PDM 需要额外的 SoC 时钟/电源（`PDM_CLKDIV`、外供） | 中 | 保留 `ERROR_STATUS` 日志；试 `CLKDIV` 不同值 |
| 3 | BIOS 用 SoundWire 而不是 PDM 连麦克风 | 低-中 | ACP 的 SoundWire 是另一套（`0x3000` 起），且需 codec；本机无 SDW codec |
| 4 | 页表/ATU 写错 → DMA 超时 | 高（但可修） | 严格照抄 §7/§8，逐步打日志 |
| 5 | kext 加载后被系统拦住（SIP/签名） | 中 | 走和 NullMoth 一样的路径：`/Library/Extensions` + 重建辅助 KC |
| 6 | 写寄存器把 ACP 弄崩（panic） | 中 | Phase 0 只读；Phase 1 后才写，且只在 start 时写 |
| 7 | 双麦克风相位/幅度不一致 | 低 | Phase 3 的事 |

---

## 13. 参考代码索引（都在 `refs/linux/`）

| 文件 | 关键位置 | 看什么 |
|---|---|---|
| `acp63_chip_offset_byte.h` | 110–160、240–300、490 | 全部寄存器偏移（**相对**） |
| `acp63.h` | 12–60 | `ACP63_REG_START/END`、`ACP_SRAM_PTE_OFFSET`、周期/缓冲参数、`ACP_WOV_GAIN_CONTROL` |
| `ps-pdm-dma.c` | 45–180 | ring 初始化、时钟/增益、启动/停止、**页表写入** |
| `ps-pdm-dma.c` | 240–340 | 位置计数、硬件参数约束 |
| `pci-ps.c` | 412–460 | Linux 如何判断"有没有 PDM"（我们不需要，但要理解它为什么失败） |
| `pci-ps.c` | 595–660 | BAR 映射与 `ACP63_REG_RANGE` |
| `acp-pdm.c` | 30–90 | 重构后的通用 PDM 实现（写法更现代，可对照） |
| `amd.h` | 95–120 | PGFSM/上电常量 |

---

## 14. 目录说明

```
acpmic-driver/
├── README.md                  ← 本文件
├── regs/acp63_regs.h          ← 寄存器头文件（可直接 include）
├── probe/                     ← Phase 0B 只读探针 kext（已可编译）
│   ├── ACPProbe.cpp
│   ├── Info.plist
│   ├── build.sh
│   └── README.md              ← 安装与判读
├── tools/mic-level/           ← 测任意输入设备有没有信号（今天用来判 ALC245 静音）
│   ├── mic-level.m
│   └── run.sh
└── refs/
    ├── ioreg-acp.txt          ← 本机 BAR/rev 原始数据
    ├── dsdt-acp-pci-device.txt
    ├── dsdt-amdi0060-0063.txt
    └── linux/                 ← Linux 参考源码（离线可读）
```

---

## 15. 今天就能做的三步

1. ~~做一个 Ubuntu 启动盘，跑 Phase 0A~~ **已完成并通过（§17）**，直接进 Phase 0B。
2. **编译并加载 `probe/ACPProbe.kext`**（见 `probe/README.md`），把 `log show` 里的
   `ACPProbe:` 行贴出来——它直接回答"PDM 在不在"。
3. 把 §14 的目录拿去当骨架，Phase 1 从 `regs/acp63_regs.h` 和 §7 的启动序列开始写。

如果 Phase 0A 和 0B 都指向"硬件不存在"，**最理性的选择是买个 USB 麦克风**（几十块钱，
插上就能用，还能顺带解决 Continuity/耳机麦的问题）。写这个驱动的价值在于"它本来就在机器
里、Windows 上有、macOS 上没有"，而不是"没有麦克风就没法开会"。

---

## 16. macOS 侧参考实现：CmlSOFAudio（重要）

`refs/macos/CmlSOFAudio-main/`（BSD-3-Clause，DexterSLamb，HP Chromebook C1030 / Intel Comet Lake）。
**硬件上完全不能用**（Intel SOF DSP + RT5682 + MAX98357A，靠 HDA BAR4 加载 `sof-cml.ri`，
走 SSP/I²S + I²C），但它和本项目的目标架构**逐一对应**，是迄今找到的最好的 macOS 骨架：

| 它有什么 | 对应本项目的哪一步 |
|---|---|
| `kext/`：`IOService` 子类，`IOBufferMemoryDescriptor` + `IODMACommand` 拿物理地址（`CmlSOFAudioDevice.cpp:8-30`） | Phase 1 采集缓冲：ACP 侧把这段物理地址按 Linux 的写法填进 `SCRATCH_REG_0` 的 PTE |
| `kext/` 里 `IOCommandGate` 串行化寄存器访问（`:384-388`） | 我们的轮询定时器 + 用户态控制也要串行化 |
| `kext/` 导出 `IOUserClient`：`kCmlSOF_MemCapture`(1) 映射采集环、`kCmlSOF_StartCapture`(4)/`StopCapture`(5) 控制 | 我们照抄这套方法号与三段映射（playback/capture/flags）的做法 |
| `plugin/`：Apple **NullAudio** 派生的 AudioServerPlugIn（196 KB C，`AudioServerPlugIn.h` 全套 + `GetZeroTimeStamp`） | Phase 2 的 CoreAudio 设备层：不用自己写 AudioServerPlugin 从零开始 |
| `plugin/CmlSOFAudioPlugin.c:3909-4020`：`IOServiceOpen` + `IOConnectMapMemory64` 拿共享环 + `IOConnectCallScalarMethod` 起停 | 我们的 plugin 与之同构，只需换设备名、通道数、采样率 |
| 采集是 **4 声道 PDM DMIC** 并且能工作 | 我们的 ACP PDM 是 2ch/48k/S32_LE，通道处理和时钟逻辑可以照着削 |

可直接借：plugin 整个文件（改常量、改 ABI 结构、改 `kAudioStreamProperty` 的 2ch/48k）、
kext 里的 DMA/IOUserClient 骨架。**不可借**：所有 SOF/DSP/HDA 寄存器、固件加载、编解码器
初始化、`sof-cml.ri`（Intel 固件，与本机无关）。

许可证仍是 BSD-3-Clause（kext/plugin 主体），plugin 同时保留 Apple NullAudio 的样例代码声明——
自己用没问题，若要并进本仓库公开发布，需保留两份版权声明。

### 16.1 两个参考项目的取舍（LatSOFAudio vs CmlSOFAudio）

`refs/macos/LatSOFAudio-main/`（BSD-3-Clause，Shubham Banekar，Dell Latitude 3410，Comet Lake，
Intel SST `8086:02c8`，ALC236）——**硬件同样不适用**（还是 Intel SOF/SST），但对我们更有针对性：

| 维度 | LatSOFAudio | CmlSOFAudio |
|---|---|---|
| 目标 | **只做内置麦克风**（Siri/听写/FaceTime 能用） | 扬声器 + 耳机 + 麦克风全做 |
| CoreAudio 暴露 | **input-only HAL plugin**（1 条输入流，`kAudioStreamTerminalTypeMicrophone`，`LatSOFAudioPlugin.c:2802`） | 输入 + 输出 |
| 另有一条路 | `LatSOFKernelAudio.cpp`（26 KB）用 **IOAudioFamily**（`IOAudioDevice` + `IOAudioEngine` + `IOAudioStream`）暴露采集 | 无 |
| 移植文档 | **`PORTING.md`（17 KB）**：§0 五分钟判死、§1 Linux 先证明、§4 采集没声音的排查顺序、§5 保命规则 | README 较简 |
| 排错日记 | **`DEBUGGING-LOG.md`（46 KB）** + `ARCHITECTURE.md`（43 KB） | 无 |
| 用户态小工具 | `contrib/latsof-setrate.c`（改采样率）、`contrib/latsof-afgwake.c`（唤醒音频固件） | 无 |

**结论：写我们的 plugin 时以 `LatSOFAudioPlugin.c` 为模板**（它就是麦克风驱动），
它的 `PORTING.md §0` 与我们的 Phase 0A 完全一致：

> Boot any Linux live USB … `arecord -l` … 录五秒再放出来。If Linux cannot record,
> macOS never will — stop here.

这一条对我们尤其关键：本机 DSDT 里没有 `_WOV`、也没有 `acp-audio-device-type`，
Linux 大概率会打印 `No PDM or SoundWire manager devices found`。如果 `arecord -l`
根本看不到 DMIC，那就说明 BIOS 把 PDM 关掉了（或走了别的路径），先别写代码。

### 16.2 决定性证据：acp6x-omen-dkms（本机同款主板的 Linux 补丁）

`refs/linux-omen/acp6x-omen-dkms-master/`（GPL-2.0，NevetScar）。仓库描述就是
**"HP OMEN 16 … (Fixes Board ID 15E2 Rev 62 AMD Microphone Array)"**，README 里还有一条
「Add support for HP board **8F06**（HyperX OMEN 16-ap1xxx）」——**正是本机主板**。
结论：**这颗 PDM 麦克风在 Linux 上是能工作的**，下面是它踩到的三个坑，直接改写我们的假设：

1. **驱动选错**：15E2 rev `0x62` 默认绑到 **Raphael**（`snd_rpl_pci_acp6x`），那个驱动功能不全、
   没有麦克风；要强制用 **Yellow Carp**（`snd_pci_acp6x`）。包里的
   `blacklist-acp6x-omen.conf` 只有一行：`blacklist snd_rpl_pci_acp6x`。
   → Phase 0A 在 Ubuntu 里先 `lspci -nnk` 看绑的是哪个驱动，若不是 `snd_pci_acp6x`，
   就算硬件好也录不到，必须先黑名单 rpl + 装这个 DKMS。
2. **`_WOV` 骗人**：`acp6x-mach.c` 的 probe 原本先读 ACPI `_WOV`，本机早期返回 0 →
   `return -ENODEV`，整个 card 不注册。补丁改成**先查 DMI quirk 表**（`8F06` 在表里）→
   `goto register_card`，`_WOV` 只在非 quirk 机器上才看。
   → 对我们 macOS 的意义：**完全不要依赖 `_WOV` / `AMDI0063` / `acp-audio-device-type`**，
   直接编程寄存器（这与 §9「跳过 _WOV」一致，现在有了本机实证）。
3. **PDM 模式怎么判定**：`pci-acp6x.c:188` 读 `ACP_PIN_CONFIG`（BAR 偏移 `0x1440`），
   若值不在 `{0,1,2,3,9,15}`（已知音频模式）里，就按 **PDM 模式**注册
   `acp_yc_pdm_dma` + `dmic-codec` + `acp_yc_mach` 三个平台设备。
   → 我们的 probe kext 打印 `PIN_CONFIG` 的值就能判断本机处于哪种模式。

**寄存器偏移的最终定论**：`acp6x.h:99` 的访问器是

```c
static inline u32 acp6x_readl(void __iomem *base_addr)
{
    return readl(base_addr - ACP6x_PHY_BASE_ADDRESS);   // 0x1240000
}
```

调用处写的是 `acp6x_readl(acp_base + ACP_WOV_PDM_ENABLE)`，`ACP_WOV_PDM_ENABLE = 0x1242C04`
→ 实际访问 **BAR0 + 0x2C04**。所以 §4 的结论（相对偏移、256 KB BAR 足够、不必 SSDT 扩窗）
在这颗芯片上是对的，`acp63` 头文件的相对偏移与之一致，两条来源互相印证。

---

## 17. Phase 0A 结果（2026-11-25）：硬件确认可录，附启动爆破音分析

### 17.1 怎么验的

Ubuntu 里跑 `acpmic-driver/tools/omen-mic-test.sh`：先 `status` 看 `1022:15e2` 绑的是
`snd_rpl_pci_acp6x`（Raphael，无麦克风）还是 `snd_pci_acp6x`（Yellow Carp，有 PDM）；
是前者就 `install` 装 `refs/linux-omen/acp6x-omen-dkms-master/`（DKMS），
它只做两件事：`blacklist snd_rpl_pci_acp6x`、用打了 DMI quirk 的 `acp6x-mach.c` 覆盖内核模块；
然后关机再开机（不是重启），最后 `test` 用 `arecord -D plughw:CARD,DEV -f S32_LE -r 48000 -c 2 -d 5` 录五秒。

**结果：有声音，peak/RMS 达标，人耳可辨。** 现象只有一处：开头有一声极短的爆破音。

### 17.2 这条结果排除了什么

| 之前的不确定 | 现在的结论 |
|---|---|
| PDM 控制器是否存在（最致命的一条） | **存在且可工作** |
| 寄存器是否真在 BAR0 + 0x2C04 | 是（Linux 用同一组偏移成功驱动） |
| BIOS 没声明 `_WOV` / DMIC 子设备是否意味着硬件缺失 | **不意味着**，只是 BIOS 没写；quirk 绕过后照常工作 |
| `AMDI0063` 没在 ioreg 里枚举是否致命 | 不致命，Linux 也不依赖它 |
| 采样格式 | `S32_LE`、2 声道、48000 Hz 实测可用 |

### 17.3 爆破音从哪来（我们的驱动怎么避开）

对照 `refs/linux/ps-pdm-dma.c` 与 `refs/linux-omen/acp6x-omen-dkms-master/acp6x-pdm-dma.c`：

- `ACP_WOV_PDM_FIFO_FLUSH` **只在 stop 路径被写**（omen 版 `:149`），启动路径没有；
- 启动路径只有：写 `ACP_WOV_CLK_CTRL`（`:61`）→ `udelay(DELAY_US)`（5 µs）→
  写 `PDM_ENABLE` / `PDM_DMA_ENABLE` → 轮询 `PDM_DMA_EN_STATUS`；
- 也就是说 **FIFO 里残留的陈旧样本 + 时钟刚起振时的 DC 台阶** 被第一批 DMA 直接搬进了缓冲区。

三条规避（写在我们自己的驱动里，Phase 1 就带上）：

1. **enable 之前先 flush**：`writel(1, BAR + 0x2C24)`（`ACP_WOV_PDM_FIFO_FLUSH`），再等 1 ms；
2. **时钟使能后多等一会儿**：`CLK_CTRL` 之后等 **10–20 ms**（Linux 只等了 5 µs，这是它自己也有的毛病），
   再 `PDM_ENABLE`；
3. **丢弃开头**：DMA 起跑后丢掉前 **100–200 ms** 的样本再送用户态（约 9 600–19 200 帧 @48 kHz）。

### 17.4 副作用与边界

- 这套 DKMS 是**替换内核模块**：升级内核后要 `dkms autoinstall` 或重跑 `install`；
  Secure Boot 开着的话模块会被拒载（`mokutil --sb-state` 查）。
- Ubuntu 侧的补丁只影响 Linux，**不影响 macOS**；macOS 侧我们是从零写 kext，不复用这套代码。
- 爆破音不影响判定结论，但说明 Linux 驱动的启动时序不是"最佳实践"，我们不要照抄它的等待时间。

### 17.5 下一步

Phase 0B（只读探针 kext，`probe/ACPProbe.cpp`，已能编译）：在 macOS 里确认
① `getDeviceMemoryWithIndex(0)` 拿到的长度真是 0x40000；
② `BAR+0x1440`（`ACP_PIN_CONFIG`）读出的值落在 `{0,1,2,3,9,15}` 之外（PDM 模式的判据）；
③ `BAR+0x2C04` / `+0x2C0C` / `+0x2C5C` 不是全 0 / 全 FF。
三条都对，Phase 1 的 DMA ring 就可以照 §7–§8 直接写。

---

## 18. 落地结果（2026-11-26，macOS 26.7.3 上已验证）

§1–§17 是「动手前的判断」，这一节是「动完手之后的实际做法」，两者不一致处以本节为准。

### 18.1 三个此前的判断被实测推翻

| 原判断 | 实测结果 |
|---|---|
| BAR 窗口只有 256 KB，寄存器在 18 MB 处，需要 SSDT 覆盖 `_CRS` | **不需要**：`acp6x_chip_offset_byte.h` 里是**相对 BAR 的偏移**（`ACP_WOV_PDM_ENABLE = 0x2C04`），老 `acp63_chip_offset_byte.h` 里的 `0x1242C04` 是「BAR 基址 + 偏移」的合成值 |
| 需要完整的 `acp_init()` 电源序列（PGFSM、软复位…） | **不需要**：`ACP_PGFSM_STATUS` 读出 `0`（`ACP_POWERED_ON`），固件已经把 ACP 点亮了 |
| 需要写中断处理 | **不需要**：10 ms 定时器读 `ACP_WOV_RX_LINEARPOSITIONCNTR` 就够，音质与延迟都正常 |

### 18.2 kext（`capture/ACPCapture.cpp`）

- **`_kmod_info` 必须自己造**：没有 Xcode 时 `ld -kext` 不会生成这个符号，OpenCore 会在 prelink 阶段报 `Invalid Parameter`（内部真实错误是 `VirtualKmod == 0` → `EFI_UNSUPPORTED`）。做法见 `capture/kmod_info.cpp`：手写 `pragma pack(4)` 的 196 字节结构，变量名必须写 `kmod_info`（写 `_kmod_info` 会产出 `__kmod_info`）。
- **Info.plist 的依赖必须齐**：缺 `com.apple.iokit.IOPCIFamily` 或 `com.apple.kpi.unsupported` 时，OC 同样只报 `Invalid Parameter`，看不出是哪一条。
- **`start()` 必须惰性**：一开就在 `start()` 里写 PTE 寄存器会让机器**直接断电**（无 panic 文件，属硬件级复位）。现在 `start()` 只做 `registerService()`，所有硬件动作都由用户态方法触发。
- 定时器要 `workLoop->addEventSource()`，只 `timerEventSource()` 不会走。
- `ACP_WOV_PDM_NO_OF_CHANNELS` 要显式写 `0`（TWO_CH）、`DECIMATION` 写 `2`，否则固件留的 `1` 会让硬件按 4×32bit 出帧，后两路恒 `0x80000000`。
- 启动顺序里 `PDM_ENABLE` 之前先 `ACP_WOV_PDM_FIFO_FLUSH=1` + 20 ms，再丢弃前 200 ms 样本，可消除开录瞬间的爆破音。

### 18.3 用户态（`plugin/ACPMicPlugin.c`，input-only HAL 插件）

- 数据本身带**巨大直流偏置**（L ≈ -0.14 FS、R ≈ -0.42 FS），所以必须一阶 DC 阻断 + 90 Hz 二阶高通（RBJ）。注意一阶高通的历史变量要存**高通输出**，存原始输入会放大 1/(1-R) 倍直流，直接把语音压没。
- AGC 用「语音包络」而不是逐块 RMS：逐块会在音节间反复上下，听感就是「模糊/泵动」。
- 门限要带迟滞（开 2.5×、关 1.4×噪声底）+ 320 ms hangover，否则句尾被切。

### 18.4 Siri 的麦克风门槛（关键，别人踩不到的知识）

macOS 的 Siri **不检查驱动是第几方**，只检查 HAL 属性：

- `+[AVVCAudioDeviceManager IsDeviceBuiltIn:]` 要求 transport 是 `'bltn'`，且输入作用域 `kAudioDevicePropertyDataSources`（`'ssc#'`）第一项是 `'imic'`；
- `CSSiriRecordingInfo` 按 transport 路由：`'bltn'` 设备必须报 `'imic'`/`'emic'`，否则 UI 显示「Connect a microphone」，**哪怕录音完全正常**；`'usb '`/`'blue'`/`'line'` 不做这个检查。

而 `'ssc#'`/`'ssrc'` **不是插件可以直接应答的设备属性**：coreaudiod 是从设备的**控件列表**里找 `kAudioDataSourceControlClassID`（`'dsrc'`）控件来合成这两个属性的。所以正确做法是新增一个**选择器控件**：

| 对象 | 属性 | 值 |
|---|---|---|
| object 6（数据源控件） | `BaseClass` | `'slct'` |
| | `Class` | `'dsrc'` |
| | `AvailableItems` | 1 项 `'imic'` |
| | `CurrentItem` | `'imic'`（可写） |
| | `ItemKind` | `'dsrc'` |
| 设备 | `ControlList` | `4, 5, 6` |

改完 `AudioObjectGetPropertyData` 立刻变成 `DataSource('ssrc') status=0 value='imic'`、`DataSources('ssc#') status=0 count=1`，Siri 随即正常（2026-11-26 18:02 实测）。

### 18.5 文件清单

| 路径 | 作用 |
|---|---|
| `capture/ACPCapture.cpp` | PCI kext：PTE 编程、DMA ring、10 ms 定时器、IORegistry 状态 |
| `capture/kmod_info.cpp` | 手写 `_kmod_info`，绕开 Xcode |
| `plugin/ACPMicPlugin.c` | input-only HAL 插件：DC 阻断、高通、presence、包络 AGC、迟滞门、软限幅、`'imic'` 数据源控件 |
| `tools/acpcap/acpcap.c` | 用户态采集/录 WAV/`--hp` 高通试听 |
| `tools/mic-level/mic-hal.m` | 实测电平与 100 ms RMS 分位数 |
| `tools/install-capture.sh`、`plugin/install.sh` | 装 kext（改 config.plist）/ 装 HAL 插件 |
