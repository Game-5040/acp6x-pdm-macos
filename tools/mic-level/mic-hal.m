// mic-hal.m —— 通过 AVFoundation 从一个指定名字的 HAL 输入设备录音，统计电平。
//
// 用途：验证 ACPMicPlugin 这条“HAL 插件 → coreaudiod → App”的完整链路，
// 它和 acpcap 直接读 kext 不同：走的是系统音频栈，能证明别的 App 也能用。
//
// 编译：
//   clang -fobjc-arc -O2 -o mic-hal mic-hal.m \
//         -framework AVFoundation -framework Foundation -framework CoreMedia
//
// 运行：
//   ./mic-hal "AMD ACP" 4          # 名字包含 "AMD ACP"，录 4 秒

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

static double gPeakL = 0, gPeakR = 0;
static double gSumL = 0, gSumR = 0;
static long   gFrames = 0;

/* 把所有样本攒下来，事后算 100 ms 窗口的 RMS 分布，并写一个 WAV 方便人耳复核。
 * 课堂这种"一直有人说话"的环境用瞬时 RMS 看不出问题，得看整段的分布：
 * 静音段应该几乎为 0，说话段应该在 0.1 上下。 */
static int dblcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y);
}

#define kMaxSeconds 120
static float *gPCM = NULL;
static long   gPCMCap = 0;

@interface Cap : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
@property (nonatomic, copy) NSString *wavPath;
@end

@implementation Cap
- (void)captureOutput:(AVCaptureOutput *)o
    didOutputSampleBuffer:(CMSampleBufferRef)sb
           fromConnection:(AVCaptureConnection *)c
{
    (void)o; (void)c;
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    if (!bb) return;
    size_t len = CMBlockBufferGetDataLength(bb);
    if (len < sizeof(float) * 2) return;

    float *buf = malloc(len);
    if (!buf) return;
    CMBlockBufferCopyDataBytes(bb, 0, len, buf);

    // audioSettings 指定的是 2ch 交错 float32，所以 L/R 交替
    long frames = (long)(len / (sizeof(float) * 2));
    for (long i = 0; i < frames; i++) {
        double l = fabs((double)buf[i * 2]);
        double r = fabs((double)buf[i * 2 + 1]);
        if (l > gPeakL) gPeakL = l;
        if (r > gPeakR) gPeakR = r;
        gSumL += (double)buf[i * 2] * (double)buf[i * 2];
        gSumR += (double)buf[i * 2 + 1] * (double)buf[i * 2 + 1];
    }
    gFrames += frames;

    if (gPCM && (gFrames * 2) < gPCMCap) {
        memcpy(gPCM + (gFrames - frames) * 2, buf, (size_t)frames * 2 * sizeof(float));
    }

    free(buf);
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *want = (argc > 1) ? [NSString stringWithUTF8String:argv[1]] : @"AMD ACP";
        double seconds = (argc > 2) ? atof(argv[2]) : 4.0;

        // 找名字匹配的输入设备；找不到就退回系统默认
        AVCaptureDevice *dev = nil;
        for (AVCaptureDevice *d in [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio]) {
            if ([d.localizedName rangeOfString:want options:NSCaseInsensitiveSearch].location != NSNotFound) {
                dev = d;
                break;
            }
        }
        if (!dev) {
            dev = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
            printf("没找到名字包含 \"%s\" 的设备，改用默认设备\n", want.UTF8String);
        }
        if (!dev) { printf("没有任何音频输入设备\n"); return 1; }
        printf("device: %s\n", dev.localizedName.UTF8String);

        AVCaptureSession *s = [[AVCaptureSession alloc] init];
        NSError *err = nil;
        AVCaptureDeviceInput *in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
        if (!in) {
            printf("打不开输入设备: %s\n", err.localizedDescription.UTF8String);
            printf("（可能是麦克风权限没给这个终端，去 系统设置 → 隐私与安全性 → 麦克风 里打开）\n");
            return 2;
        }
        [s addInput:in];

        AVCaptureAudioDataOutput *out = [[AVCaptureAudioDataOutput alloc] init];
        out.audioSettings = @{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVLinearPCMBitDepthKey: @32,
            AVLinearPCMIsFloatKey: @YES,
            AVLinearPCMIsNonInterleaved: @NO,
            AVLinearPCMIsBigEndianKey: @NO,
            AVNumberOfChannelsKey: @2,
            AVSampleRateKey: @48000.0
        };
        Cap *cap = [[Cap alloc] init];
        [out setSampleBufferDelegate:cap queue:dispatch_get_global_queue(0, 0)];
        [s addOutput:out];

        gPCMCap = (long)(kMaxSeconds * 48000 * 2);
        gPCM = calloc((size_t)gPCMCap, sizeof(float));

        [s startRunning];
        // 前 0.2 秒是插件预缓冲/DC 阻断收敛期，前面不统计
        [NSThread sleepForTimeInterval:0.2];
        gPeakL = gPeakR = 0; gSumL = gSumR = 0; gFrames = 0;
        [NSThread sleepForTimeInterval:seconds];
        [s stopRunning];

        double rmsL = gFrames ? sqrt(gSumL / gFrames) : 0;
        double rmsR = gFrames ? sqrt(gSumR / gFrames) : 0;
        printf("frames=%ld (%.2f s)\n", gFrames, gFrames / 48000.0);
        printf("L: peak=%.4f rms=%.4f\n", gPeakL, rmsL);
        printf("R: peak=%.4f rms=%.4f\n", gPeakR, rmsR);
        double pk = fmax(gPeakL, gPeakR);
        /* 100 ms 窗口 RMS 分布 */
        long win = 4800;
        long nWin = gFrames / win;
        if (nWin > 0) {
            double *rmsv = malloc(sizeof(double) * nWin);
            for (long w = 0; w < nWin; w++) {
                double acc = 0;
                for (long i = 0; i < win; i++) {
                    float l = gPCM[(w * win + i) * 2 + 0];
                    float r = gPCM[(w * win + i) * 2 + 1];
                    acc += (double)l * l + (double)r * r;
                }
                rmsv[w] = sqrt(acc / (win * 2));
            }
            qsort(rmsv, nWin, sizeof(double), dblcmp);
            printf("100ms 窗口 RMS：最小 %.4f  10%% %.4f  中位 %.4f  90%% %.4f  最大 %.4f\n",
                   rmsv[0], rmsv[nWin / 10], rmsv[nWin / 2], rmsv[nWin * 9 / 10], rmsv[nWin - 1]);

            /* 写 16-bit WAV，方便直接听 */
            NSString *path = @"/tmp/mic-hal.wav";
            int16_t *pcm16 = malloc((size_t)gFrames * 2 * sizeof(int16_t));
            for (long i = 0; i < gFrames * 2; i++) {
                float v = gPCM[i];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                pcm16[i] = (int16_t)lrintf(v * 32767.0f);
            }
            FILE *f = fopen(path.UTF8String, "wb");
            if (f) {
                uint32_t dataBytes = (uint32_t)(gFrames * 2 * sizeof(int16_t));
                uint32_t chunkSize = 36 + dataBytes;
                uint32_t rate = 48000, byteRate = 48000 * 4;
                uint16_t channels = 2, bits = 16, align = 4, fmt = 1;
                uint32_t fmtSize = 16;
                fwrite("RIFF", 1, 4, f); fwrite(&chunkSize, 4, 1, f);
                fwrite("WAVEfmt ", 1, 8, f);
                fwrite(&fmtSize, 4, 1, f);      /* 先写块长度，再写格式标签 */
                fwrite(&fmt, 2, 1, f);
                fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f);
                fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f);
                fwrite(&bits, 2, 1, f);
                fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
                fwrite(pcm16, 2, (size_t)gFrames * 2, f);
                fclose(f);
                printf("已写入 /tmp/mic-hal.wav（%.1f 秒）\n", gFrames / 48000.0);
            }
            free(pcm16);
            free(rmsv);
        }

        if (pk < 0.0005)      printf("判定：静音\n");
        else if (pk > 0.99)   printf("判定：有信号，但可能削波（峰值接近满量程）\n");
        else                  printf("判定：有信号\n");
        free(gPCM);
    }
    return 0;
}
