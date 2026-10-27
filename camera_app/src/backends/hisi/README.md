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

## SDK release
The camera's `ot_*.ko` are `HI3519DV500_MPP_V2.0.1.0 B040`. The public
V2.0.2.0 sample tree is B050, and its ioctl structs differ (VENC stream 0x218
against B040's 0x1d8, channel attributes 0x80 against 0x74, region attach,
ISP sync), so a B050 build fails at its first encoder call. The B040 user
side is public as SDK V1.0.1.1 B040, whose ioctls all match the camera's
drivers. camera-app therefore keeps the camera's kernel modules and builds
against:
- headers from OpenIPC `openhisilicon`;
- the MPP shared libraries from OpenIPC `firmware` (musl builds);
- a B040-era IMX678 driver (`open-source-toolkit/c0e5d`, 4-lane only), which
  ships the IMX515 IQ tables.

`sample_comm` and xop still come from the V2.0.2.0 tree. `sdk_compat.h`
covers what they need that B040 lacks, and `sensor_stubs.c` defines the
sensors that `sample_comm_isp.c` can name but which are not linked.

## Build
`make hisi` at the repository root fetches, pinned and never committed: the
Bootlin musl toolchain (`tools/bootstrap_hisi_toolchain.sh`), the V2.0.2.0
sample tree (`tools/bootstrap_hisi_mpp.sh`) and the B040 tree
(`tools/bootstrap_hisi_b040.sh`). It links `camera_app/build/hisi/camera-app`
against the B040 libraries, with musl's loader at
`/appfs/ap_cameragimbal/lib/ld-musl-aarch64.so.1` (the rootfs has only
glibc), and a static musl `web/hisi-web`. Override the prefix with
`HISI_CROSS_COMPILE=<prefix>`.

## Pipeline
- **VI/ISP**: `sample_comm_vi_get_default_vi_cfg(SONY_IMX678_MIPI_8M_30FPS_12BIT)`,
  4-lane on MIPI dev 0 / VI dev 0 / pipe 0. The vendor app has a 4K60 10-bit mode,
  so the board wires all 4 lanes. ISP runs on the IMX678 driver's defaults: the
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
- **VUI**: every channel sets `bitstream_restriction_flag`, so the SPS declares
  `max_num_reorder_frames = 0`. Without it Chrome's decoder holds the level's
  whole DPB (5 frames at 4K) before showing the first one.
- Overlay reuses `mt11/overlay.cpp` (generic `ss_mpi_rgn`); exposure and image
  controls are mt11's ISP calls on pipe 0.

## Bring-up overrides (environment)
- `CA_HISI_SENSOR_BUS=<0-7>`: sensor I²C bus. The default 3 is the SDK's DMEB
  reference board value; the vendor loads `sys_config.ko ... board=dmeb`. The
  `/dev/i2c-2` and `/dev/i2c-7` strings in `xipc` belong to its GM7122 CVBS
  encoder, not the sensor.

## Status
Builds, links and starts under `qemu-aarch64` from the unpacked appfs image
(every B040 symbol resolves through the musl loader), then fails cleanly at
SYS/VB init because there are no MPP drivers. The supervisor and web UI pass
`make hisi-test` on the host. Nothing has run on the camera yet. On first boot
check `/proc/media-mem` against the VB plan, the sensor bus, and that VENC
channels 0 and 1 log their first encoded frames.

## Storage
`/appfs` is a read-only squashfs (UBI volume on mtd4). Settings (`camera.ini`,
`web.pass`) live on the writable UBIFS `/date/ap_cameragimbal`; recordings,
captures and logs go to the microSD card on `/mnt`.

## Boot (`packaging/hisi`)
rcS starts the rootfs recovery updater, then `/appfs/sh/start.sh`, then
telnetd. Our `start.sh` replaces the vendor one and never starts `xipc`:
it mounts the tmpfs `/tmp` and `/var` that `xipc` used to, then runs
`ap_cameragimbal/service.sh`. The supervisor:
- loads the MPP modules with `xipc`'s list, order and parameters
  (`sys_config.ko sensors=sns0=imx515 board=dmeb`, and MMZ from `mem=` on a
  1 GiB DDR: the stock `mem=384M` gives `mmz=anonymous,0,0x58200000,638M`), except
  `ot_wdt`, which nothing feeds yet;
- mounts the card's first partition on `/mnt` and creates `DCIM/record`,
  `DCIM/capture` and `logs` only on a mounted card;
- seeds `camera.ini` and `web.pass` (`ardupilot`) on first boot;
- runs `hisi-web` on port 80 and camera-app with the web's restart protocol
  (`/run/camera-app.request` / `.started`). A driver or startup failure leaves
  the web UI up and waits for a restart from it.

`/date/ap_cameragimbal/mpp.conf` may set `MPP_SENSOR`, `MPP_BOARD`,
`MPP_DDR_MB`, `MPP_MMZ_START` or `MPP_MMZ_SIZE` for bring-up. The address
needs no setup: the recovery updater applies the saved `eth0` MAC, IP and
gateway from `/appfs/fconfigs/param/updatetype.ini` at boot, independently
of `xipc`. Logs are in `/run` (`camera_app.log`, `hisi-web.log`,
`hisi-service.log`).

## Updates
`make hisi_appfs` writes `build/HISI_AP_<version>_<hash>_appfs.img`, a whole
appfs (mtd4) image in the vendor update format: a squashfs in a UBI image in
the vendor container. Three uploaders take it, and each erases mtd4, writes
the image and reboots:
- the vendor web UI's firmware upgrade, which only treats `.img` files as
  camera firmware (`.bin` goes to its driver-board upgrade);
- the rootfs recovery updater (`/usr/hiface/update`, HTTP `POST
  :9091/fwUpgrade`, raw body), which survives our appfs;
- our Status page, which forwards to the recovery updater.

Our Status page also takes the vendor's own appfs releases (e.g.
`IPC_18.010.U35.5_0054_V16(260305).img`). Their volume reserves the whole
partition (265 PEBs) rather than just the PEBs in the image, as ours does.
`tests/test_hisi_firmware.py` checks the format and the upload names. With
`HISI_VENDOR_IMAGE` set to a vendor `.img`, it also checks that our web
validator accepts it. With `HISI_VENDOR_ROOTFS` set, it runs the vendor
updater under qemu against our image.

Planned: zt30-style application slots on `/date` with trial and automatic
rollback, with the appfs copy as the golden fallback, so routine updates do
not rewrite appfs. Recovery, in order: slot rollback, the golden copy, telnet
(always started by the rootfs), the recovery updater, U-Boot serial.
