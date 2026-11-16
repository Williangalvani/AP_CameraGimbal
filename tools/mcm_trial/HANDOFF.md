# `mcm-trial` branch: MAVLink Camera Manager on the Hi3516DV500 — HANDOFF

Status date: **2026-11-16**. Read this first, then `PLAN.md` (same folder),
which holds the method, every task and the raw results.

## What this branch is

`mcm-trial` (pushed to `fork`, Williangalvani/AP_CameraGimbal; never push it
to `origin`, and don't rebase it on ArduPilot) is
`4kcam-sdcard` (`9cbaade`) plus:

- this folder, `tools/mcm_trial/` (`c468dee`, `38fe3ae`, `da7872d`);
- the hisi backend's **frame socket** (`5e9f113`,
  `camera_app/include/camera_app/frame_socket.h`), which `apcamsrc` reads;
- unrelated commits that ride along: `VIDEO_MAIN_FPS` in the camera
  definition (`617c6e8`) and the MAVLink image-control checks (`7658ed5`,
  `4d6d015`).

No hisi-web or build change is on this branch, and MCM is not part of an
appfs image. The flashed camera-app has no frame socket; apcamsrc needs a
camera-app built from this branch.

"MCM" here means **MAVLink Camera Manager** (the BlueOS service), not the
vendor's **4K Cam Manager**. The 4K Cam Manager compatibility work is already
in `hisi-next`, `4kcam-image-controls` and `4kcam-sdcard`; with it, an MCM
running on BlueOS discovers the camera by ONVIF and relays its RTSP (HANDOFF
§20 in the workspace). This trial asks the next question: **can MCM run on
the camera itself**, beside camera-app, and serve WebRTC directly?

Everything in the trial ran from RAM (`/run`, tmpfs). Nothing was flashed,
and the camera was left as found after each session.

## Where things stand

- **MCM runs on the camera.** MCM `t3.26.3` with GStreamer 1.26 runs from a
  squashfs mounted at `/run/mcm/root`, beside camera-app, with
  `disable_mavlink` (camera-app stays the MAVLink camera) and its own ports
  (RTSP 8564, MAVLink `udpin:127.0.0.1:14560`, REST 6020, signalling 6021,
  STUN 3478).
- **musl is the build to keep.** Idle: 31 MB RSS, 0.5% CPU, against 54 MB and
  5% for the glibc (Debian) build. It needs `musl/mcm-musl.patch` (two lines).
- **Flash cost:** the lean bundle is 23.6 MB as squashfs (MCM binary alone
  13.1 MB). It fits appfs with 8 MB left (`fit_appfs.sh`, 202 of 265 LEBs),
  above the ~20 MB target in PLAN.md's Decision section.
- **720p WebRTC works:** ~30 fps in Chrome, MCM ~17% CPU.
- **4K WebRTC works with caveats.** Relaying camera-app's 4K RTSP:
  - Default 208 KB socket send buffer: each 4K keyframe overflows it. With
    `musl/mcm-rtx.patch` (keep `rtprtxsend`, 500 ms history) it plays at
    30 fps, MCM 55%, but freezes once per keyframe.
  - `net.core.wmem_default/wmem_max` = 4 MiB: 30 fps, no loss, MCM 49% +
    camera-app 23%, still ~2 freezes (~200 ms) per 20 s.
  - Those freezes come from the relay hop: keyframes reach the browser
    ~190 ms after the previous frame, past Chrome's ~183 ms freeze threshold.
- **apcamsrc works and is the route to keep** (PLAN.md "apcamsrc"). MCM reads
  camera-app's encoded frames from the frame socket instead of relaying its
  RTSP. 4K WebRTC: 30 fps, **0 freezes in 3 minutes**, camera-app 17% + MCM
  17% CPU (relay: 23% + 49%), first frame 0.8 s after the session request,
  with the default socket send buffer. It needed, beyond the element:
  `musl/mcm-apcam.patch` (the `ApCam` source, a 4 MiB send buffer on the ICE
  socket, a faster readiness check), frames sent to the socket before RTSP
  in camera-app, and MCM started with `--disable-onvif` (its ONVIF discovery
  probes camera-app's RTSP every ~26 s, which delays keyframes).
- **An MPP source in MCM's process is possible but was not needed.**
  `probe/mpp_xproc_probe.c` read and set VENC, ISP and VPSS state from a
  second process beside camera-app without disturbing its stream. apcamsrc
  takes camera-app's socket instead, which keeps camera-app the only owner
  of the encoders.

- **Keyframe on request works** (`camera-app-keyframe.patch`, tested
  2026-11-15 on `9cbaade` + the patch, from RAM with
  `camera/swap_camera_app.sh`). camera-app's RTSP server used to ignore RTCP
  PLI/FIR, so MCM's keyframe requests went nowhere. The patch adds
  `ca_rtsp_set_keyframe_callback` and makes the hisi backend call
  `ss_mpi_venc_request_idr` when a client starts playing or sends PLI/FIR
  (at most every 500 ms per stream). Measured with
  `profile/keyframe_check.py` (H.264, GOP 30 at 30 fps):

  | | flashed camera-app | with the patch |
  |---|---|---|
  | PLAY to first IDR, main | 36–932 ms | 13–93 ms |
  | PLI/FIR to IDR, main, TCP and UDP | 437–705 ms (next GOP) | 61–76 ms (32 of 32) |
  | PLI to IDR, sub | — | 39–41 ms |

  Requests closer than 500 ms are held to the next GOP, as intended.
  camera_app, web and hisi host tests pass with the patch applied.

## Not finished

- **The relay with the keyframe patch** was not measured; apcamsrc made it
  moot (the frame socket answers key frame requests itself). The patch still
  helps RTSP viewers of camera-app.
- **MCM's own patches are not upstream**: `mcm-apcam.patch` mixes the
  apcam source with two general fixes (the ICE send buffer and the
  readiness check) that BlueOS could take on their own.
- **Intra refresh is not an alternative**: with it enabled on VENC 0
  (`probe/venc_tune.c`), the main stream stopped producing frames (camera-app
  waits for an IDR before releasing a new RTSP client). The encoder was put
  back to GOP 30, QP delta 2, refresh off.
- PLAN.md Tasks 6 to 8 (glass-to-glass latency, 30-minute soak, pruning the
  bundle from the libraries actually mapped) were not run.

## Running MCM with apcamsrc

Last done from **willpc** (Linux, 192.168.2.100 on the camera's subnet;
camera at 192.168.2.10). On willpc, Docker must run with `--network host`
(its bridge network is broken), and the arm64 Alpine image replaces the
amd64 one: `docker pull -q --platform linux/amd64 alpine:3.22` after an
arm64 build. The build copy is `~/mcm_trial` (same layout as this folder;
`rsync` this repo's `tools/mcm_trial/` into it). MCM's source is
`~/mcm_trial/mcm-src`, a checkout of `t3.26.3` with `musl/mcm-musl.patch`,
`musl/mcm-rtx.patch` and `musl/mcm-apcam.patch` applied, uncommitted, in
that order. After changing MCM, regenerate `mcm-apcam.patch` as the diff
from `t3.26.3` with the first two patches applied (a scratch
`git worktree add` of `t3.26.3` works; remove it afterwards) and copy it
back here: only the patch is in git.

Test apcamsrc in this repository (it builds camera-app's `frame_socket.cpp`
into a host feeder; needs GStreamer dev files, ffmpeg and PyGObject):
`make -C tools/mcm_trial/apcamsrc test`, 15 checks.

Build, from `~/mcm_trial` (MCM takes ~3.5 min on 24 cores):

```sh
(cd apcamsrc && docker run --rm --platform linux/arm64 --network host \
    -v $PWD:/work -w /work alpine:3.22 sh build_musl.sh)
docker run --rm --network host -v $PWD:/work -w /work debian:trixie-slim \
    sh musl/build_musl_mcm.sh build
docker run --rm --network host -v $PWD:/work -w /work alpine:3.22 \
    sh musl/stage_musl_bundle.sh    # copies libgstapcam.so into out/mcm-musl.sqfs
```

camera-app, from the repository root:
`make -C camera_app -j24 hisi HISI_DEVICE_CODE=19/2/35`.

Serve both trees to the camera:

```sh
python3 -m http.server 8088 --bind 192.168.2.100 -d ~/mcm_trial &
python3 -m http.server 8089 --bind 192.168.2.100 &   # from the repository root
```

Drive the camera with `tools/hisi_shell.py -t SECONDS 'command' ...` (telnet,
from the repository root). Everything below lives in `/run` (RAM):

```sh
# camera-app with the frame socket, bind-mounted over the flashed one
wget -q -O /run/swap_camera_app.sh http://192.168.2.100:8089/tools/mcm_trial/camera/swap_camera_app.sh
chmod +x /run/swap_camera_app.sh
/run/swap_camera_app.sh http://192.168.2.100:8089/camera_app/build/hisi/camera-app
# its log (/run/camera_app.log) then says "frame socket ready on @apcam/video"

# MCM bundle and start; --reset in start_mcm.sh drops saved streams
wget -q -O /run/deploy_mcm.sh http://192.168.2.100:8088/camera/deploy_mcm.sh
sh /run/deploy_mcm.sh http://192.168.2.100:8088 musl
setsid nohup /run/mcm/start_mcm.sh --disable-onvif >/run/mcm/state/mcm.out 2>&1 </dev/null &
```

Then from the host `sh create_streams.sh 192.168.2.10 apcam` (streams
"4K Cam main (on camera)" and "4K Cam sub (on camera)"), and play `cam_main`
from `http://192.168.2.10:6020/webrtc` or `rtsp://192.168.2.10:8564/cam_main`.
Measure with:

- `probe/webrtc_stats.py --stream '4K Cam main (on camera)' --seconds 180`
  (needs Chrome and a Python with `websockets`; on willpc
  `/tmp/cdpvenv/bin/python3`, which `/tmp` may have lost);
- `sudo python3 profile/rtp_gaps.py --seconds 60 --threshold 100` meanwhile
  (keyframe arrival gaps on `br0`);
- `/run/mcm/measure.sh LABEL SECONDS` on the camera (CPU and RSS).

Put the camera back when done:

```sh
kill $(pidof mavlink-camera-manager); sleep 2
umount /run/mcm/root; rm -rf /run/mcm
/run/swap_camera_app.sh restore
rm -rf /run/ca-test /run/swap_camera_app.sh /run/deploy_mcm.sh
md5sum /appfs/ap_cameragimbal/bin/camera-app   # flashed: ef558a5b...
```

The camera was left like that on 2026-11-16: flashed camera-app running, no
MCM, `net.core.wmem_*` at the default 212992. The user's image settings must
survive any test: white balance manual, red 337, green 256, blue 256,
orientation auto (`/date/ap_cameragimbal/camera.ini`).

## Files

| Path | What it is |
|---|---|
| `PLAN.md` | Goal, constraints, tasks 1–8, all measurements |
| `build_bundle.sh`, `run.sh`, `verify_bundle.sh` | glibc (Debian) bundles and their check |
| `musl/` | musl MCM build, bundle staging, `mcm-musl.patch`, `mcm-rtx.patch`, `mcm-apcam.patch` |
| `apcamsrc/` | GStreamer source for camera-app's frame socket, its tests and musl build |
| `fit_appfs.sh` | Test-only appfs image with the bundle, for sizing; not for flashing |
| `camera/` | On-camera scripts: deploy, start MCM, measure, swap in a RAM camera-app |
| `create_streams.sh` | Creates MCM's main and sub streams over REST |
| `probe/` | MPP cross-process probe, `venc_tune`, `tsample` (ptrace sampler), `webrtc_stats.py` (headless Chrome WebRTC stats) |
| `profile/` | Sampler symbolizer, RTSP timing, keyframe-request and RTP arrival-gap scripts, hot-thread samples |
| `camera-app-keyframe.patch` | Keyframe on PLAY/PLI/FIR, tested (above) |
| `out/sizes.txt`, `out/deps-*.txt`, `out/build.log` | Bundle sizes and library lists |
| `out/kfserve/` | RAM bring-up scripts for testing the keyframe camera-app |

`camera/` scripts take the HTTP server as an argument (above). The older
`out/kfserve/` scripts call `tools/camsh.py` from the Mac's workspace and
expect the Mac at `192.168.2.15:8088`; `tools/hisi_shell.py` in this
repository does the same job. The camera's root password was replaced with
a placeholder in PLAN.md.

## Left out of git

Binaries are not committed. The apcam builds are in `~/mcm_trial/` on willpc
(and `camera_app/build/hisi/` in this worktree); the earlier ones are in
`4kcamera_works/mcm_trial/` on the Mac. Rebuild them with the scripts here if
those copies are gone.

| File | Size | SHA-256 |
|---|---|---|
| `out/mcm-lean.sqfs` (glibc, OpenSSL libnice/libsrtp) | 23552000 | `27774f15760f9c7f4c99e18c125cd4018bdacf9da944d415f40e44ab57ea301f` |
| `out/mcm-musl.sqfs` (with apcam, on willpc) | 23629824 | `ed5aba4a15b96b133680c6f8cbe0d61a9e29230a8c2a7c4a1ef92249608bb392` |
| `apcamsrc/build/musl/libgstapcam.so` | 67320 | `255ba0d1b3e5f3346bc1f6351b8aa9c6634af558e6a8aac169497477718ebdac` |
| `camera_app/build/hisi/camera-app` (`5e9f113`, frame socket) | 1930536 | `029e79792f2ba56a274b269c01708ec00441c7cac43afebbc9d26c6bc01f3308` |
| `out/cam4k_8s.h264` (4K test clip) | 46509296 | `c1a8d6140f8900e9ec87b055552d81346de064745aed0702bd79262022864317` |
| `out/kfserve/ours.sqfs` (`7cc9b93` appfs, reassembled) | 1982464 | `7a4141eabbaec756d40d8e7739e7932b4c3d214768f281dd7a9a5cdfe4797ce2` |
| `out/kfserve/bin/camera-app` (keyframe patch) | — | `80cc659fe42e38a4e2427c886b2361c3f5e3a90f8f7db0cdf16647a3953a7fd7` |
| `probe/build/` | — | aarch64 musl builds of the probes |

The `mcm-lean.sqfs` SHA-256 in PLAN.md (`2453469…`) is the earlier Debian
build (now `lean-debian`); the file above is the OpenSSL rebuild.

## Next steps

1. Run PLAN.md Tasks 6–8 (latency, 30-minute soak, pruning) on apcamsrc and
   apply its Decision thresholds.
2. Decide how MCM ships: from appfs (flash budget above), with
   `--disable-onvif` and the apcam streams created at boot.
3. Offer the ICE send-buffer and readiness fixes to BlueOS's MCM separately
   from the apcam source.

Check who else is using the camera before taking over its video pipeline.
