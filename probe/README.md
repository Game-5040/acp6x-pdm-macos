# ACPProbe —— Phase 0 只读探针

把 ACP PCI 功能（`pci1022,15e2`）的 BAR0 映射进来，读一遍寄存器并打印。
**默认不写任何寄存器**（除了 PCI 命令寄存器里的 Memory/BusMaster 使能位）。

## 1. 编译

```bash
cd probe
./build.sh
```

产物：`probe/ACPProbe.kext`

编译参数里有两个必须的：

- `-DKERNEL`：`Kernel.framework` 里 `IOPCIDevice` 真正的类声明包在 `#if defined(KERNEL)` 里，
  不加这个会拿到 PCIDriverKit 的用户态投影，编译报 `no member named 'setMemoryEnable'`。
- `-fapple-kext` + `ld -kext`：产物必须是 `Mach-O kext bundle`，否则 `kmutil` 不认。

`build.sh` 已经处理好了。改代码后重跑即可，不需要 Xcode 工程。

## 2. 安装

和你装 NullMoth 的流程一样（本项目不是 kext 开发者签名的，靠 AMFI 放宽 + 辅助内核集合加载）：

```bash
# 1) 拷进去
sudo cp -R probe/ACPProbe.kext /Library/Extensions/

# 2) 重建辅助内核集合（macOS 26 的写法）
sudo kmutil install --update-all --volume-root /

# 3) 重启
sudo reboot
```

重启后验证：

```bash
kmutil showloaded | grep ACPProbe
```

看到 `com.shang.driver.ACPProbe (0.1.0)` 就说明加载了。

> 如果 `kmutil showloaded` 里没有：
> - `sudo kextutil -t /Library/Extensions/ACPProbe.kext` 看依赖问题；
> - `log show --last 2m --predicate 'eventMessage CONTAINS "ACPProbe"'` 看内核拒绝原因。
> - 检查 config.plist 的 `Kernel → Add` 是否也会加载它（如果你把它放进 EFI/OC/Kexts 的话；
>   本项目建议只放 `/Library/Extensions`，不占 OpenCore 的注入路径）。

## 3. 看输出

```bash
log show --last 5m --predicate 'eventMessage CONTAINS "ACPProbe"' --info --debug \
  | grep ACPProbe
```

正常会看到：

```
ACPProbe: BAR0 phys=0xFC980000 len=0x40000 virt=0x... rev=0x62
ACPProbe: read-only mode (DO_POWER_ON=0); PGFSM_STATUS=0x00000002
ACPProbe:   [0x01000] ACP_SOFT_RESET        = 0x...
ACPProbe:   [0x02C04] WOV_PDM_ENABLE       = 0x...
...
ACPProbe: scan 0..0x40000: nonzero=... all-FF=... first=0x... last=0x...
ACPProbe: done. ...
```

## 4. 怎么判读

| 现象 | 结论 | 下一步 |
|---|---|---|
| `BAR0 phys=0xFC980000 len=0x40000` | 映射成功 | 继续看寄存器 |
| 表里全是 `0xFFFFFFFF`，扫描 `all-FF` 接近 0x10000 | 模块不响应（没上电或没有这个模块） | 把 `DO_POWER_ON` 改 1 重编重装，再试 |
| `PGFSM_STATUS = 2`（已断电） | ACP 在省电状态 | 必须开 `DO_POWER_ON` |
| `WOV_MISC_CTRL` 读到 `0x18` 之类 | **PDM 存在** | 立刻进 Phase 1 |
| `WOV_PDM_ENABLE` = 0，但 `0x2C00`–`0x2C70` 有非零值 | PDM 存在、未使能 | 正常 |
| `scan` 的 `first/last` 集中在 `0x0`–`0x2000` | 只有 CLKRST 块可读，WOV 块不存在 | 大概率硬件没有 PDM，走 README §5A |
| 加载后立刻 panic | 映射/访问越界 | 把扫描范围缩小到 `0..0x20000` |

## 5. 打开上电（谨慎）

`ACPProbe.cpp` 顶部：

```c
#define DO_POWER_ON 0   →   1
```

重编、重装、重启。上电序列是 `PGFSM_CONTROL = 0xFFFFFFFF`，然后最多等 2 秒。
这一步只写一个寄存器，风险可控；但如果 ACP 与 HD Audio 共用电源域，
理论上可能影响声卡（重启即可恢复）。

## 6. 卸载

```bash
sudo rm -rf /Library/Extensions/ACPProbe.kext
sudo kmutil install --update-all --volume-root /
sudo reboot
```
