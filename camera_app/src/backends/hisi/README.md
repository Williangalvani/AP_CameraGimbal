# `hisi` backend — HiSilicon Hi3516DV500 IP camera

Fixed 4K IP camera on a HiSilicon **Hi3516DV500** (aarch64, Linux 5.10) with a
Sony **IMX678** CSI sensor. Camera (image pipeline) only — no gimbal, thermal,
optical zoom or focus.

## Why this is mostly an mt11 derivative
`src/backends/mt11` already drives the HiSilicon **"ot" MPP** SDK (`ss_mpi_*`,
`sample_comm`, `scene_auto`) for the `ss928v100` (Hi3519DV500), the direct
sibling of our Hi3516DV500 (the unified SDK ships "Hi3519DV500 & Hi3516DV500").
The SDK already includes our sensor libs (`libsns_imx678_2l.a`,
`libsns_imx662*.a`) and the full ISP suite, so we skip mt11's hand-rolled
sensor-register path (its `imx586.cpp` / `*_registers.inc`) and let the stock
`sns_imx678_2l` lib drive the sensor. We derive from mt11's pipeline rather than
writing MPP from scratch. (Note: `src/backends/zt30` is SigmaStar MI, not
HiSilicon — not our template.)

mt11 is dual-sensor (zoom+wide) + thermal + e5739 lens MCU; `hisi` keeps only
the single RGB sensor path.

## What's wired so far (this branch)
- `include/apcam/target_hisi.h` — target properties (single RGB lens, 2 streams,
  photo, MAVLink-only, no gimbal/thermal).
- `include/apcam/target_ids.h`, `target.h`, `targets.mk` — register `APCAM_TARGET_HISI`.
- `camera_app/Makefile` — `hisi` in `SUPPORTED_BACKENDS`; aarch64 + `-static`;
  `HISI_MPP_ROOT` points the existing SS928 media machinery at the Hi3516DV500
  SDK. **The SDK is supplied by the builder and never committed** (license-clean,
  same as zt30/rpi). Default: `../build/deps/hi3516dv500-mpp`.
- `control.cpp` — reuses the optional-gimbal CADDX control (no gimbal unless a
  UART gimbal is configured).
- `media.cpp` — `APC_Media_Backend::create()` + full backend surface, but the
  MPP pipeline is a **TODO** (`ready()` is false; no video yet).

## Remaining work to produce video (needs the SDK present to compile/iterate)
1. **Media objects/libs in the Makefile.** `hisi` falls into the mt11-style link
   arm (the final `else`: aarch64, `-static`, links `MEDIA_LIBS` in
   `--start-group`). The `OBJECTS` list is currently mt11-specific
   (`build/mt11_media.o`, `mt11_isp.o`, `mt11_imx586.o`, …). Gate it so for
   `hisi` it builds `hisi_media.o / hisi_pipeline.o / hisi_isp.o / hisi_overlay.o`
   instead, keeps `sample_comm_*` + `scene_*` + xop RTSP + minimp4, and **drops
   the thermal objects/codecs** (`mt11_thermal`, `e5739`, `THERMAL_*`). Link the
   `sns_imx678_2l` sensor lib (already in `OTHER_SENSOR_LIB_NAMES`).
2. **Chip define.** mt11 compiles with `-Dss928v100`. Confirm the correct macro
   for Hi3516DV500 in the SDK (the unified SDK selects the chip variant) and
   adjust `MEDIA_DEFINES` for `hisi`.
3. **Pipeline (`pipeline.cpp` + `isp.cpp`, ported from mt11, single sensor).**
   Mirror `ca_mt11_system_init / configure_vi / vpss_start / scene_start /
   video_attributes` but for ONE sensor: VI (IMX678 via `sns_imx678_2l`) -> ISP
   (+ `scene_auto` with an IMX678 IQ file) -> VPSS (main + sub) -> VENC
   (H.265 main / H.264 sub / JPEG snap). Board specifics to confirm from the
   device / `libusrplatform.so`: MIPI lane map + data rate, I²C bus
   (`/dev/i2c-2|7`), and the IMX678 IQ file path (kept on-device, not in repo).
4. **media.cpp**: port mt11/media.cpp's APC_Media_Backend, dropping thermal and
   dual-lens. **Photo**: JPEG via a dedicated VENC channel. **Exposure**: via ISP.

## Firmware update / deployment (fixed camera, safe-by-default)
The device is UBI-on-SPI-NAND; partitions: U-Boot, env, kernel, DTB,
UBI(appfs+rootfs), and a writable `/date`. Keep our footprint to the **appfs
application partition** — the vendor's own update model, which is why their
`.img` was appfs-only.

- **Build output = an appfs image.** Our `camera-app` + web + assets are packaged
  as the appfs UBI volume payload, replacing the vendor `xipc`. Kernel / rootfs /
  U-Boot are left untouched by normal updates.
- **Flashing** uses the base rootfs's MTD/UBI tools (`ubiupdatevol` / `ubiformat`
  / `flashcp`) — already on the device; we don't reinvent flashing.
- **Upload orchestration** is provided by AP_CameraGimbal's own web firmware
  upload (`WEB_HAVE_FIRMWARE_UPLOAD`, apbundle-style), wired to write the appfs
  volume. We can also keep accepting the vendor's image format (magic
  `cbbccaab`, CRC32 at +0xC0, mtd target string) if we want the stock web
  uploader to accept our images during bring-up.
- **Validation before flashing**: verify magic + size + CRC32 (and ideally a
  signature) and that the payload mounts as a valid squashfs; refuse otherwise.
- **If an image is invalid / appfs is bad**: because only appfs is rewritten, the
  device still boots its intact kernel + base rootfs and shell (UART/telnet); the
  app simply doesn't start. Recovery = reflash appfs. Low brick risk. Never let
  the normal update path rewrite U-Boot/kernel without an A/B scheme + a verified
  U-Boot recovery (TFTP/Ymodem) fallback.
