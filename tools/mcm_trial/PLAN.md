# MCM on the camera: RAM-only trial

**Goal:** find out whether MAVLink Camera Manager (MCM) with WebRTC can run on
the Hi3516DV500 beside camera-app, and what it would cost in flash, RAM and
CPU, before writing an `apcamsrc` element or touching appfs.

**Approach:** MCM's stock ONVIF source pulls camera-app's own RTSP
(`rtsp://192.168.2.10:8554/stream_0`, `/stream_1`) over the loopback route
and serves it again by RTSP and WebRTC. No camera-app, MCM or element changes.
MCM and GStreamer run from a squashfs in `/run` (tmpfs). **Nothing is
flashed; a reboot undoes everything.**

**Stack:** MCM release `t3.26.3` (aarch64, glibc), Debian trixie arm64
GStreamer 1.26 and libraries, bundled with their own glibc 2.41 (the camera's
rootfs has 2.34). Built in Docker on willpc under qemu.

## Constraints

- No flashing, no writes to `/appfs`, `/date` or `/appfs/fconfigs`.
- camera-app keeps running and keeps its ports: RTSP 8554, MAVLink UDP/TCP
  14550, live video 8555, web 80. MCM uses RTSP **8564**, MAVLink
  `udpin:127.0.0.1:14560`, REST 6020, signalling 6021, STUN 3478.
- MCM streams set `disable_mavlink` (camera-app is already the MAVLink camera)
  and `disable_thumbnails` (the lean bundle has no H.264 decoder; without it
  MCM refuses to create the stream).
- The bundle must be mounted at `/run/mcm/root`: the binaries' loader and
  RPATH point there.

## Files (`mcm_trial/`)

| File | Where it runs | What it does |
|---|---|---|
| `build_bundle.sh` | arm64 trixie container | Installs GStreamer, fetches MCM, stages `full` (every base/good/bad/nice plugin) and `lean` bundles, writes `out/mcm-{full,lean,mcm-only}.sqfs` and `out/sizes.txt` |
| `run.sh` | inside each bundle | Sets the library, plugin and registry paths and runs MCM, `gst-launch-1.0` or `gst-inspect-1.0` |
| `verify_bundle.sh` | arm64 alpine container (no glibc) | Every needed element loads; MCM starts and answers REST |
| `camera/deploy_mcm.sh` | camera | Fetches a bundle into `/run` and loop-mounts it |
| `camera/start_mcm.sh` | camera | Starts MCM with the ports above |
| `camera/measure.sh` | camera | MemAvailable, RSS/PSS/threads and CPU of camera-app and MCM, and the libraries MCM mapped |
| `create_streams.sh` | host | Waits for ONVIF discovery, creates the main and sub streams |

The lean plugin set is `coreelements app typefindfunctions rtsp rtp
rtpmanager udp videoparsersbad webrtc nice dtls srtp sctp proxy
video4linux2`, taken from MCM's ONVIF pipeline (`rtspsrc`, depay, parse, pay,
tee), RTSP server (`appsrc`), UDP sink and WebRTC sink (`webrtcbin`, `rtpbin`,
FEC and RED encoders). `video4linux2` is only there for its device provider:
without any provider, MCM panics at startup
(`gst_device_monitor.rs:24`, "Failed to start the GStreamer device monitor").

## Results so far (2026-10-28)

`out/sizes.txt`, same squashfs options as appfs (xz, 64 KiB blocks):

| Bundle | Staged | Squashfs | Plugins | Libraries |
|---|---|---|---|---|
| MCM binary only | 41.8 MB | 13.1 MB | 0 | 0 |
| lean | 93.1 MB | 28.7 MB | 15 | 80 |
| full | 291.6 MB | 89.6 MB | 257 | 336 |

appfs has about 31 MB free, so lean as built uses about 90% of it. Where its
libraries come from (`out/deps-lean.txt`):

- Debian's libnice (used by `nice` and `webrtc`) pulls in gnutls, unistring,
  p11-kit, gupnp-igd, libsoup, sqlite, libxml2, Kerberos, gmp and nettle:
  about 14 MB uncompressed. libnice built with `-Dcrypto-library=openssl
  -Dgupnp=disabled` needs none of them; OpenSSL is already there for `dtls`.
- Debian's libsrtp2 (used by `srtp`) is built with NSS: about 2.2 MB.
  Built with OpenSSL it adds nothing.
- `dtls` brings OpenSSL (`libcrypto` 6.3 MB, `libssl` 1.2 MB), which WebRTC
  needs anyway.
- `video4linux2` brings 1.3 MB (libv4l, gudev, libjpeg, libdrm) for a device
  provider; a tiny MCM patch (don't panic without providers) or a v4l2 plugin
  built without libv4l2 and gudev removes that.
- The MCM binary alone is 13.1 MB of the 28.7 MB.

Libraries compress to about 30% here, so those three rebuilds should save
about 5 MB (lean about 23 MB). Below that needs a smaller MCM (frontend,
zenoh, tracy and swagger UI as Cargo features) or `gstreamer-full`.

**Update:** with libnice 0.1.22 and libsrtp 2.6.0 rebuilt against OpenSSL
(`build_bundle.sh`), lean is 23.6 MB (49 libraries; `lean-debian` keeps the
old 28.7 MB one). `fit_appfs.sh` puts it in `ap_cameragimbal/mcm` of the
e72af0 appfs: 202 of 265 LEBs, 8.0 MB left, hisi-web's check passes. The
Debian libsrtp2 also fails at runtime (NSS dlopens softokn, which ldd misses:
`srtpenc` "Failed to add stream", err 5); the OpenSSL one works.

On the camera (2026-10-28, `bitrate_kbps = 45000` on the main stream, not
the 12 Mbit/s default): MCM idle 54 MB RSS, 5% CPU. As an RTSP relay of
camera-app's stream, 720p 29 fps (MCM 18%), 4K 17 fps (MCM 93%). Dropped:
the relay is not the goal. In-process cost of 4K at 45 Mbit/s from a file
(240 frames, `gst-launch-1.0`, one core = 100%): h264parse 9%, plus
rtph264pay and udpsink 33-39%, plus srtpenc 54%.

Both bundles pass Task 2. Lean is missing only the optional elements left out
on purpose: `videoconvert timeoverlay jpegenc x264enc x265enc avdec_h264
avdec_h265 jpegdec shmsrc shmsink videotestsrc qrtimestampsrc`.

`out/mcm-lean.sqfs` sha256
`24534694956ad0c73fcc84af04be037b493ab57b9f26f596ba9e4b315d1b75ac`.

## Task 1: build the bundles (host, done)

```sh
rsync -a mcm_trial/ will@192.168.15.108:mcm_trial/
ssh will@192.168.15.108
docker run --privileged --rm --network none tonistiigi/binfmt --install arm64
docker run --rm --platform linux/arm64 --network host -v ~/mcm_trial:/work \
    debian:trixie-slim sh /work/build_bundle.sh
```

Docker's bridge network is broken on willpc, hence `--network host`. The
binfmt registration lasts until willpc reboots.

## Task 2: check the bundles have no hidden dependencies (host, done)

```sh
for b in lean full; do
  docker run --rm --platform linux/arm64 --network none \
      -v ~/mcm_trial/out/stage-$b:/run/mcm/root:ro -v ~/mcm_trial:/work:ro \
      alpine:3 sh /work/verify_bundle.sh
done
```

Expected: every element `ok`, MCM answers `/streams` with `[]`, and for lean
the only plugin warnings are for the elements left out on purpose (listed
under Results). Both passed on 2026-10-28.

## Task 3: deploy to RAM on the camera (needs the camera on 192.168.2.x)

Serve `mcm_trial/` from the Mac: `python3 -m http.server 8088` in
`mcm_trial/` (copy `out/` back from willpc first). Then:

```sh
CAM_PASS=<camera root password> tools/camsh.py 'wget -q -O /run/deploy_mcm.sh http://192.168.2.15:8088/camera/deploy_mcm.sh && sh /run/deploy_mcm.sh http://192.168.2.15:8088 lean'
tools/camsh.py '/run/mcm/measure.sh baseline'
```

Expected: `mounted mcm-lean.sqfs`, and MemAvailable falls by about the
squashfs size (the file lives in tmpfs).

## Task 4: start MCM and create the streams

```sh
tools/camsh.py 'setsid nohup /run/mcm/start_mcm.sh >/run/mcm/state/mcm.out 2>&1 </dev/null & sleep 20; tail -20 /run/mcm/state/mcm.out'
./mcm_trial/create_streams.sh 192.168.2.10
tools/camsh.py '/run/mcm/measure.sh idle'
```

Expected: MCM lists `HDIPC - RadCam (IPCamera)` and creates both streams.
If WS-Discovery to itself does not work (multicast loopback), check
`http://192.168.2.10:6020/onvif/devices` and `mcm.out`. That would be the
first thing to fix, or the reason to go straight to `apcamsrc`.

## Task 5: measure

Run `measure.sh LABEL 30` in each state and keep the output:

1. `idle`: MCM running, no clients.
2. `rtsp-main`: `ffplay rtsp://192.168.2.10:8564/cam_main` from the Mac.
3. `webrtc-sub`: one WebRTC client on the 720p stream (MCM's own page at
   `http://192.168.2.10:6020`, in Chrome).
4. `webrtc-main`: one WebRTC client on the 4K stream.
5. `webrtc-2`: two WebRTC clients (main and sub).

For each state also check camera-app still delivers 30 fps to its own RTSP
(`ffprobe -count_frames` over 10 s on `:8554/video1`, expect about 300).

## Task 6: latency

Same stopwatch method as HANDOFF §18: glass to glass through MCM's WebRTC
on 720p and 4K, against camera-app's direct RTSP. Six samples each.

## Task 7: soak

`webrtc-main` for 30 minutes with `measure.sh` every 5 minutes. Pass if MCM's
RSS and thread count level off. Then restart camera-app from the web UI and
check MCM's streams come back by themselves (MCM retries for 300 s by default).

## Task 8: prune and size the lean bundle

Take the union of `/run/mcm/state/maps-*.txt` after Tasks 5 to 7. Libraries
and plugins in `stage-lean` that never appear are candidates to drop. Rebuild
lean with the smaller plugin list, rerun Task 2, and record the new
`out/sizes.txt`. That number is the honest flash cost of the stock-Debian
route. A `gstreamer-full` static build and a trimmed MCM (no embedded
frontend, zenoh, tracy or swagger UI) are the next steps if it is too big.

## Decision

Suggested thresholds, to be agreed:

- **Flash:** the lean squashfs at most about 20 MB, leaving about 10 MB of
  the 31 MB free in appfs.
- **RAM:** MemAvailable stays above 64 MB with two WebRTC clients.
- **CPU:** camera-app plus MCM at most about 140% (of 200%) with one 4K
  WebRTC client, and camera-app's own RTSP stays at 30 fps.
- **Latency:** MCM WebRTC adds at most about 100 ms over direct RTSP.

If all pass, the RTSP-relay route may be enough and `apcamsrc` is only an
optimisation (one RTP depay/pay hop less). If CPU or latency fails, `apcamsrc`
(camera-app socket + GStreamer source + a small `VideoSourceGstType` variant
in MCM, modelled on `QR`) is the next step. If flash or RAM fails, a lean
GStreamer and MCM build comes first, whichever source is used.

## Capture-in-MCM probes (2026-10-28)

Cross-process MPP (`probe/mpp_xproc_probe.c`, run on the camera beside
camera-app, no `sys_init`, no stream packets taken): sys version, VENC 0/1
`get_chn_attr` + `query_status`, ISP `get_pub_attr`, `query_exposure_info`,
`get_ae_stats`, `get_exposure_attr` + `set_exposure_attr` (same value), and
VPSS main `get_chn_frame` + `mmap` + release all succeed. camera-app and its
4K stream were unaffected. The ISP lib's cacheable mmap is refused by ot_mmz
and falls back through `b040_compat`, as in camera-app. VPSS main frames are
compressed (compress mode 1).

musl MCM (`musl/`): t3.26.3 cross-builds for aarch64-unknown-linux-musl
against Alpine 3.22 GStreamer with a two-line patch (`musl/mcm-musl.patch`:
`malloc_trim` gated on glibc, zeroed `sched_param`). Binary 41.5 MB. Bundle
with Alpine's musl loader and the lean plugin set: 23.6 MB squashfs, passes
`verify_bundle.sh` in a glibc-only arm64 container, fits appfs at 202 of 265
LEBs (8.0 MB left). Alpine libnice pulls GnuTLS; the OpenSSL libnice rebuild
would trim it. Not yet run on the camera.

On the camera (RAM only), musl bundle: both musls are 1.2.5; the MPP probe
also passes when run through the bundle's Alpine loader, so MPP libs and
`b040_compat` can load inside musl MCM. Alpine libsrtp works (srtpenc on real
RTP). MCM idle: 31 MB RSS, 0.5% CPU (glibc: 54 MB, 5%). ONVIF self-discovery
finds stream_0/stream_1. WebRTC to a browser from the 720p RTSP source plays
1280x720 at ~30 fps; MCM 16.7% CPU, 45 MB RSS (glibc relay at 720p: 18%).
Camera left as found afterwards.

4K over WebRTC (musl MCM, 12 Mbit/s main stream relayed from camera-app):
first attempt decoded 0 frames, 13-15% packet loss, MCM 137% CPU. Cause:
nicesink sends each frame's RTP list with one sendmmsg; the 212992-byte
default socket send buffer holds ~90 packets, so the tail of every 4K
keyframe was dropped (SndbufErrors rose once per burst, ~84 packets lost
each). The browser never got a complete keyframe and NACKed/PLIed; MCM
advertises RTX but excises rtprtxsend, so every NACK became upstream
retransmission-request events walking the whole pipeline into rtspsrc,
which was the 97%-busy thread (`profile/`: ptrace sampler + symbolizer;
memcpy ~0.1-3% on every hot thread, no needless copies found). With
net.core.wmem_default/wmem_max = 4 MiB (RAM-only test, reverted): 3840x2160
at 30.00 fps, 0 lost, 0 NACK/PLI over 30 s, MCM 51% + camera-app 23% CPU,
7 ms jitter buffer; 2 freezes (~200 ms each) per 20 s still unexplained.

Serving retransmissions (`musl/mcm-rtx.patch`: keep rtprtxsend, history
500 ms by time instead of 100 packets). Default 208 KB send buffer: 4K now
plays at 30.00 fps (was 0), 3449 lost / 3546 retransmitted in 20 s, 21
NACKs, 0 PLI, MCM 55% CPU (was 137%), but 19 freezes / 4.2 s (one per
keyframe, the NACK round trip). 4 MiB send buffer: 0 lost, 0 retransmitted,
30.00 fps, MCM 49%. Remaining ~2 freezes per 20 s come from the relay:
camera-app's own RTSP (`profile/rtsp_timing.py`) has constant 33 ms RTP
gaps and <=103 ms arrival gaps at keyframes; through MCM keyframes arrive
~190 ms after the previous frame (Chrome's freeze threshold ~183 ms)
because MCM's udpsrc thread runs rtspsrc, two depay+parse branches, pay and
tee per packet (~0.35 ms each). A native capture element removes this hop.
