# `mcm-trial` branch: MAVLink Camera Manager on the Hi3516DV500 — HANDOFF

Status date: **2026-11-15**. Read this first, then `PLAN.md` (same folder),
which holds the method, every task and the raw results.

## What this branch is

`mcm-trial` is `4kcam-sdcard` (`9cbaade`) plus this folder, `tools/mcm_trial/`,
and one camera-app change: the hisi backend's **frame socket**
(`camera_app/include/camera_app/frame_socket.h`), which `apcamsrc` reads.
No hisi-web or build change is on this branch, and MCM is not part of an
appfs image.

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
- **A native capture element is feasible.** `probe/mpp_xproc_probe.c` read and
  set VENC, ISP and VPSS state from a second process beside camera-app
  without disturbing its stream, also through the musl bundle's loader. That
  is the basis for `apcamsrc` (camera-app socket or MPP source + a small
  `VideoSourceGstType` variant in MCM).

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

On the host, from `tools/mcm_trial/` (the musl build expects MCM `t3.26.3`
in `mcm-src/` with `musl/mcm-musl.patch`, `musl/mcm-rtx.patch` and
`musl/mcm-apcam.patch` applied, in that order):

```sh
(cd apcamsrc && make test)    # host build and 15 checks against the real frame socket
(cd apcamsrc && docker run --rm --platform linux/arm64 --network host \
    -v $PWD:/work -w /work alpine:3.22 sh build_musl.sh)
docker run --rm --network host -v $PWD:/work -w /work debian:trixie-slim \
    sh musl/build_musl_mcm.sh build
docker run --rm --network host -v $PWD:/work -w /work alpine:3.22 \
    sh musl/stage_musl_bundle.sh    # copies libgstapcam.so into the bundle
```

The camera needs a camera-app with the frame socket (its log says
`frame socket ready on @apcam/video`). On the camera:

```sh
sh /run/deploy_mcm.sh http://HOST:8088 musl
setsid nohup /run/mcm/start_mcm.sh --disable-onvif >/run/mcm/state/mcm.out 2>&1 </dev/null &
```

Then from the host `sh create_streams.sh CAMERA_IP apcam`, and play
`cam_main` from MCM's `/webrtc` page or `rtsp://CAMERA_IP:8564/cam_main`.
`probe/webrtc_stats.py --stream '4K Cam main (on camera)'` measures a
session; `sudo profile/rtp_gaps.py` shows keyframe arrival gaps meanwhile.

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

`camera/` and `out/kfserve/` scripts call `tools/camsh.py` and expect files
served from the Mac at `192.168.2.15:8088`; `camsh.py` lives in the
workspace (`4kcamera_works/tools/`), not in this repository. The camera's
root password was replaced with a placeholder in PLAN.md.

## Left out of git

Binaries are not committed. They are in `4kcamera_works/mcm_trial/` on the
Mac; rebuild them with the scripts here if that copy is gone.

| File | Size | SHA-256 |
|---|---|---|
| `out/mcm-lean.sqfs` (glibc, OpenSSL libnice/libsrtp) | 23552000 | `27774f15760f9c7f4c99e18c125cd4018bdacf9da944d415f40e44ab57ea301f` |
| `out/mcm-musl.sqfs` (with apcam, on willpc) | 23629824 | `ed5aba4a15b96b133680c6f8cbe0d61a9e29230a8c2a7c4a1ef92249608bb392` |
| `apcamsrc/build/musl/libgstapcam.so` | 67320 | `255ba0d1b3e5f3346bc1f6351b8aa9c6634af558e6a8aac169497477718ebdac` |
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
