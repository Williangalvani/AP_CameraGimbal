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
  ships the IMX515 IQ tables and is compiled from source into camera-app.

`sample_comm` and xop still come from the V2.0.2.0 tree. `sdk_compat.h`
covers what they need that B040 lacks, and `sensor_stubs.c` defines the
sensors that `sample_comm_isp.c` can name but which are not linked.

The camera's V2.0.1.0 B040 `ot_isp.ko` numbers the ISP ioctls from 0x6c up one
higher than the V1.0.1.1 libraries do. `b040_compat.c` interposes `ioctl` and
the MPP mmap calls to bridge this and two smaller differences. It renumbers
only when `/proc/umap/isp` names `HI3519DV500_MPP_V2.0.1.0 B040`; with other
drivers it logs that it passes ISP ioctls through unchanged.

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
  sub (depth 1 for one-push white balance), and channel 2 record (only when needed, enabled only while recording).
- **VENC**: 0 = main (`/video1`), 1 = sub (`/video2`), 2 = dedicated
  recorder, 3 = on-demand JPEG still. Recordings are MP4 in the main stream's
  codec: H.264 (`avc1`) or H.265 (`hvc1`). When the recording resolution is
  the main resolution (the defaults: 4K main, 4K recording) recording reuses
  the main encoder, in either codec, and VENC 2 is not created. Recordings
  then carry the main stream's overlay regardless of `osd_recording`. Only a
  different recording resolution creates VENC 2, in the main codec.
- **Rate control**: CBR. The sensor runs at 30 fps; `stream.main` `frame_rate`
  (5-30) sets the main encoder's output rate, and the sub stream and a
  dedicated recorder stay at 30. The frame rate applies when camera-app
  starts: a save reports "Restart to apply VIDEO_MAIN_FPS", and Save and
  restart applies it. `bitrate_kbps` / `VIDEO_MAIN_KBPS` applies
  live to the main encoder (so to shared recordings too); 0 (the default) picks
  12 Mbit/s at 2160p, 4 Mbit/s at 1080p and 2 Mbit/s below.
- **Keyframe interval**: `[stream.main]` / `[stream.sub]`
  `keyframe_interval_ms` (`VIDEO_MAIN_KEYMS` / `VIDEO_SUB_KEYMS`), 30-10000 ms,
  default 1000. The encoder's GOP is `max(1, round(ms * fps / 1000))` frames,
  at the main encoder's configured `frame_rate` and the sub stream's 30 fps, so
  an interval below one frame makes every frame a keyframe. The dedicated
  recorder (VENC 2) keeps 30 frames (1 s); recordings that share the main
  encoder (the defaults) get the main interval. A change is set like the
  bitrate, without a restart (`ss_mpi_venc_get_chn_attr`, the CBR `gop`,
  `ss_mpi_venc_set_chn_attr`); whether the encoder takes a new GOP on a
  running channel is not yet verified on hardware. If it refuses, camera-app
  logs the SDK error and keeps the old GOP and bitrate: a MAVLink set is
  refused, and a web save reports "Could not apply saved settings" (the saved
  value then applies when camera-app next starts). The "media ready" log line shows each stream's `gop` in
  frames, and a live change logs "main/sub stream keyframe every N frames".
  The keyframes RTSP PLAY, RTCP PLI/FIR and frame-socket clients ask for
  (`ss_mpi_venc_request_idr`) are separate and unchanged. At CBR a keyframe
  takes several times the bits of another frame, so a shorter interval
  recovers from loss and lets players join sooner but lowers the quality of
  the whole picture at the same bitrate; a longer one does the opposite.
  Recordings that share the main encoder get its interval too: with a long
  one, players seek only to keyframes that far apart, and a crash or power
  cut can lose up to that much at the end of the file. The CBR rate control's
  `stats_time` stays 1 s (`pipeline.cpp`) whatever the GOP; whether a GOP
  longer than 1 s needs `stats_time` of at least GOP/fps to avoid quality
  pumping is still to be checked on hardware.
  The 4K Cam Manager `gop` maps to it; see `docs/4kcam-manager-compat.md`.
  The `gop` it reports is computed from the saved `frame_rate`, which applies
  only when camera-app restarts, so after a frame rate change it can differ
  from the running encoder's until then (the "media ready" line shows that).
- **VUI**: every channel sets `bitstream_restriction_flag`, so the SPS declares
  `max_num_reorder_frames = 0`. Without it Chrome's decoder holds the level's
  whole DPB (5 frames at 4K) before showing the first one.
- **Frame socket** (`frame_socket.h`): the main and sub encoders' access units
  on the abstract Unix socket `@apcam/video`, for MAVLink Camera Manager's
  `apcamsrc` (`tools/mcm_trial/apcamsrc`). A client names a stream, gets its
  codec, size and rate, then every access unit with its encoder timestamp,
  starting at a key frame. It can ask for a key frame (an IDR, at most every
  250 ms per stream). Up to 4 clients; one that falls 15 frames behind skips
  to the next key frame. Frames go to the socket before RTSP, whose push takes
  ~15 ms on a 4K key frame. With no client connected, publishing costs nothing.
- **IR-cut**: a two-wire motor on sysfs GPIO 72 (day, filter in) and 73
  (night). `ircut.cpp` pulses day once per process from a detached thread at
  media start-up; a missing sysfs GPIO is logged and ignored.
- **DC iris** (U35.5 board, device code 19/2/51, only): the lens has a DC iris
  on PWM0 that stays shut, so the picture is black, until something drives it.
  The behaviour is matched to the vendor firmware by experiment. Before VI/ISP
  start-up `iris.cpp` sets the PWM clock enable (low byte of `0x11014598` to
  `0x10`, upper bits kept) and muxes pad `0x10260058` to PWM0 (`0x1105`, against
  `0x1200` otherwise) through `/dev/mem`. Once the ISP runs, `isp.cpp` turns on
  the ISP's auto DC iris (`min_pwm_duty` 10, `kp` 7000, the rest left at the
  SDK defaults), and the ISP then drives PWM0 through `/dev/pwm` (period 1000;
  the vendor firmware was measured holding duty 950). The board is told from
  `/appfs/fconfigs/param/updatetype.ini`: any other device code, or no file,
  leaves the pins and the ISP alone, as on the MAIN V9 (19/2/35) bench camera.
  The start-up log says whether the DC iris was enabled or skipped, and why. A
  register or ISP failure is logged and the video starts anyway.
  Every image apply sets the auto DC iris again (enabled, auto, DC type, not
  forced open or shut); manual shutter, ISO and gains leave it in auto,
  as the vendor firmware does, so it still moves towards the AE target.
- **Iris priority** (`[image] iris_mode = priority`, `IMG_IRIS_MODE` 1, DC iris
  board only): the shutter and gains stay fixed and the DC iris alone holds the
  brightness at the AE target, which `exposure_compensation` and `backlight`
  still move. The AE stays automatic, with each range pinned to one value:
  exposure time the `shutter` (1/60 s when it is auto); analog gain the ISO's
  (ISO 100 1×, doubling per step), or with ISO auto and `gain_mode` manual the
  enabled manual gains (`dgain` is the ISP digital gain), else 1×; sensor
  digital gain 1×; the system gain their product. `max_exposure`,
  `ae_strategy` and `slow_shutter` don't apply, and the frame rate stays fixed.
  So the exposure time is at most one frame, `1000000 / fps` µs at the
  sensor's frame rate (the ISP's `frame_rate`, 30 fps; `APCAM_FRAME_RATE` if
  the ISP doesn't report it). The configured stream rate doesn't matter: the
  encoders drop sensor frames to reach it. A longer shutter (1/12 or 1/25 s at
  30 fps) is pinned at one frame (1/30 s) and logged as `iris priority:
  shutter 1/12 s needs more than one frame at 30 fps, using 1/30 s`. Only this
  mode clamps; camera.ini, `IMG_SHUTTER` and the 4K Cam Manager keep the
  requested shutter, so a MAVLink client reads back its own request.
  The pinned values come from `iris_priority_exposure()` in `image_tuning.h`
  and are logged as `ISP iris priority: ...`; the `ISP tuning` line shows
  `iris=`. Leaving the mode puts back the AE ranges it replaced. The binlog's
  AE record reads the ISP's exposure, so it shows the pinned (clamped) time.
  For the web, camera-app writes `<CAMERA_APP_READY_PATH>.iris` (temporary
  file, then rename) once the media has started and the DC iris outcome is
  known, and after every image apply: `pid`, `dc_iris`, `priority_active`,
  `shutter_requested_us` and `shutter_applied_us` (both 0 outside the mode),
  one `key=value` per line (`include/apcam/hisi_iris.h`). The web's **Iris
  mode** row uses it when the `pid` is the running camera-app's, and otherwise
  makes camera-app's start-up decision itself (device code and
  `CAMERA_APP_DC_IRIS`). Without a working DC iris the row is shown disabled,
  with the saved value and "Not available: this camera has no working DC
  iris."; a save leaves the value as it is, and one asking to switch to
  priority is refused. While the mode is applied the row shows the applied
  shutter, and the requested one when the frame rate cut it.
  On a camera without a working DC iris a live change to priority is refused
  (`iris priority needs the DC iris ...`) and the old settings stay. At
  start-up, or while a saved `priority` already had no effect, it is logged
  and the normal exposure applies instead; camera.ini keeps `priority`. At
  start-up an ISP that refuses the pinned ranges also falls back.
  To be verified on hardware: that the ISP accepts ranges with equal minimum
  and maximum and that the iris then holds the target. If it doesn't, two
  fallbacks are candidates, neither implemented: widen each maximum by one
  step, or set a fully manual exposure and drive the iris from a software
  loop on its `hold_value`.
- Overlay reuses `mt11/overlay.cpp` (generic `ss_mpi_rgn`); exposure and image
  controls are mt11's ISP calls on pipe 0, except metering. Of the 15x17 AE
  zones, `center` weights the middle 5x5 by 8 against 1 elsewhere, and
  `spot` weights only the middle 3x3 (about 3.5% of the picture). mt11 still
  weights one middle zone 15 against 1, which moved the exposure by under 1%
  here.
- **White balance** differs from mt11. Auto uses the vendor's AWB calibration
  for this sensor and lens (`isp.cpp`). The presets and `temperature` ask the
  ISP for that calibration's gains at a colour temperature, and fall back to
  auto if it has none. `manual` takes R/G/B gains in 1/256, defaulting to the
  calibration's 4950 K gains. One-push (Photos page, green or blue water)
  runs on its own thread (`one_push_wb.h`, `media.cpp`): it sets manual gains,
  waits 20 ms, averages the centre half of a channel 1 frame as BT.709 RGB and
  steps the gains until the channels match, then saves them as manual gains.
  ISP attributes and statistics have their own mutex, taken only per step, so
  a one-push does not stall recording or the main loop.
- **Temporal noise reduction**, against ghosting behind moving objects at
  night: four `[image]` keys, no MAVLink names, live like the other image
  settings and on the Parameters page. Spatial NR is not touched. Absent,
  `tnr` is off and the others keep what the camera did before: the IMX678
  driver's Bayer NR strength and the VI start-up's 3DNR.
  - `tnr` (default `false`) is the ISP Bayer NR's `tnr_en`; `tnr_strength`
    0–255 (default 255, the driver's) its `tfs` in every ISO bin and in the
    manual set.
  - `vi_3dnr` (default `true`) is the VI pipe 3DNR's `enable`
    (`ss_mpi_vi_set_pipe_3dnr_attr`); `vi_3dnr_strength` 0–100 (default 100)
    scales, in percent, the pipe's start-up 3DNR parameters, read once at the
    first apply: each `tfy[].tfs0`/`tfs1` and `nrc0.trc`. 100 sets nothing.
    The parameters are read as the SDK's `scene_auto` does (V1, manual); if
    the pipe reports automatic mode with an ISO table, the table is copied
    and scaled too, otherwise only the manual set is scaled (logged). A pipe
    that refuses to turn its 3DNR off while running is logged and gets
    strength 0 instead.
  Out-of-range values make camera.ini invalid, as for the other keys. An SDK
  refusal is logged and the video carries on; the `ISP tuning` line shows
  what is in effect, e.g. `tnr=on:255 vi_3dnr=on:100`. Not yet checked on
  hardware: whether the pipe takes a live 3DNR enable change and a manual
  parameter set, and how much each key helps at night.

## Bring-up overrides (environment)
- `CA_HISI_SENSOR_BUS=<0-7>`: sensor I²C bus. The default 3 is the SDK's DMEB
  reference board value; the vendor loads `sys_config.ko ... board=dmeb`. The
  `/dev/i2c-2` and `/dev/i2c-7` strings in `xipc` belong to its GM7122 CVBS
  encoder, not the sensor.
- `CAMERA_APP_DC_IRIS=1` or `0`: forces the DC iris on or off whatever the
  device code says. Unset, empty or any other value leaves it to the board.

## Status
Runs on the camera in place of `xipc`: 4K30 H.264 on RTSP `/video1` and the
sub stream on `/video2`, the Live page, image controls, white balance
including one-push, photos and recording to the microSD card, and updates
from the Status page. The web UI's photo, record and format buttons work
without a flight controller (camera-app answers commands from loopback TCP
clients before it has a system ID). An 83 s 4K30 recording at the default
12 Mbit/s wrote about 1.5 MB/s, decoded without errors and had every frame
(2497). The supervisor and web UI pass `make hisi-test` on a Linux host.

## Checking the controls
`tools/hisi_image_check.py run` (see `tools/README.md`) steps each setting
through known values on a running camera and judges the change in frames
grabbed from RTSP, or with `ffprobe` for stream format, then restores every
field. On the bench (indoor, still scene) every image, exposure, gain, white
balance, overlay, orientation and stream setting passed, as did photo
capture and one-push white balance. Centre-weighted metering was UNSURE:
the scene's centre is within 5% of its average, so it cannot show it. Some
effects are uneven by design: exposure compensation follows the vendor's
steps (-6 darkens the picture by about 30%, +6 doubles it), and shutter
times longer than 1/30 are held to the frame time unless `slow_shutter` is
on.

`tools/hisi_mavlink_check.py run` does the same through the MAVLink camera
parameters (`PARAM_EXT_SET`, as QGroundControl uses them with `camera.xml`).
The 4K Cam Manager's own controls (hue, sharpness, gamma, gains and the AE
limits) have no MAVLink names. On the bench the camera definition, readback,
range checks, `PARAM_SET`, brightness, contrast, saturation, exposure, ISO,
the overlay cross, every stream setting including `VIDEO_MAIN_FPS`, and
`MAV_CMD_IMAGE_START_CAPTURE` passed. `VIDEO_MAIN_FPS` used to be missing
from `camera.xml`; it now saves and reports "restart camera-app to apply"
there as through `PARAM_SET`. Shutter, spot metering, every white balance
parameter, `CAM_WB_ONEPUSH` and `MOUNT_ORIENT` passed too; centre-weighted
metering was UNSURE for the same reason as above.

## Storage
`/appfs` is a read-only squashfs (UBI volume on mtd4). Settings (`camera.ini`,
`web.pass`) live on the writable UBIFS `/date/ap_cameragimbal`; recordings,
captures and logs go to the microSD card on `/mnt`.

On the board tested so far, cards never leave the 400 kHz identification
clock (about 170 KiB/s): the kernel's switch to a UHS bus speed fails with
"mmc0: Problem setting bus speed mode!" and it then keeps the slowest clock.
`sdcard.sh` works around it by moving the SDIO clock select in the CRG
register `0x110135c0` (bits 24-26) to 25 MHz once a card is up, and back to
400 kHz when it is removed. Reads then run at about 11 MB/s and writes at
about 10 MB/s, well above a 4K recording's 1.5 MB/s. The cause may
be the board's 1.8 V I/O switch for the slot, since the device tree
advertises SDR104.

`ap_cameragimbal/sdbench.sh [SIZE_MIB]` measures a card from the shell: it
writes SIZE_MIB (default 256) to `/mnt/.sdbench.tmp`, drops the page cache,
reads it back and prints both speeds, after the card's name, size, bus
timing and clock. It refuses while any process has a file open on the card
(recording, logging or a download; it names the process), and holds
`sdcard.sh`'s lock so the card is not mounted or formatted meanwhile. It
also holds `/run/sdbench.lock` (`SDBENCH_LOCK`), so benchmarks run one at a
time from the shell and the web alike, and camera-app checks it
(`CAMERA_APP_STORAGE_BENCH_LOCK`): while it is held, starting a recording
or a BIN log fails with EBUSY, auto-record waits, and a MAVLink format is
`TEMPORARILY_REJECTED`. The write and the read are each stopped after
`SDBENCH_TIMEOUT` seconds (default 180) with BusyBox or coreutils `timeout`,
or a watchdog without it, so a failing card cannot keep the locks. The
temporary file is removed when it exits. Its exit status: 0 done, 2 usage,
3 card not ready, 4 card busy (a lock is held), 5 a file on the card is
open, 6 not enough space, 1 any other failure; the reason is on its last
`sdbench: ` line.

The Status page's "Benchmark microSD card" button (a ready card only) runs
it with 128 MiB in the background (`SDBENCH_TOOL`, exported by `service.sh`)
and shows its progress, then the write and read speeds, the card and bus
lines, or its refusal (translated for statuses 3-6), on a "microSD speed"
row that stays until the next benchmark or boot. `GET /storage/state`
reports it as `bench`. The output goes to `/run/sdbench.result`, whose
first line is `running`, then `done` or `failed <status>`; the web takes
`/run/sdbench.lock` itself and passes it to the tool
(`SDBENCH_LOCK_INHERITED=1`), and one left `running` without the lock
(killed) is reported failed. It is detached from hisi-web and survives its
restarts. The page stops following a benchmark after 10 minutes.

The Format and Benchmark forms answer with a 303 to the Status page
(`/?sdcard=format&since=…`, `/?sdcard=bench`, or `/?notice=N` for a
refusal), so reloading it never repeats the POST; `storage.js` then drops
the query. `sdcard.sh format` leaves its result in `/run/sdcard.state.format`
(`ok UPTIME` or `failed REASON UPTIME`), which `GET /storage/state` reports
as `format`; the page waits for a result newer than the one before its
format (`since`) rather than taking a ready card for success, for up to
10 minutes.

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
- seeds `camera.ini` and `web.pass` (`ardupilot`) when they are missing or
  empty, through a temporary file;
- runs `udhcpc` while `camera.ini` sets `[network] dhcp = true` (Parameters →
  Network → DHCP client), checked every second, so a saved change applies
  without a camera-app restart;
- runs `hisi-web` on port 80 and camera-app with the web's restart protocol
  (`/run/camera-app.request` / `.started`). A camera-app that crashes or fails
  to start is restarted after 2, 10 and 30 s; after a fourth failure in a row
  (a run of 10 minutes resets the count) it leaves the web UI up and waits for
  a restart from it. So does a driver load failure. A camera-app that ignores
  SIGTERM is killed after 15 s. If `hisi-web` exits it is restarted, backing
  off to 10 s while it keeps exiting at once.

A camera-app that dies without tearing down the ISP (a crash or SIGKILL) leaves
`ot_isp.ko` initialised for pipe 0, and every later start fails with "ISP[0]
already inited" (0xa01c800c). Only a reboot clears it: reloading the drivers
can panic the kernel. So when a start fails with that message, the supervisor
reboots the camera, at most 3 times in a row (counted in
`/date/ap_cameragimbal/stale-isp-reboots`, cleared once camera-app is ready);
after that it falls back to the restart delays above.

There is no hardware watchdog (`ot_wdt` is not loaded, and unloading it once
loaded hung the board), so a kernel or driver hang needs a power cycle.

`/date/ap_cameragimbal/mpp.conf` may set `MPP_SENSOR`, `MPP_BOARD`,
`MPP_DDR_MB`, `MPP_MMZ_START` or `MPP_MMZ_SIZE` for bring-up. A file that does
not run is reported and ignored, so it cannot keep the web UI down. The address
needs no setup: the recovery updater applies the saved `eth0` MAC, IP and
gateway from `/appfs/fconfigs/param/updatetype.ini` at boot, independently
of `xipc` (192.168.2.10 via 192.168.2.1 on the units seen so far). The
optional DHCP client adds to that rather than replacing it: `udhcpc.sh` puts
the lease on `eth0:1` and points the default route at the lease's router, and
when the lease ends (or the client is switched off, which releases it) the
alias goes and the fixed gateway returns. The fixed address answers
throughout, so a camera on a network without DHCP, or with an unknown lease,
is still found at 192.168.2.10. DNS servers from the lease are not applied:
`/etc/resolv.conf` points into the vendor's `fconfigs`. Logs are in `/run`
(`camera_app.log`, `hisi-web.log`, `hisi-service.log`; udhcpc logs to the
last).

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
