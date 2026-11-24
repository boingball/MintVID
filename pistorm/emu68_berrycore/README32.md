# Emu68 1.0.7 BerryCore kernel for PiStorm32 lite (CM4)

`berrycore32_patch.S` + `build_berrycore32.py` put the BerryCore CPU1 service
(HDMI sound modules AAC/AC3/E-AC3, see `berrycore_patch.S` and `README.md`)
into the original Emu68 1.0.7 PiStorm32 lite kernel
(`Emu68-pistorm32lite`, 08.12.2025, git 305f686,
sha256 262afc1473299d1e95cffd6ebff6427b4b737776e8380d264cf52bdb1878a9e0):

    python3 build_berrycore32.py upstream32/Emu68-pistorm32lite Emu68-pistorm32lite-BerryCore

| Offset | Pi 3 kernel | PiStorm32 lite kernel |
|---|---|---|
| version strings | 0x5B, 0x1540C3 | 0x5B, 0x1607E3 |
| secondary_boot, CPU1 skips `async_log` | 0x654 | 0x654 |
| former `serial_writer()` -> branch to service | 0x8A00 | 0x8A60 |
| BerryCore service (932 bytes, zero padding before `.trailer`) | 0x20F500 | 0x21B600 |

The service code is byte-identical to the Pi 3 BerryCore service. The two
kernels were compared instruction by instruction around `secondary_boot()`
and `serial_writer()`.

Emu68 © Michal Schulz, MPL-2.0 (https://github.com/michalsc/Emu68, tag v1.0.7).
