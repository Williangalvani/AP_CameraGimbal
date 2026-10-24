# `hisi` backend — HiSilicon Hi3516DV500 IP camera

Fixed 4K IP camera on a HiSilicon **Hi3516DV500** (aarch64, Linux 5.10) with a
Sony **IMX678** CSI sensor. Camera (image pipeline) only — no gimbal, thermal,
optical zoom or focus.

## An mt11 derivative on the Hi3519DV500 SDK
`src/backends/mt11` drives the HiSilicon **"ot" MPP** SDK (`ss_mpi_*`,
`sample_comm`) on the SS928. The Hi3516DV500 uses the unified
**Hi3519DV500** SDK: same API family, chip define `-Dhi3519dv500` (there is no
separate 3516DV500 define). `hisi` is mt11's pipeline cut down to one RGB
sensor: no thermal, dual lens, e5739 lens MCU, side-by-side or survey. The
SDK's stock `libsns_imx678.a` drives the sensor, so mt11's hand-rolled
`imx586.cpp` register path is not used. (`src/backends/zt30` is SigmaStar MI,
not a template for this.)

## Build
`make hisi` at the repository root fetches the pinned Hi3519DV500 MPP sample
tree (`tools/bootstrap_hisi_mpp.sh`, a sparse checkout of a public Gitee mirror
into `build/deps/hi3516dv500-mpp`, never committed) and links static aarch64
`camera_app/build/hisi/camera-app` and `web/hisi-web`. The cross prefix defaults to mt11's
`aarch64-linux-gnu-`; override with `HISI_CROSS_COMPILE=<prefix>`. The vendor
firmware itself was built with GCC 10.3 / glibc 2.34, which Bootlin's
`aarch64--glibc--stable-2021.11-1` toolchain matches.

## Pipeline
- **VI/ISP**: `sample_comm_vi_get_default_vi_cfg(SONY_IMX678_MIPI_8M_30FPS_12BIT)`,
  4-lane on MIPI dev 0 / VI dev 0 / pipe 0. The vendor app has a 4K60 10-bit mode,
  so the board wires all 4 lanes. ISP runs on `libsns_imx678` defaults: the
  factory firmware has no IQ/scene files (its tuning is compiled into `xipc`),
  so `scene_auto` is not linked.
- **VB**: sized like the SDK's own 4K venc sample (one 4K 4:2:2 VI pool plus one
  pool per VPSS channel, JPEG and BNR-motion supplements), about 210 MB with
  the defaults. Every pool is logged at startup.
- **VPSS group 0**: channel 0 main (user queue depth 2 for photos), channel 1
  sub, and channel 2 record (only when needed, enabled only while recording).
- **VENC**: 0 = main (`/video1`), 1 = sub (`/video2`), 2 = dedicated H.264
  recorder, 3 = on-demand JPEG still. With the defaults (4K H.264 main, 4K
  recording) recording reuses the main encoder and VENC 2 is not created.
  Recordings then carry the main stream's overlay regardless of `osd_recording`.
- **Rate control**: CBR. The sensor runs at 30 fps; `stream.main` `frame_rate`
  (5-30) sets the main encoder's output rate, and the sub stream and a
  dedicated recorder stay at 30. `bitrate_kbps` / `VIDEO_MAIN_KBPS` applies
  live to the main encoder (so to shared recordings too); 0 (the default) picks
  12 Mbit/s at 2160p, 4 Mbit/s at 1080p and 2 Mbit/s below.
- Overlay reuses `mt11/overlay.cpp` (generic `ss_mpi_rgn`); exposure and image
  controls are mt11's ISP calls on pipe 0.

## Bring-up overrides (environment)
- `CA_HISI_SENSOR_BUS=<0-7>`: sensor I²C bus. The default 3 is the SDK's DMEB
  reference board value; the vendor loads `sys_config.ko ... board=dmeb`. The
  `/dev/i2c-2` and `/dev/i2c-7` strings in `xipc` belong to its GM7122 CVBS
  encoder, not the sensor.
- `CA_HISI_SENSOR_2LANE=1`: use the 2-lane IMX678 mode
  (`SONY_IMX678_2L_MIPI_8M_30FPS_10BIT`).

## Status
Builds, links and starts under `qemu-aarch64`, where it fails cleanly at
SYS/VB init because there are no MPP drivers. It has not run on the camera yet.
On first boot check `/proc/media-mem` against the VB plan, the sensor bus, and
that VENC channels 0 and 1 log their first encoded frames.

## Storage and updates (designed, not implemented)
- `/appfs` is a read-only squashfs (UBI volume on mtd4). Settings (`camera.ini`,
  `web.pass`) live on the writable UBIFS `/date/ap_cameragimbal`; recordings,
  captures and logs go to the microSD card on `/mnt`.
- Replacing `xipc` means our launcher must load the MPP kernel modules and MMZ,
  mount the card and serve the web server's restart requests
  (`/run/camera-app.request` / `.started`). None of it exists yet.
- Updates: zt30-style application slots on `/date` with trial and automatic
  rollback, plus a golden copy in appfs. appfs itself is written only for the
  first install and launcher changes, in the vendor image format, through the
  stock uploader or the rootfs recovery updater (`/usr/hiface/update`, HTTP
  `:9091/fwUpgrade`), which survives our appfs. Recovery, in order: slot
  rollback, the golden copy, telnet (always started by the rootfs), the
  recovery updater, U-Boot serial.
