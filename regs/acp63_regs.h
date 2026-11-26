/*
 * acp63_regs.h — AMD ACP 6.x (WOV / PDM) register map, relative offsets from BAR0.
 *
 * 来源：Linux 内核
 *   include/sound/acp63_chip_offset_byte.h      （寄存器偏移，ACP6.3/7.0/7.1 共用）
 *   sound/soc/amd/ps/acp63.h                    （常量与位域）
 *   sound/soc/amd/ps/ps-pdm-dma.c               （启动序列、DMA、PTE 表）
 *   sound/soc/amd/acp/acp-pdm.c                 （重构后的通用 PDM 实现）
 *
 * 重要：这里的偏移是 **相对 BAR0** 的（0x2C04 而不是 0x1242C04）。
 * 老的 acp6x_chip_offset_byte.h 里用的是绝对地址（基址 0x1240000），
 * 那份头文件会被误读成"寄存器在 BAR 之外 18 MB 处"。本机 BAR0 只有 256 KB，
 * 用相对偏移完全装得下（最高用到的 0x10000 + 页表长度）。
 *
 * 本机实测：pci1022,15e2，revision 0x62，BAR0 = 0xFC980000，长度 0x40000。
 */

#ifndef ACP63_REGS_H
#define ACP63_REGS_H

/* ---- 本机 PCI 身份 ---- */
#define ACP_PCI_VENDOR_AMD          0x1022
#define ACP_PCI_DEVICE_ID           0x15E2
#define ACP_PCI_REV_EXPECTED        0x62    /* 本机读到的值；Linux 表里是 0x63（ACP6.3） */

/* ---- 窗口 ---- */
#define ACP63_REG_START             0x1240000   /* Linux 用绝对地址描述，我们只用其差 */
#define ACP63_REG_END               0x125C000
#define ACP63_REG_RANGE             (ACP63_REG_END - ACP63_REG_START)   /* 0x1C000 */

/* ---- ACP_CLKRST block：上电与复位 ---- */
#define ACP_SOFT_RESET              0x01000
#define ACP_CONTROL                 0x01004
#define ACP_STATUS                  0x01008
#define ACP_PGFSM_CONTROL           0x01024
#define ACP_PGFSM_STATUS            0x01028

#define ACP_PGFSM_CNTL_POWER_ON_MASK    0xFFFFFFFF
#define ACP_PGFSM_CNTL_POWER_OFF_MASK   0x00000000
#define ACP_PGFSM_STATUS_MASK           0x03
#define ACP_POWERED_ON                  0x00
#define ACP_POWER_ON_IN_PROGRESS        0x01
#define ACP_POWERED_OFF                 0x02

/* ---- ACPAXI2AXI ATU：把 ACP 视角的地址翻译到系统内存 ---- */
#define ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1      0x00C00
#define ACPAXI2AXI_ATU_BASE_ADDR_GRP_1      0x00C04
#define ACPAXI2AXI_ATU_CTRL                 0x00C40
#define ACPAXI2AXI_ATU_PAGE_SIZE_GRP_2      0x00C08
#define ACPAXI2AXI_ATU_BASE_ADDR_GRP_2      0x00C0C
/* grp_3 .. grp_16 依次 +8 */

#define PAGE_SIZE_4K_ENABLE             2

/* ---- ACP 引脚/中断 ---- */
#define ACP_PIN_CONFIG                  0x01440
#define ACP_EXTERNAL_INTR_CNTL          0x01A04
#define ACP_EXTERNAL_INTR_CNTL1         0x01A08
#define ACP_EXTERNAL_INTR_STAT          0x01A0C

#define PDM_DMA_STAT                    0x10        /* bit4：PDM DMA 周期 */
#define PDM_DMA_INTR_MASK               0x10000     /* bit16：写 INTR_CNTL 使能 PDM DMA 中断 */

/* ---- PTE 表基址（SRAM 里放页表的地方） ---- */
#define ACP_SCRATCH_REG_0               0x10000
#define ACP_SRAM_PTE_OFFSET             0x03800000  /* 写 ATU_BASE_ADDR_GRP_1 时 | BIT(31) */
#define PDM_PTE_OFFSET                  0
#define PDM_MEM_WINDOW_START            0x4000000

/* ---- ACP_WOV block：PDM 采集的全部寄存器 ---- */
#define ACP_WOV_PDM_ENABLE                  0x02C04
#define ACP_WOV_PDM_DMA_ENABLE              0x02C08
#define ACP_WOV_RX_RINGBUFADDR              0x02C0C
#define ACP_WOV_RX_RINGBUFSIZE              0x02C10
#define ACP_WOV_RX_LINKPOSITIONCNTR         0x02C14   /* DMA 在 ring 里的当前位置 */
#define ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH  0x02C18   /* 累计字节数，高 32 位 */
#define ACP_WOV_RX_LINEARPOSITIONCNTR_LOW   0x02C1C   /* 累计字节数，低 32 位 */
#define ACP_WOV_RX_INTR_WATERMARK_SIZE      0x02C20
#define ACP_WOV_PDM_FIFO_FLUSH              0x02C24
#define ACP_WOV_PDM_NO_OF_CHANNELS          0x02C28
#define ACP_WOV_PDM_DECIMATION_FACTOR       0x02C2C
#define ACP_WOV_PDM_VAD_CTRL                0x02C30
#define ACP_WOV_WAKE                        0x02C54
#define ACP_WOV_BUFFER_STATUS               0x02C58
#define ACP_WOV_MISC_CTRL                   0x02C5C
#define ACP_WOV_CLK_CTRL                    0x02C60
#define ACP_PDM_VAD_DYNAMIC_CLK_GATING_EN   0x02C64
#define ACP_WOV_ERROR_STATUS_REGISTER       0x02C68
#define ACP_PDM_CLKDIV                      0x02C6C

/* ---- 位域 / 常量（来自 ps/acp63.h） ---- */
#define ACP_PDM_ENABLE                  1
#define ACP_PDM_DISABLE                 0
#define ACP_PDM_DMA_EN_STATUS           2       /* DMA_ENABLE bit1 置位表示正在跑 */
#define ACP_PDM_CLK_FREQ_MASK           7       /* CLK_CTRL 写入值 */
#define ACP_WOV_GAIN_CONTROL_MSB        4       /* GENMASK(4,3) */
#define ACP_WOV_GAIN_CONTROL_MASK       0x18    /* (1<<4)|(1<<3) */
#define PDM_DECIMATION_FACTOR           2
#define TWO_CH                          2

#define DELAY_US                        5
#define ACP_COUNTER                     20000

/* ---- 采集参数（Linux 硬性约束，macOS 侧照抄最省事） ---- */
#define PDM_FMT_BITS                    32      /* S32_LE */
#define PDM_CHANNELS                    2
#define PDM_RATE                        48000
#define PDM_PERIOD_BYTES_MIN            4096
#define PDM_PERIOD_BYTES_MAX            8192
#define PDM_PERIODS                     4
#define PDM_BUFFER_BYTES                (PDM_PERIOD_BYTES_MAX * PDM_PERIODS)   /* 32768 */

/* ---- 读改写小工具 ---- */
#define ACP_SET_BITS(reg, mask)         ((reg) |= (mask))
#define ACP_CLR_BITS(reg, mask)         ((reg) &= ~(mask))
#define ACP_GET_GAIN(reg)               (((reg) & ACP_WOV_GAIN_CONTROL_MASK) >> 3)
#define ACP_SET_GAIN(reg, g)            (((reg) & ~ACP_WOV_GAIN_CONTROL_MASK) | \
                                         (((g) & 0x3) << 3))

#endif /* ACP63_REGS_H */
