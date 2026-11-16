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
Results on apcamsrc: "Tasks 6–8 on apcamsrc" below (glass to glass not
measured; per-frame path latency instead).

## Task 7: soak

`webrtc-main` for 30 minutes with `measure.sh` every 5 minutes. Pass if MCM's
RSS and thread count level off. Then restart camera-app from the web UI and
check MCM's streams come back by themselves (MCM retries for 300 s by default).
Done 2026-11-16 on apcamsrc: passed (below).

## Task 8: prune and size the lean bundle

Take the union of `/run/mcm/state/maps-*.txt` after Tasks 5 to 7. Libraries
and plugins in `stage-lean` that never appear are candidates to drop. Rebuild
lean with the smaller plugin list, rerun Task 2, and record the new
`out/sizes.txt`. That number is the honest flash cost of the stock-Debian
route. A `gstreamer-full` static build and a trimmed MCM (no embedded
frontend, zenoh, tracy or swagger UI) are the next steps if it is too big.
Done 2026-11-16 for the musl apcam bundle: 23.3 MB (below).

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

## apcamsrc (2026-11-15)

camera-app publishes each encoded access unit on the abstract Unix socket
`@apcam/video` (`camera_app/include/camera_app/frame_socket.h`); `apcamsrc`
(`apcamsrc/`, a GstPushSrc) reads it, and MCM's `ApCam` source type
(`musl/mcm-apcam.patch`) builds `apcamsrc ! h264parse ! tee ! rtph264pay !
tee`. No RTSP, no depayloading. Sources are `apcam:main` and `apcam:sub`;
MCM lists them when the plugin is present and reads their format from the
socket. A GstForceKeyUnit event (MCM sends one on PLI and when ICE connects)
becomes an IDR request.

Measured with `probe/webrtc_stats.py` (headless Chrome on MCM's own /webrtc
page, `getStats` every second) and `profile/rtp_gaps.py` (frame arrival gaps
sniffed on the host; SRTP leaves RTP headers in clear). 4K main stream,
12 Mbit/s, GOP 30; the camera's link is 100 Mbit/s.

| Setup | Freezes | Lost (all retransmitted) | CPU camera-app + MCM |
|---|---|---|---|
| Relay, 4 MiB `wmem` (above) | ~2 / 20 s | 0 | 23% + 49% |
| apcamsrc, default 208 KB `wmem` | 37 / 40 s | 12026 / 40 s | 17% + 30% |
| apcamsrc, 4 MiB `wmem` | 2–5 / 90 s | 2–14 | 17% + 21% |
| + frame socket before RTSP, `--enable-realtime-threads` | 1 / 180 s | 21 | — |
| + `--disable-onvif`, default priorities | 0 / 180 s | 4 | 17% + 17% |
| + ICE socket `SO_SNDBUFFORCE`, default `wmem` (final) | 0 / 180 s | 3 | 16.7% + 17.3% |

All runs played 3840x2160 at 30.0 fps. Final run: keyframe arrival gaps p99
129 ms, max 162 ms; MCM 43 MB RSS, camera-app 6 MB.

What each step found:

- **Keyframes are the whole problem.** A 4K IDR is ~600 KB, ~450 packets:
  at least 50 ms on a 100 Mbit/s wire, and it arrives ~130 ms after the
  previous frame, against Chrome's ~183 ms freeze threshold. Anything that
  delays a keyframe by ~50 ms freezes the picture.
- **Default send buffer:** as with the relay, each keyframe overflows the
  208 KB socket buffer. MCM now sets 4 MiB with `SO_SNDBUFFORCE` on the
  selected ICE socket (`enlarge_ice_send_buffer` in `webrtc_sink.rs`; the
  kernel reports 8 MiB), so no sysctl is needed. MCM runs as root here.
- **camera-app ordering:** per 4K keyframe, `get_stream` plus the copy out of
  VENC takes 10.7 ms and `ca_rtsp_push_video` 15.5 ms even with no RTSP
  viewer (SEI insertion copies the frame, then xop's locks), up to 42 ms
  when the RTSP server thread holds them. The frame socket now publishes
  first (0.8 ms). Measured with temporary timers in `consume_frame`.
- **MCM's ONVIF discovery** runs every ~26.5 s and opens RTSP sessions to
  camera-app on both camera addresses (and to other cameras on the LAN).
  On two cores that delayed a keyframe by ~40 ms every 27 s. With apcam
  sources ONVIF is not needed: run MCM with `--disable-onvif`.
- **MCM thread priorities:** without `--enable-realtime-threads`, MCM puts
  every pipeline streaming thread at nice 19 (`lower_to_background_priority`)
  although `--help` says "normal scheduling". Realtime threads hid the ONVIF
  bursts; with ONVIF off the defaults are enough.
- **Session start:** MCM waits for the pipeline position to advance before
  adding a WebRTC session, else 3 s. The bin asks only its sinks, which see
  no data on an idle stream; `apcamsrc` now answers POSITION and MCM asks
  the sources too. First frame 0.82 s after the session request (was 3.7 s),
  0.66 s when the stream already runs.
- **camera-app restart:** the WebRTC session resumes on its own after the
  ~3 s restart; `apcamsrc` reconnects every 500 ms and starts on a key frame.
- MCM RTSP (`rtsp://CAMERA:8564/cam_main`, TCP and UDP) and the 720p sub
  stream over WebRTC (30 fps, 0 freezes, 2.1 Mbit/s) work from apcam too.
- The few lost packets are not dropped on the camera (`Udp6SndbufErrors`
  and the NIC counters do not move); retransmission recovers them.

## Tasks 6–8 on apcamsrc (2026-11-16)

Run from willpc, everything in RAM, nothing flashed. camera-app was the
flashed build `cb71401` (`4kcam-sdcard`: CORS, vendor WB stepper, keyframe on
PLAY/PLI/FIR) with `mcm-trial` merged on a local scratch branch for the frame
socket (merged cleanly; not pushed), swapped in with
`camera/swap_camera_app.sh`. MCM: `out/mcm-musl.sqfs` (sha256 `ed5aba4a…`,
apcam), `--disable-onvif`, streams from `create_streams.sh … apcam`.
`net.core.wmem_*` stayed at the default 212992 throughout. Raw logs:
`out/t678/`.

### Task 6: latency

**Glass to glass was not measured.** The camera faces a desk (no screen in
view) and nobody could point it at one, so the stopwatch method was not
possible. Measured instead, frame by frame, with `profile/path_latency.py`
(sniffs camera-app's direct RTSP over UDP and MCM's WebRTC at the same time
on `br0`, pairs frames by their size sequence; 1200 of 1200 paired, RTP
deltas 100% consistent) and Chrome's `getStats` (`probe/webrtc_stats.py`,
`processing_ms` = first packet received to frame decoded, includes the
jitter buffer):

| 40 s, WebRTC minus direct RTSP arrival | 4K main | 720p sub |
|---|---|---|
| ordinary frames, last packet, median (p90 / p99) | +2.5 ms (3.3 / 6.9) | +0.8 ms (1.2 / 1.8) |
| keyframes, last packet, median (p90 / max) | −31.8 ms (−22.5 / 58.6) | +9.2 ms (9.7 / 12.3) |
| Chrome receive to decoded (`processing_ms`) | 15.7–17.4 ms | 2.1–2.4 ms |
| Chrome jitter buffer / decode | 8.8–10.5 / 6.9 ms | 1.1–1.4 / 0.9 ms |

The WebRTC path reaches the host as fast as camera-app's own RTSP: the
frame socket is written before RTSP, so 4K keyframes even arrive ~30 ms
earlier through MCM. MCM adds no measurable network-side delay; on the
receiver Chrome adds ~17 ms at 4K (a direct-RTSP player adds its own
buffer, not measured). Encoder and sensor latency are common to both paths
and were not measured. With an extra 4K RTSP client on camera-app at the
same time (this test) the WebRTC view froze 4 times in 60 s; alone it does
not (Task 7).

### Task 7: soak

30.7 min of 4K WebRTC (one headless Chrome), `measure.sh` 30 s every 5 min:

| min | MemAvailable | CPU camera-app + MCM | RSS camera-app / MCM | MCM threads |
|---|---|---|---|---|
| 0 | 254.5 MB | 16.9% + 17.6% | 7.22 / 44.9 MB | 30 |
| 5 | 253.8 MB | 17.2% + 17.3% | 7.23 / 45.6 MB | 30 |
| 10 | 254.6 MB | 16.8% + 17.1% | 7.26 / 44.9 MB | 30 |
| 15 | 254.3 MB | 17.0% + 17.6% | 7.26 / 44.8 MB | 30 |
| 20 | 253.4 MB | 17.3% + 17.2% | 7.28 / 45.7 MB | 30 |
| 25 | 253.8 MB | 17.0% + 17.6% | 7.31 / 45.0 MB | 30 |
| 30 (session ending) | 255.5 MB | 16.8% + 15.4% | 7.36 / 43.7 MB | 28 |

Browser over 1839 s: 3840x2160 at 29.99 fps, **2 freezes (0.37 s total)**,
2 packets lost (both retransmitted), 2 NACK, 0 PLI, 0 frames dropped,
12.2 Mbit/s, jitter buffer 8.8 ms. Camera `SndbufErrors` and
`Udp6SndbufErrors` stayed 0. **Pass:** MCM RSS stays 44.8–45.7 MB and 30
threads (VmHWM 47.3 MB from the first sample on); camera-app RSS grew
136 kB in 30 min (7.22 → 7.36 MB), worth watching in a longer run.

camera-app restart mid-session (the service restart request): new
camera-app ready in 5 s, the same WebRTC session went black 2.7 s (one
freeze) and resumed at 30 fps on its own; MCM's streams were untouched.

Other states (MCM restarted with its registry cached, see Task 8):

| State | MemAvailable | CPU camera-app + MCM | MCM RSS | Result |
|---|---|---|---|---|
| idle | 261.2 MB | 15.0% + 0.2% | 25.2 MB | — |
| webrtc-main (+ ffprobe on :8554) | 254.8 MB | 21.3% + 18.0% | 44.0 MB | 29.99 fps, 3 freezes / 60 s |
| webrtc-sub | 255.7 MB | 16.2% + 8.2% | 42.7 MB | 30.0 fps, 0 freezes |
| webrtc-2 (main + sub, + ffprobe) | 252.5 MB | 21.8% + 25.8% | 45.6 MB | 4K 30.1 fps 1 freeze, 720p 30.0 fps 0 |
| MCM RTSP main UDP + sub TCP | 254.7 MB | 18.2% + 20.1% | 43.6 MB | both ~30 fps |

camera-app's own RTSP (`ffprobe -count_frames`, 10 s on `stream_0`) gave 301
frames with webrtc-main and with webrtc-2. The extra 4K RTSP reader on
camera-app is what costs the freezes in those rows (as in Task 6).

### Task 8: prune and size

With `GST_REGISTRY_FORK=no` (run.sh) the first start scans the registry in
MCM's own process and maps every plugin, so `maps-*` from that run lists
everything. With the registry cached (MCM restarted) the union of
`maps-{idle,webrtc-main,webrtc-sub,webrtc-2,rtsp-main}` (`out/t678/keep.txt`,
60 files) leaves out only the `rtsp` (rtspsrc), `sctp` and
`typefindfunctions` plugins. Every library is mapped: each is a NEEDED
dependency of something that is. `video4linux2` stays (MCM's device monitor
needs a provider).

`KEEP=/work/PATH/keep.txt sh musl/stage_musl_bundle.sh` drops those three
plugins plus `gst-launch-1.0`, `gst-inspect-1.0` and `gst-plugin-scanner`
(1.0 MB uncompressed):

| Bundle | Squashfs |
|---|---|
| `mcm-musl.sqfs` (before) | 23,629,824 |
| `mcm-musl-pruned.sqfs`, 64 KiB blocks (appfs) | **23,314,432** (sha256 `0b644353…`) |
| same, 1 MiB blocks (a separate image file) | 20,828,160 |

`-Xbcj arm` gives no gain; this mksquashfs has no arm64 BCJ filter.
`verify_bundle.sh` on the pruned stage (built with `KEEP_TOOLS=1`): every
element ok except `rtspsrc`, `sctpenc`, `sctpdec` (dropped on purpose; the
ONVIF/RTSP-relay route no longer works with this bundle), MCM answers REST.
On the camera, from RAM: 4K WebRTC 30.3 fps 0 freezes / 60 s, 720p WebRTC
30.0 fps 0 freezes / 30 s, MCM RTSP `cam_main` 301 frames / 10 s; the
1 MiB-block image: 4K WebRTC 30.2 fps 0 freezes / 30 s.

`fit_appfs.sh` (test only, never flashed; now takes a device code):

- 19/2/51 (UBI in mtd4): `HISI_AP_e72af0_appfs.img` + pruned bundle:
  200 of 265 LEBs, 8.3 MB left.
- 19/2/35 (raw squashfs in mtd5, this camera's layout): flashed
  `HISI_AP__cb7140_appfs_p35.img` (2.0 MB) + pruned bundle: 25.3 MB of
  50.3 MB, 25.0 MB left. hisi-web's check passes for both.

The bundle is the MCM binary (8.1 MB of xz alone, 13.1 MB at 64 KiB blocks)
plus GStreamer/GLib/OpenSSL/GnuTLS. Below ~20 MB needs the next steps from
Task 8: libnice on OpenSSL (drops GnuTLS, unistring, p11-kit, nettle,
hogweed, gmp, idn2, tasn1: ~1.8 MB xz), an MCM without frontend, zenoh,
tracy and swagger UI, or `gstreamer-full`.

### Decision outcome (apcamsrc, 2026-11-16)

| Threshold | Measured | Result |
|---|---|---|
| Flash ≤ ~20 MB | 23.3 MB pruned at appfs's 64 KiB blocks (20.8 MB at 1 MiB) | **Fail** (by 3.3 MB). On 19/2/35 mtd5 it still leaves 25 MB; on 19/2/51 8.3 MB, under the 10 MB wanted |
| RAM: MemAvailable > 64 MB with two WebRTC clients | 252.5 MB | Pass |
| CPU ≤ ~140% with one 4K client, camera-app RTSP 30 fps | ~35% (17% + 17.5%); 301 frames / 10 s | Pass |
| Latency: WebRTC adds ≤ ~100 ms over direct RTSP | ~0 ms network-side (4K +2.5 ms median, keyframes earlier; 720p +0.8 ms) + ~17 ms Chrome at 4K | Pass for what was measured; glass to glass not measured |

Per this section's rule, flash failing means a lean GStreamer and MCM build
comes first; CPU, RAM and latency leave apcamsrc as the route to keep.
