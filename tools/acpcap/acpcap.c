/*
 * acpcap —— 从 ACPCapture.kext 读 PCM，存成 WAV，并打印峰值/RMS
 *
 * 用法：
 *   ./acpcap                录 3 秒，写 /tmp/acp.wav
 *   ./acpcap 5 out.wav      录 5 秒
 *   ./acpcap --status       只看驱动状态
 *   ./acpcap --dump 1       录 1 秒并把前 32 个样本打成十六进制
 */

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#define kMethodGetStatus 0
#define kMethodStart     2
#define kMethodStop      3
#define kMethodInfo      4
#define kMethodArm       5
#define kMethodRing      6
#define kMethodInit      7
#define kMethodSnap      8

static const uint32_t kRate = 48000, kCh = 2;

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_wav(const char *path, const uint8_t *data, uint32_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return -1; }
    uint32_t dataBytes = bytes;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, kCh);
    put32(f, kRate); put32(f, kRate * kCh * 4); put16(f, kCh * 4); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, dataBytes);
    fwrite(data, 1, dataBytes, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    double secs = 3.0;
    const char *out = "/tmp/acp.wav";
    int statusOnly = 0, dump = 0, armOnly = 0, ringOnly = 0, initOnly = 0, snapOnly = 0, hex = 0, hp = 0;

    if (argc > 1 && strcmp(argv[1], "--status") == 0) statusOnly = 1;
    else if (argc > 1 && strcmp(argv[1], "--arm") == 0) armOnly = 1;
    else if (argc > 1 && strcmp(argv[1], "--ring") == 0) ringOnly = 1;
    else if (argc > 1 && strcmp(argv[1], "--init") == 0) initOnly = 1;
    else if (argc > 1 && strcmp(argv[1], "--snap") == 0) { snapOnly = 1; if (argc > 2) hex = 1; }
    else if (argc > 1 && strcmp(argv[1], "--hex") == 0) { snapOnly = 1; hex = 1; }
    else if (argc > 1 && strcmp(argv[1], "--dump") == 0) { dump = 1; if (argc > 2) secs = atof(argv[2]); }
    else if (argc > 1 && strcmp(argv[1], "--hp") == 0) { hp = 1; if (argc > 2) secs = atof(argv[2]); if (argc > 3) out = argv[3]; }
    else if (argc > 1) secs = atof(argv[1]), (argc > 2 ? (out = argv[2]) : 0);

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                           IOServiceMatching("ACPCapture"));
    if (!svc) { fprintf(stderr, "找不到 ACPCapture 服务（kext 没装上？）\n"); return 2; }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "IOServiceOpen 失败: 0x%x\n", kr); return 2; }

    uint64_t info[3] = {0}, cnt = 3;
    kr = IOConnectCallScalarMethod(conn, kMethodInfo, NULL, 0, info, (uint32_t *)&cnt);
    if (kr) { fprintf(stderr, "取信息失败: 0x%x\n", kr); return 2; }
    uint32_t ringBytes = (uint32_t)info[0], capBytes = (uint32_t)info[1];
    printf("ring=%u B  cap=%u B  page=%llu B\n", ringBytes, capBytes,
           (unsigned long long)info[2]);

    uint64_t st[4] = {0};
    uint32_t outCnt = 4;
    kr = IOConnectCallScalarMethod(conn, kMethodGetStatus, NULL, 0, st, &outCnt);
    uint64_t wb = (st[1] << 32) | st[0];
    printf("writeBytes=%llu  lastLinear=%llu  dmaEnable=0x%llx\n",
           (unsigned long long)wb, (unsigned long long)st[2],
           (unsigned long long)st[3]);
    if (statusOnly) {
        system("ioreg -rc ACPCapture -l | grep -E '\"(ticks|linearNow|ringAddr|ringSize|atuBase|atuPage|atuCtrl|clkCtrl|miscCtrl)\" = '");
        return 0;
    }

    if (initOnly) {
        kr = IOConnectCallScalarMethod(conn, kMethodInit, NULL, 0, NULL, NULL);
        printf("init(映射 BAR + 申请内存) => 0x%x %s\n", kr, kr ? "失败" : "OK");
        return kr ? 6 : 0;
    }
    if (armOnly) {
        kr = IOConnectCallScalarMethod(conn, kMethodArm, NULL, 0, NULL, NULL);
        printf("arm(只写 PTE) => 0x%x %s\n", kr, kr ? "失败" : "OK");
        return kr ? 6 : 0;
    }
    if (ringOnly) {
        kr = IOConnectCallScalarMethod(conn, kMethodRing, NULL, 0, NULL, NULL);
        printf("ring(环缓冲+ATU) => 0x%x %s\n", kr, kr ? "失败" : "OK");
        return kr ? 6 : 0;
    }

    if (snapOnly) {
        kr = IOConnectCallScalarMethod(conn, kMethodInit, NULL, 0, NULL, NULL);
        if (kr) { fprintf(stderr, "init 失败: 0x%x\n", kr); return 5; }
        kr = IOConnectCallScalarMethod(conn, kMethodSnap, NULL, 0, NULL, NULL);
        if (kr) { fprintf(stderr, "snap 失败: 0x%x\n", kr); return 5; }
        mach_vm_address_t sa = 0; mach_vm_size_t ss = 0;
        kr = IOConnectMapMemory64(conn, 0, mach_task_self(), &sa, &ss, kIOMapAnywhere);
        if (kr) { fprintf(stderr, "映射失败: 0x%x\n", kr); return 2; }
        int32_t *p = (int32_t *)(uintptr_t)sa;
        size_t n = ringBytes / 4;
        int64_t peak = 0; double sumsq = 0;
        size_t nz = 0;
        for (size_t i = 0; i < n; i++) {
            int64_t v = p[i];
            if (v) nz++;
            if (llabs(v) > peak) peak = llabs(v);
            sumsq += (double)v * (double)v;
        }
        printf("snap: %zu 个 int32 样本，非零 %zu，peak=%lld，rms=%.1f\n",
               n, nz, (long long)peak, sqrt(sumsq / (double)(n ? n : 1)));
        printf("前 16 个样本:");
        for (size_t i = 0; i < 16 && i < n; i++) printf(" %d", p[i]);
        printf("\n");
        if (hex) {
            /* 按 4 通道 32-bit 解交织，逐通道看统计，判断 0x80000000 是不是固定占位 */
            printf("\n按 4ch int32 解交织（stride %d B）:\n", 16);
            int32_t *q = p;
            for (int ch = 0; ch < 4; ch++) {
                int64_t cpk = 0, cmin = 0x7fffffff, cmax = (-0x7fffffff - 1);
                double sq = 0; size_t cnt = ringBytes / 16;
                for (size_t i = 0; i < cnt; i++) {
                    int32_t v = q[i * 4 + ch];
                    if (llabs(v) > cpk) cpk = llabs(v);
                    if (v < cmin) cmin = v;
                    if (v > cmax) cmax = v;
                    sq += (double)v * (double)v;
                }
                printf("  ch%d: peak=%11lld  min=%11lld  max=%11lld  rms=%14.1f  前 6 个:",
                       ch, (long long)cpk, (long long)cmin, (long long)cmax,
                       sqrt(sq / (double)(cnt ? cnt : 1)));
                for (size_t i = 0; i < 6; i++) printf(" %d", q[i * 4 + ch]);
                printf("\n");
            }
            printf("\n按 2ch int32 解交织（stride 8 B）:\n");
            for (int ch = 0; ch < 2; ch++) {
                int64_t cpk = 0; double sq = 0; size_t cnt = ringBytes / 8;
                for (size_t i = 0; i < cnt; i++) {
                    int32_t v = q[i * 2 + ch];
                    if (llabs(v) > cpk) cpk = llabs(v);
                    sq += (double)v * (double)v;
                }
                printf("  ch%d: peak=%11lld  rms=%14.1f  前 6 个:", ch, (long long)cpk,
                       sqrt(sq / (double)(cnt ? cnt : 1)));
                for (size_t i = 0; i < 6; i++) printf(" %d", q[i * 2 + ch]);
                printf("\n");
            }
            unsigned char *b = (unsigned char *)p;
            printf("\n原始字节（前 128 B）:\n");
            for (int r = 0; r < 8; r++) {
                printf("  %04x:", r * 16);
                for (int c = 0; c < 16; c++) printf(" %02x", b[r * 16 + c]);
                printf("   ");
                for (int c = 0; c < 16; c += 4) {
                    int32_t v = (int32_t)(b[r*16+c] | (b[r*16+c+1]<<8) | (b[r*16+c+2]<<16) | ((uint32_t)b[r*16+c+3]<<24));
                    printf(" %11d", v);
                }
                printf("\n");
            }
            /* 按 3 字节（24 bit）左对齐/右对齐各算一次峰值，看数据到底是不是 24 bit */
            int64_t peak24 = 0, peak16 = 0;
            for (size_t i = 0; i + 3 < ringBytes; i += 3) {
                int32_t v = (int32_t)((uint32_t)b[i] | ((uint32_t)b[i+1] << 8) | ((uint32_t)b[i+2] << 16));
                if (v & 0x800000) v |= ~0xFFFFFF;
                if (llabs(v) > peak24) peak24 = llabs(v);
            }
            int16_t *s16 = (int16_t *)p;
            for (size_t i = 0; i < ringBytes / 2; i++) if (llabs(s16[i]) > peak16) peak16 = llabs(s16[i]);
            printf("\n按 24-bit 读法 peak=%lld（满量程 8388608）; 按 16-bit 读法 peak=%lld（满量程 32768）\n",
                   (long long)peak24, (long long)peak16);
        }
        return 0;
    }

    mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
    kr = IOConnectMapMemory64(conn, 0, mach_task_self(), &addr, &size, kIOMapAnywhere);
    if (kr) { fprintf(stderr, "映射缓冲失败: 0x%x\n", kr); return 2; }
    printf("已映射 %llu B @ %p\n", (unsigned long long)size, (void *)addr);

    kr = IOConnectCallScalarMethod(conn, kMethodInit, NULL, 0, NULL, NULL);
    if (kr) { fprintf(stderr, "init 失败: 0x%x\n", kr); return 5; }
    kr = IOConnectCallScalarMethod(conn, kMethodArm, NULL, 0, NULL, NULL);
    if (kr) { fprintf(stderr, "arm 失败: 0x%x\n", kr); return 5; }
    kr = IOConnectCallScalarMethod(conn, kMethodRing, NULL, 0, NULL, NULL);
    if (kr) { fprintf(stderr, "ring 失败: 0x%x\n", kr); return 5; }
    kr = IOConnectCallScalarMethod(conn, kMethodStart, NULL, 0, NULL, NULL);
    if (kr) { fprintf(stderr, "startCapture 失败: 0x%x（DMA 没起来）\n", kr); return 5; }
    usleep(200000);   /* 等 200 ms，让时钟与 FIFO 稳定 */

    uint64_t want = (uint64_t)(secs * kRate * kCh * 4);
    if (want > capBytes) want = capBytes;
    if (want > size) want = size;

    outCnt = 4;
    IOConnectCallScalarMethod(conn, kMethodGetStatus, NULL, 0, st, &outCnt);
    wb = (st[1] << 32) | st[0];
    uint64_t start = wb;
    for (int i = 0; i < 600; i++) {
        outCnt = 4;
        if (IOConnectCallScalarMethod(conn, kMethodGetStatus, NULL, 0, st, &outCnt)) break;
        wb = (st[1] << 32) | st[0];
        if (wb - start >= want) break;
        usleep(20000);
    }
    if (wb - start < want) {
        fprintf(stderr, "警告：只收到 %llu / %llu 字节\n",
                (unsigned long long)(wb - start), (unsigned long long)want);
        want = wb - start;
    }
    if (!want) { fprintf(stderr, "没有采到数据。dmaEnable=0x%llx\n", (unsigned long long)st[3]); return 3; }

    const uint8_t *base = (const uint8_t *)(uintptr_t)addr;
    uint8_t *buf = malloc(want);
    uint64_t off = start % capBytes, copied = 0;
    while (copied < want) {
        uint32_t n = (uint32_t)(want - copied);
        if (n > capBytes - off) n = (uint32_t)(capBytes - off);
        memcpy(buf + copied, base + off, n);
        off = (off + n) % capBytes;
        copied += n;
    }

    /* 可选：一阶 DC 阻断 + 归一化。硬件给出的样本带很大直流偏置
       （约 -0.15 / -0.43 满量程），真正的声音只有 1% 量级。 */
    if (hp) {
        int32_t *v = (int32_t *)buf;
        size_t frames = (size_t)(want / 8);
        double R = 0.9995, x1[2] = {0, 0}, y1[2] = {0, 0};
        double *tmp = malloc(frames * 2 * sizeof(double));
        double pk = 0;
        if (!tmp) { fprintf(stderr, "内存不足\n"); return 5; }
        for (size_t i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                double x = v[i * 2 + c] / 2147483648.0;
                double y = x - x1[c] + R * y1[c];
                x1[c] = x; y1[c] = y;
                tmp[i * 2 + c] = y;
                if (fabs(y) > pk) pk = fabs(y);
            }
        }
        double g = (pk > 1e-9) ? 0.9 / pk : 1.0;
        for (size_t i = 0; i < frames * 2; i++) {
            double y = tmp[i] * g;
            if (y > 1.0) y = 1.0;
            if (y < -1.0) y = -1.0;
            v[i] = (int32_t)(y * 2147483647.0);
        }
        free(tmp);
        printf("高通(一阶, R=%.4f) + 归一化增益 x%.1f\n", R, g);
    }

    /* 统计 */
    const int32_t *s = (const int32_t *)buf;
    uint64_t n = want / 4;
    double peak = 0, sum = 0;
    for (uint64_t i = 0; i < n; i++) {
        double v = s[i] / 2147483648.0;
        if (fabs(v) > peak) peak = fabs(v);
        sum += v * v;
    }
    double rms = n ? sqrt(sum / n) : 0;
    printf("样本 %llu  peak=%.6f  rms=%.6f  %s\n",
           (unsigned long long)n, peak, rms,
           peak > 0.01 ? "有信号" : (peak < 0.001 ? "静音" : "很弱"));

    if (dump) {
        printf("前 32 个样本：");
        for (int i = 0; i < 32 && i < (int)n; i++) printf(" %08x", (uint32_t)s[i]);
        printf("\n");
    }

    if (write_wav(out, buf, (uint32_t)want) == 0)
        printf("已写入 %s（%.2f 秒，S32_LE %uch %uHz）\n", out, secs, kCh, kRate);

    IOConnectCallScalarMethod(conn, kMethodStop, NULL, 0, NULL, NULL);

    free(buf);
    IOConnectUnmapMemory64(conn, 0, mach_task_self(), addr);
    IOServiceClose(conn);
    return peak > 0.001 ? 0 : 4;
}
