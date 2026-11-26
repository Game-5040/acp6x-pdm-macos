# ACP6x PDM Microphone for macOS

A microphone driver for the **AMD ACP6x / ACP6.3 PDM (digital MEMS) microphone array** found in
Ryzen laptops, written from scratch for macOS 15/26 (Hackintosh or otherwise). It consists of a small
PCI kext that programs the ACP DMA engine, and an input-only userspace HAL plugin that makes the
result show up as a normal CoreAudio input device.

Tested on an **HP OMEN 16-ap1xxx** (board `8F06`, Ryzen 9 9955HX, RTX 5070 Laptop, macOS 26.7.3):

| Thing | Status |
|---|---|
| Capture 48 kHz / 2ch / S32_LE from the PDM array | ✅ works |
| System input device (Voice Memos, QuickTime, Zoom, browser…) | ✅ works |
| Built-in microphone in Siri | ✅ works (needs the `'imic'` data source control, see below) |
| macOS Dictation | ✅ works |
| Hardware AEC / beamforming | ❌ not implemented |
| Sleep/wake re-arm | ⚠️ re-open the device after wake (`StopIO`/`StartIO` re-arms it) |

No Apple developer account, no Xcode and no DriverKit entitlement are needed: everything builds with
the Command Line Tools and `clang`.

## Why this exists

On many Ryzen laptops the analog codec (e.g. Realtek ALC245) has **no microphone wired to it** — the
built-in mic is a PDM array that hangs off the AMD Audio CoProcessor. Windows drives it through the
ACPDM endpoint; Linux does via `snd_pci_acp6x` + `dmic-codec`; macOS has neither, so the codec's
input node appears but records pure silence.

The ACPI on these machines frequently never declares the PDM device at all (`_WOV` missing,
no `acp-audio-device-type` property), which is why the Linux quirk for this exact board exists
([`acp6x-omen-dkms`](https://github.com/NevetScar/acp6x-omen-dkms) matches `DMI_BOARD_NAME = "8F06"`
and skips the ACPI check). This driver skips ACPI entirely and talks to the registers directly.

## Requirements

- macOS 15 (Sequoia) or 26 (Tahoe), x86_64, OpenCore with kernel injection (`Lilu` not required).
- A machine whose ACP PCI device is `pci1022,15e2` and whose `ACP_PIN_CONFIG` reads outside
  `{0,1,2,3,9,15}` (that is the PDM-mode indicator — see `probe/`).
- SIP such that kext injection works, and the ability to install a HAL plugin into
  `/Library/Audio/Plug-Ins/HAL`.

## Install

```sh
# 1. diagnose first (read only): build and inject the probe kext
cd probe && ./build.sh
cd .. && sudo ./tools/install-probe.sh install
# reboot, then
ioreg -rc ACPProbe -l | grep -E "pinConfig|pgfsmStatus|barLen"

# 2. capture kext
cd capture && ./build.sh
cd .. && sudo ./tools/install-capture.sh install
# reboot

# 3. HAL plugin → the device appears in Sound settings
cd plugin && ./build.sh && sudo ./install.sh
```

Under OpenCore, `tools/install-capture.sh` adds `Kexts/ACPCapture.kext` plus the matching
`Kernel → Add` entry in `config.plist` (with a backup and an `ocvalidate` run). If you use a different
bootloader, inject `ACPCapture.kext` yourself.

Quick check without booting anything special:

```sh
cd tools/acpcap && clang -O2 -o acpcap acpcap.c -framework IOKit -framework CoreFoundation
./acpcap --status          # ring/linear counters from the kext
./acpcap 5 /tmp/test.wav   # 5 seconds, DC-blocked WAV
```

## How it works

```
userspace  ACPMicPlugin.driver (HAL plugin)
             │  IOServiceOpen(ACPCapture) + IOConnectMapMemory64(ring) + scalar methods
             ▼
kext       ACPCapture.kext (IOService on pci1022,15e2)
             │  BAR0 mmio: PTE table → ring buffer → PDM enable
             ▼
hardware   AMD ACP6x PDM DMA engine → digital MEMS microphone array
```

Key registers (offsets relative to BAR0, see `regs/acp63_regs.h`):

| Offset | Register | Note |
|---|---|---|
| `0x01028` | `ACP_PGFSM_STATUS` | read `0` = already powered on, no power sequence needed |
| `0x01440` | `ACP_PIN_CONFIG` | outside `{0,1,2,3,9,15}` ⇒ PDM mode |
| `0x02C04` | `ACP_WOV_PDM_ENABLE` | |
| `0x02C08` | `ACP_WOV_PDM_DMA_ENABLE` | poll for bit 1 = enabled |
| `0x02C0C` | `ACP_WOV_RX_RINGBUFADDR` | |
| `0x02C14` | `ACP_WOV_RX_LINKPOSITIONCNTR` | |
| `0x02C18/1C` | `ACP_WOV_RX_LINEARPOSITIONCNTR` | absolute byte counter |
| `0x02C24` | `ACP_WOV_PDM_FIFO_FLUSH` | write before enabling, removes the startup pop |
| `0x02C28` | `ACP_WOV_PDM_NO_OF_CHANNELS` | write `0` (TWO_CH); the firmware leaves `1` behind |
| `0x02C2C` | `ACP_WOV_PDM_DECIMATION_FACTOR` | write `2` |
| `0x02C5C` | `ACP_WOV_MISC_CTRL` | gain in bits 4:3 |
| `0x02C60` | `ACP_WOV_CLK_CTRL` | write `7` |
| `0x0010000` | `ACP_SCRATCH_REG_0` | page table entries (low, high\|BIT31), 8 bytes per 4K page |

The DMA buffer is not addressed directly: the engine walks a page table in the ACP SRAM window,
which the kext fills page by page. See `docs/DESIGN.md` for the full write-up, including the register
dump of this machine and the three assumptions that turned out to be wrong.

### The Siri trap

Siri refuses a microphone that does not identify itself as a built-in mic, **no matter how well it
records**. `+[AVVCAudioDeviceManager IsDeviceBuiltIn:]` wants `transport == 'bltn'` and the input
`kAudioDevicePropertyDataSources` (`'ssc#'`) to contain `'imic'`; `CSSiriRecordingInfo` additionally
requires `'imic'`/`'emic'` for `'bltn'` transports (USB/Bluetooth/Line are exempt, which is why those
microphones always worked).

Those two properties cannot be answered directly: coreaudiod **synthesises** them from a
`kAudioDataSourceControlClassID` (`'dsrc'`) control in the device's `kAudioObjectPropertyControlList`.
The plugin therefore exposes object 6:

| Property | Value |
|---|---|
| `BaseClass` | `'slct'` |
| `Class` | `'dsrc'` |
| `AvailableItems` | one item, `'imic'` |
| `CurrentItem` | `'imic'` (settable) |
| `ItemKind` | `'dsrc'` |
| device `ControlList` | `4, 5, 6` |

After that, `AudioObjectGetPropertyData('ssrc')` returns `'imic'` and Siri starts listening.

## Known limitations

- **No AEC, no beamforming, no noise suppression.** The array is two capsules roughly 2–3 cm apart;
  the plugin treats it as a stereo input. Use `VoiceProcessingIO` in your app if you need AEC.
- **DC offset is huge** (L ≈ −0.14 FS, R ≈ −0.42 FS on this unit). It is removed by a DC blocker plus
  a 90 Hz second-order high-pass; without that the real signal is barely 1 % of full scale.
- **Gain staging is empirical.** The plugin uses a speech-envelope AGC plus a hysteresis gate
  (`2.5×` open / `1.4×` close, 320 ms hangover); tune `kAGC_TargetRMS` if your capsule differs.
- **Only one PDM instance** is handled, and only at 48 kHz (the hardware decimation factor is fixed).
- `start()` must stay lazy: writing the page table from `start()` resets the machine on some units.

## Credits

Prior art that made this possible:

- [`NevetScar/acp6x-omen-dkms`](https://github.com/NevetScar/acp6x-omen-dkms) — the Linux patch for
  this exact board (shows the DMI bypass and the `ACP_PIN_CONFIG` test).
- Linux `sound/soc/amd/ps/ps-pdm-dma.c` and `include/sound/acp63_chip_offset_byte.h` — the register
  sequence.
- [`shubhambanekar/LatSOFAudio`](https://github.com/shubhambanekar/LatSOFAudio) — HAL plugin structure
  and the analysis of Siri's `'imic'` gate.
- [`DexterSLamb/CmlSOFAudio`](https://github.com/DexterSLamb/CmlSOFAudio) — the kext
  `IODMACommand` + `IOConnectMapMemory64` pattern.

## License

MIT — see `LICENSE`.
