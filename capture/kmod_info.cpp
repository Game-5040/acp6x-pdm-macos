// kmod_info.cpp —— 手工补齐 kext 的 _kmod_info 符号
//
// 为什么需要这个文件：
//   真正的 kext 二进制里有一个 __DATA,__data 段的符号 _kmod_info，
//   它是 XNU 的 kmod_info_t 结构（kext 的名字、版本、start/stop 入口）。
//   Xcode 的 kext 构建模板会自动生成它；本机只有 Command Line Tools，
//   ld -kext 不会生成（试过 -r / 不带 -r / clang++ 驱动链接 /
//   -sectcreate __TEXT __info_plist，全都没有），于是：
//
//     _kmod_info 缺失 -> OpenCore 的 KextFindKmodAddress 返回 0
//     -> PrelinkedInjectKext 里 Kext->Context.VirtualKmod == 0
//     -> Link.c 的 InternalPrelinkKext 返回 EFI_UNSUPPORTED
//     -> 外层统一折成 EFI_INVALID_PARAMETER
//     -> 日志：OC: Prelinked injection ACPCapture.kext (...) - Invalid Parameter
//
// 结构布局（必须与 XNU mach/kmod.h 一致，注意 pragma pack(4)，
// 实测 LegionBtFix.kext 的 _kmod_info 就是 4 字节对齐、总长 0xC4）：
//   offset  size  field
//        0     8  next
//        8     4  info_version   (1)
//       12     4  id             (0xffffffff = 未分配)
//       16    64  name           (CFBundleIdentifier)
//       80    64  version        (CFBundleVersion)
//      144     4  reference_count (-1)
//      148     8  reference_list
//      156     8  address
//      164     8  size
//      172     8  hdr_size
//      180     8  start          -> 返回 KERN_SUCCESS 的桩函数
//      188     8  stop           -> 同上
//   总计 0xC4 字节
//
// 另外 OC 要求 _kmod_info 的地址满足
//   Kmod < LoadAddress + Size - sizeof(KMOD_INFO_64_V1)
// 所以我们在这个结构后面再放一块填充，保证它不落在 __DATA 的最末尾。

#include <stdint.h>

extern "C" int acpcapture_kmod_start(void *info, void *data);
extern "C" int acpcapture_kmod_stop(void *info, void *data);

extern "C" int
acpcapture_kmod_start(void *info, void *data)
{
  (void)info;
  (void)data;
  return 0;   // KERN_SUCCESS
}

extern "C" int
acpcapture_kmod_stop(void *info, void *data)
{
  (void)info;
  (void)data;
  return 0;   // KERN_SUCCESS
}

#pragma pack(4)
struct ACPKmodInfo {
  struct ACPKmodInfo *next;
  int32_t             info_version;
  uint32_t            id;
  char                name[64];
  char                version[64];
  int32_t             reference_count;
  void               *reference_list;
  uintptr_t           address;
  uintptr_t           size;
  uintptr_t           hdr_size;
  int               (*start)(void *, void *);
  int               (*stop)(void *, void *);
};
#pragma pack()

extern "C" {

// 变量名要写 kmod_info（不要写 _kmod_info）：
// Mach-O 会给 C 符号自动加一个前导下划线，于是最终符号正好是 _kmod_info
__attribute__((used, section("__DATA,__data")))
struct ACPKmodInfo kmod_info = {
  nullptr,                 // next
  1,                       // info_version (KMOD_INFO_VERSION)
  0xffffffffu,             // id
  "com.shang.driver.ACPCapture",   // name
  "0.1.0",                 // version
  -1,                      // reference_count
  nullptr,                 // reference_list
  0,                       // address  (加载时由内核填)
  0,                       // size
  0,                       // hdr_size
  acpcapture_kmod_start,     // start
  acpcapture_kmod_stop       // stop
};

// 填充：保证 _kmod_info 不在 __DATA 末尾，否则 OC 的范围检查会失败
__attribute__((used, section("__DATA,__data")))
char _acpcapture_kmod_pad[256] = { 1 };

}
