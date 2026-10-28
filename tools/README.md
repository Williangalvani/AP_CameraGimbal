# Tools

- `build_camera_definitions.py` exports each target's built-in camera definition
  XML using the host compiler. `make camera-definitions` writes these to
  `build/camera-definitions/`; the firmware serves the same XML over MAVFTP.

- `install_build_environment.py` installs x86_64 Debian/Ubuntu hardware-build
  prerequisites and fetches pinned A8/ZR10/Z1-Mini compilers and SDKs. It creates
  an isolated Python environment and `build/environment.mk` for top-level Make.
  Use `--skip-system` to leave system packages alone, or `--targets` to select
  cameras. It never downloads camera firmware bundles.
- `bootstrap_dependencies.sh` downloads and verifies the pinned public SS928
  MPP sample tree and minimp4 header used by target and SITL builds.
- `prepare_xop.py` applies `xop_warnings.patch` to a private RTSP source copy,
  fixing buffer ownership, TCP_NODELAY, the TCP send queue size and compiler
  warnings. Its default mode
  also prepares Cygwin/macOS sockets; `--native` retains Linux epoll. Unchanged
  files retain their timestamps. Compiler warning checks remain enabled.
- `prebuilt_mt11_tools.py` verifies and unpacks the stripped, compressed MT11
  support tools from `packaging/mt11/tools/`. Normal release builds use these
  binaries without downloading or compiling their sources.
- `build_mt11_tools.sh` is the maintainer rebuild recipe: it downloads, verifies
  and cross-compiles static AArch64
  `rsync`, `strace`, `ltrace`, `tcpdump`, Dropbear and `dropbearkey` binaries for
  `/app/bin` in MT11 packages. See [refresh instructions](../packaging/mt11/tools/README.md).
- `build_mt11_package.sh` creates an MT11 application update from the supported
  in-repository kernel, rootfs and update descriptor. It is normally invoked
  through `make mt11_package`; web and root passwords default to `ardupilot`
  and can be overridden.
- `repack-root-password.sh` rebuilds the MT11 root filesystem with a supplied
  SHA-512 crypt verifier, `MT11_ROOT_PASSWORD`, or an interactively entered
  password. The package builder invokes it with the configured project value.
- `build_a8_package.sh` creates the SIYI A8 mini SD update (`make a8_package`);
  its first argument is the checked-in platform directory. See `packaging/a8/README.md`.
- `build_zr10_firmware.sh` combines the checked-in ZR10 platform directory with
  the AP application archive, UUID helper and startup scripts to make an SD image.
- `video_telemetry.py INPUT --output telemetry.jsonl` extracts per-frame
  position/attitude SEI metadata from H.264/H.265 files, MP4 recordings or RTSP
  streams. Container/RTSP inputs require FFmpeg; see the
  [schema and timing notes](../camera_app/README.md#video-telemetry-metadata).
- `temp_to_png.py` converts an MT11 640x512 little-endian radiometric `_I.bin`
  capture to a viewable PNG.

These scripts do not download a vendor firmware bundle. Reviewed immutable
MT11 platform images used by the package builder are under
`packaging/mt11/base`.

Native GDB is intentionally not part of the MT11 overlay. A stripped static
GDB 17.2 build occupied 10.2 MiB (4.1 MiB compressed), which is too costly on
the camera's 140 MiB `/app` volume.

- `build_thermal_codecs.sh host|aarch64` builds pinned minimal FFmpeg libraries
  for the MT11 FFV1 stream and the codec benchmark. Invoked by the camera Makefile.
- `raw_thermal_probe.py URI --mavproxy PATH --output DIRECTORY` captures native
  16-bit FFV1 frames and paired JSON metadata from MT11 or SITL. See the
  [raw thermal testing guide](../camera_app/RAW_THERMAL.md).

## Legacy thermal directories to lossless video

`thermal_to_video.py` converts a flat directory of 640x512 little-endian uint16
`.bin` frames to the same FFV1 level-3 Matroska profile used by the MT11 raw
stream. It uses range coding, slice CRCs and independent frames; every sensor
bit is preserved. Install `numpy` and `av>=18.1` in the Python environment.

```sh
python3 tools/thermal_to_video.py /path/to/capture /path/to/capture.mkv
python3 tools/thermal_to_video.py --extract /path/to/capture.mkv /path/to/extracted
```

Both commands print counts and encoding reports the complete container size and
compression ratio. Extraction restores original filenames and file modification
times and verifies each decoded frame against its stored SHA-256. Output files
and directories must not already exist. Results are published only after success;
invalid input, checksum errors and incomplete archives are rejected.

By default, playback timing comes from the camera filenames
`YYYY-MM-DD_HH-MM-SS_MILLISECONDS_I.bin`, sorted by parsed time (so `_13` is
13 milliseconds, before `_100`). If any filename lacks that format, file
modification times are used for ordering and timing instead. `--fps 5` overrides
playback timing with a fixed 5 Hz cadence while retaining original names and
modification times. This does not resample or discard any frames. Gaps in the
original capture are otherwise retained.

The per-frame `apcg.thermal.v1` metadata includes temperature range, original
filename, file modification time in nanoseconds, source SHA-256 and archive frame
count. `capture_monotonic_us` is reconstructed elapsed time plus one microsecond,
marked `capture_clock=reconstructed_relative` and `timestamp_source=legacy_*`;
it is not a recovered hardware clock. Filename timezone is not assumed. Old raw
files have no vehicle/gimbal pose, so `telemetry` is null unless a dataflash log
is supplied. Display rotation is 180 degrees by default (upright MT11);
`--rotation 0` changes this metadata only.

`--bin FLIGHT.bin` reconstructs each frame's `apcg.telemetry.v1` snapshot from an
ArduPilot dataflash log. GPS week/ms give the log absolute UTC, so every record
maps to the frames' absolute filename (or modification) time. Position (`POS`),
NED velocity (`XKF1` lane 0), vehicle attitude and yaw rate (`ATT`, `RATE`) and
gimbal attitude (`MNT`) are linearly interpolated between the bracketing samples,
with yaw and heading following the shortest arc; each field's `age_ms` is the
distance to the nearest real sample. A frame outside the log's coverage keeps its
reconstructed UTC clock with a null pose. Gimbal backends fill `MNT` differently:
the converter prefers each axis' reported angle, falls back to the demanded angle
(so a held pitch command is used when the actual is not logged), and prefers
vehicle-relative yaw, converting earth-referenced yaw with the vehicle yaw. The
chosen source per axis is recorded in each frame's `gimbal_pose_source`. This
needs `pymavlink`. Requires filenames or modification times that already sit on
the same absolute UTC clock as the log; it does not resample frames.

`--extract --metadata` additionally writes a matching `.json` for each `.bin`.
Extraction also accepts recordings of the new camera stream; without original
filenames, files are named by frame ID. Camera-stream files lack the archive's
source checksums and expected frame count; FFV1 decoding/CRC and range checks
still apply. The converter handles raw pixels and file timestamps, and does not
import any external telemetry sidecar files.

Run the converter regressions with `python3 tests/test_thermal_to_video.py`.
