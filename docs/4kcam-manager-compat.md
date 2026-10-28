# 4K Cam Manager compatibility (Hi3516DV500)

The Hi3516DV500 + IMX678 camera is Blue Robotics' 4K Cam (the vendor firmware
calls it RadCam). Blue Robotics ships a BlueOS extension, [4K Cam
Manager](https://github.com/bluerobotics/br4kcam-manager), that finds the camera
through MAVLink Camera Manager (MCM), creates its streams, and gives Cockpit a
widget, a one-push white-balance action and joystick suggestions.

This firmware answers the same discovery, ONVIF and control requests as the
vendor's `xipc`, so the stock extension (0.4.x) and MCM work unchanged. Nothing
needs configuring in Cockpit.

The extension also drives focus, zoom and tilt, but through autopilot servo
outputs (`SERVOn_FUNCTION` and a generated Lua script), not the camera. Those
work regardless of the camera firmware and are not covered here.

## What the extension needs

From MCM, by ONVIF:

1. **WS-Discovery** finds the camera. The extension keeps only devices whose
   scopes give the name `hd` and the hardware `HW0100302` or `HW20200610`.
2. **`GetDeviceInformation`**: MCM names the video source
   `{model} - {manufacturer} ({hardware_id})`, and the extension wants
   `HDIPC - RadCam (IPCamera)`.
3. **`GetServices`**, then **`GetProfiles`** and **`GetStreamUri`** on the media
   service. Each profile needs a video encoder configuration. MCM probes the
   RTSP URL itself for the codec.
4. A WS-Security UsernameToken login as `admin` / `blue`.

From the extension, directly: `POST http://<camera>:80/action/<name>`, with no
authentication and a JSON body. Its reads send no body and no
`Content-Length`, which the web server takes as an empty body.

The extension only creates its MCM stream for a source URL ending in
`stream_0`, and keeps the image and video controls disabled until its
Hardware setup (the Navigator PWM channels for focus, zoom and tilt) has been
applied, even on a camera without those actuators.

## Behaviour

Everything below is in `hisi-web`, behind `APCAM_HAVE_4KCAM_COMPAT` (set in
`target_hisi.h`), and is served only while **Allow 4K Cam Manager** is on.

### Discovery

A child process of `hisi-web` joins `239.255.255.250:3702` on every IPv4
interface and answers `Probe` messages with a `ProbeMatch`, sent back to the
prober. It exits with its parent (`PR_SET_PDEATHSIG`), so the supervisor's
restart of `hisi-web` restarts it too. It reads **Allow 4K Cam Manager** for
each probe, since saving the setting doesn't restart the web. The group is
rejoined every 30 s or so, so an interface that comes up later is joined within
about a minute at worst. The child starts only when the web listens on its
default port (80), since `XAddrs` carry no port; with `-p` another port it logs
`WS-Discovery off`. If port 3702 can't be bound, the child logs
`WS-Discovery unavailable` and exits; it starts again only with `hisi-web`.

A probe is answered only if its `MessageID` isn't empty and its `Types` are
absent or empty (MCM sends an empty `Types`) or include a QName whose local
part is `NetworkVideoTransmitter` or `Device`; the namespaces aren't checked.
Scopes in the probe aren't checked. Datagrams from a multicast address,
`255.255.255.255`, `0.0.0.0` or port 0 are dropped before anything else is
done. The
match carries:

- `EndpointReference` `urn:uuid:<UUID derived from the eth0 MAC>`, stable across
  boots;
- type `dn:NetworkVideoTransmitter`;
- the vendor's scopes: `type/video_encoder`, `hardware/HW0100302`, `name/hd`,
  `Profile/Streaming`, `location/country`;
- `XAddrs` `http://<address>/onvif/device_service`, the camera's address on the
  route to the prober (ONVIF replies use the address the request came in on,
  since MCM requires service addresses within the one it discovered).

### ONVIF

`POST /onvif/device_service` answers SOAP 1.2 for:

| Operation | Reply |
|---|---|
| `GetSystemDateAndTime` | the camera's UTC time |
| `GetDeviceInformation` | manufacturer `RadCam`, model `HDIPC`, firmware version the build hash, serial number the MAC, hardware ID `IPCamera` |
| `GetServices` | device management (ver10) and media (ver10), both at `http://<address>/onvif/device_service` |
| `GetProfiles` | `main` and `sub`: video encoder configuration from `[stream.main]` / `[stream.sub]` (codec, resolution, frame rate, bitrate) |
| `GetStreamUri` | `rtsp://<address>:<CAMERA_APP_RTSP_PORT or 8554>/stream_0` or `/stream_1`, the vendor's paths; the extension only streams a source ending in `stream_0`. The hisi camera-app serves them alongside `/video1` and `/video2`. |

`<address>` is the camera's address on the connection. Other operations get a
SOAP `ActionNotSupported` fault, and a body without an operation
`InvalidArgVal`, both with HTTP 400. Every operation except
`GetSystemDateAndTime` needs a valid UsernameToken `PasswordDigest` for user
`admin` and the configured password; a bad or missing one gets HTTP 400 and a
fault with subcode `ter:NotAuthorized`, on which onvif-rs retries with a token.
`Username`, `Password`, `Nonce` and `Created` are read from the `UsernameToken`
on, so a `wsu:Timestamp`'s `Created` before it can't stand in. The digest is
compared in constant time. `Created` isn't compared with the clock, which may
not be set, so a captured request can be replayed.

The ONVIF password protects only what ONVIF itself reveals (device
information, profiles and stream URIs): RTSP and `/action` are open while
**Allow 4K Cam Manager** is on, so that setting is the real gate.

Replies must deserialize in MCM's `onvif-rs` schemas, which check element names
and namespaces; `web/tests/onvif-check` checks them. onvif-rs's ver10 encoding
enum has no H.265, so a stream set to H.265 reads there as an unknown
encoding. MCM keeps the profile and takes the codec from the RTSP stream.

### Control protocol

`POST /action/<name>` returns HTTP 200 and a flat JSON object: the requested
fields under the vendor's key names, plus `code` (0 on success), `device_mac`,
`device_id`, `device_ip`, `log` and `sign_tby`. Unknown actions return `code` −1.
Settings are changed the same way the web's parameter form changes them, by
rewriting `camera.ini`, which camera-app picks up within 500 ms. `set*`
requests hold a `flock` on `<RUNTIME_DIR>/fourkcam.lock` from reading
`camera.ini` to saving it, so overlapping ones can't drop each other's changes;
the lock doesn't cover the parameter form or camera-app.

The body is parsed strictly as one flat JSON object; an empty body is an empty
object. Only numbers are used: strings, nested values and literals are checked
and skipped. A malformed number, string or bracket rejects the whole request
with `code` −1. Fields that are 0 / 1 enums (`auto_awb`, `awb_auto_mode`,
`auto_exposureEx`, `auto_gain_mode`, `manual_AGain_enable`,
`manual_DGain_enable`, `AE_strategy_mode`, `low_farme_rate`, `mirror`, `flip`)
count only when 0 or 1. A field sent with
the value reads report changes nothing, so echoing a whole reply back, as a
client may, doesn't move settings that don't map exactly. Read replies clamp
each value to the extension's type (0–255, the enums, u16), so they always
deserialize.

| Action | Supported |
|---|---|
| `getSysConfig` | identity fields only; the extension uses it as a liveness probe |
| `getImageAdjustment` / `setImageAdjustment` | table below |
| `getImageAdjustmentEx` / `setImageAdjustmentEx` | table below |
| `getVencConf` / `setVencConf` | `channel` 0 = main stream, 1 = sub stream; channel 2 reads as the sub stream, and sets to it are accepted and ignored |
| `restart` | restarts camera-app the way the web UI does (not a reboot); while a restart is in progress it replies at once with `code` −1 and `log` `restart in progress` instead of queueing another |

Field mapping:

| Vendor field | This firmware |
|---|---|
| `brightness`, `contrast`, `saturation` (0–255) | `image.*` (0–100), scaled and rounded |
| `auto_awb` 0 / 1 | `white_balance` `auto` / `manual`. Reads report 1 for every non-auto mode; 1 switches only from `auto`, so a preset or colour temperature stays unless the gains change. Gains sent with `auto_awb` 0 are saved, but white balance stays `auto` |
| `awb_red`, `awb_green`, `awb_blue` (0–255) | `wb_red` = r×15+256, `wb_green` = (g+32)×8, `wb_blue` = (b+32)×8, the vendor's slider formula, and `white_balance` `manual`. Reads invert it and clamp to 0–255 |
| `awb_auto_mode` 0 / 1 | `image.wb_water` `green` / `blue`: the auto white balance calibration and colour matrices for that water, and the one-push target |
| `hue`, `sharpness`, `gamma` (0–255) | `image.*`, the same value. On the ISP: hue × 100 / 255 (128 is neutral); sharpness moves each ISO step's detail by sharpness / 2 − 64 around the vendor's tuning; gamma picks one of three tone curves by gamma / 32 (224–255 keep the current one) |
| `blc_level` (0–255) | `image.backlight`: the auto-exposure brightness target (the vendor's name aside, not black level) |
| `awb_style_red`, `awb_style_green`, `awb_style_blue` (0–255) | `image.wb_style_*`: a gain of 1 + value / 512 on that colour after colour correction |
| `onceAWB` 1 | one-push with `wb_water`, through the same `camera.ini.wb.command` as the Photos page. The reply comes back at once; the extension then polls `getImageAdjustment` until the manual gains change and settle. Nothing reads the result, so camera-app's `camera.ini.wb.status` stays until the next one-push |
| `auto_exposureEx` 0 / 1, `exposure_time` | `shutter` `auto` / the nearest manual shutter |
| `max_exposure` (1/x s) | `image.max_exposure`, one of the extension's 13 values (others snap to the nearest); the longest automatic exposure. Reads report the nearest of them |
| `max_sys_gain`, `auto_AGain_max`, `auto_DGain_max` (0–255) | `image.max_gain`, `max_again`, `max_dgain`: automatic gain limits of (48 × value + 1)×, 0.75 × value and value × (each at least 1×) |
| `auto_gain_mode` 0 / 1 | `image.gain_mode` `auto` / `manual`. Manual lets the two enables below fix those gains; exposure time stays automatic unless `auto_exposureEx` is 1. A fixed `iso` (Photos page, MAVLink) overrides it |
| `manual_AGain_enable`, `manual_AGain`, `manual_DGain_enable`, `manual_DGain` | `image.manual_again`, `again`, `manual_dgain`, `dgain`: with `gain_mode` manual, analog or ISP digital gain fixed at value / 4 in whole steps (at least 1×) |
| `AE_strategy_mode` 0 / 1 | `image.ae_strategy` `highlight` / `lowlight` priority |
| `low_farme_rate` 0 / 1 (`setImageAdjustmentEx`) | `image.slow_shutter`: auto exposure may lower the frame rate in the dark |
| `mirror`, `flip` | `mount.orientation`, which only has a 180° rotation: reads report both as 1 for `inverted` and both as 0 otherwise. The extension sends one of them alone; any one sent that differs from the current state sets `inverted` (1) or `upright` (0), and otherwise nothing changes |
| `set_default` 1 (`setImageAdjustment`) | resets every `[image]` key above that `setImageAdjustment` maps (not `slow_shutter` or orientation) to the web's defaults; the request's other fields are ignored. The extension sends it for the image parameters' **Restore defaults** and with its recommended settings |
| `encode_type` 1 / 5 | `codec` `h264` / `h265` |
| `pic_width` × `pic_height`, `pixel_list` | `resolution`, from the target's resolution list: the requested size, else one of the same height (the extension's recommended sub stream is 640×480) |
| `bitrate` (kbps) | `[stream.main] bitrate_kbps`, 1–20000. Reads of 0 (automatic), and of the sub stream, report the automatic rate for the resolution |
| `frame_rate` | `[stream.main] frame_rate`, 5–30; the sub stream reports 30 |
| `max_framerate` | 30 |
| anything else, including `setImageAdjustmentEx`'s `set_default` | accepted and ignored; reads report the vendor's defaults |

Resolution and codec changes need camera-app to restart. As with the vendor,
they apply on the extension's **Save and restart camera**, which sends
`restart`. The reply comes once camera-app is back. The extension then waits
for the camera to stop answering, up to its 60 s offline timeout, and since the
web stays up during a camera-app restart it always waits the full 60 s
before it carries on.

### Settings

| INI | Web | Default |
|---|---|---|
| `[compat] fourkcam_manager` | **Allow 4K Cam Manager** | `true` |
| `[compat] onvif_password` | **ONVIF password** | `blue` |
| `[image] wb_water` | **One-push water** (green / blue), which also sets the auto white balance calibration | `green` |
| `[image] hue`, `sharpness`, `gamma`, `backlight`, `wb_style_*`, `max_gain`, `gain_mode`, `max_again`, `max_dgain`, `manual_again`, `again`, `manual_dgain`, `dgain`, `ae_strategy`, `max_exposure`, `slow_shutter` | Video tab, in the units above | vendor defaults: 128, 128, 20, 50, 0, 12, auto, 32, 0, false, 128, false, 128, highlight, 25, false |

With **Allow 4K Cam Manager** off, discovery doesn't answer and
`POST /onvif/device_service` and `POST /action/*` return 404. These are the
only state-changing routes that skip the web login, and only while the setting
is on, since the extension cannot log in; other methods and paths need the
login as usual. While it is on, a request to either route with an `Origin`
header gets 403 and does nothing: browsers send `Origin` on every POST, and
neither the extension nor MCM (both reqwest) sends it, so a web page the user
visits can't drive the camera.

These settings fail closed; defaults apply only to keys absent from
`camera.ini`. If `camera.ini` can't be read, or its `fourkcam_manager` value is
too long to read, the routes return 404 and discovery doesn't answer. They are
on only when the value is exactly `true`. If `onvif_password` can't be read, is
too long or is empty, every ONVIF login fails. An `/action` set that can't take
its lock or read `camera.ini` gets `code` −1.

The ONVIF password is 1–63 bytes (not characters) with no whitespace or control
characters, as the parameters page checks.

## Cockpit

Through the extension, Cockpit gets the 4K Cam widget, the **4K Cam One-Push
White Balance** actions (all cameras or one) and the extension's joystick
suggestions. More camera actions, such as a photo or switching the water type,
would be added to the extension, not here.

## Testing

- **Unit tests** (host): `web/tests/test_onvif.cpp` and
  `web/tests/test_fourkcam.cpp`, in `make -C web test`. They build and run on
  macOS or Linux on their own: build with `make -C web tests/test_onvif
  tests/test_fourkcam`, then run `./tests/test_onvif` and
  `./tests/test_fourkcam` from `web/` (they read fixtures relative to it). The
  rest of `make -C web test` runs on Linux.
- **Schemas**: `web/tests/onvif-check` builds onvif-rs at MCM's revision on a
  Linux host with cargo and parses replies the way MCM does.
  `run.sh HOST requests` regenerates the request fixtures; `run.sh HOST check
  DIR` checks saved replies.
- **On the camera** (`hisi-web` bind-mounted in RAM):
  `web/tests/check_4kcam_compat.py`, run from a machine on the camera's LAN,
  probes WS-Discovery (including `Types` it should and shouldn't answer), runs
  each ONVIF operation with a correct digest, a wrong one and none, checks
  that requests with `Origin` get 403 and change nothing, reads and writes
  each `/action` (checking the extension's types, `mirror` or `flip` alone and
  `set_default`), runs `onceAWB` (manual gains within 10 s), turns the setting
  off (404, no probe reply) and restarts camera-app, checking that a second
  `restart` meanwhile gets `restart in progress`. It backs up and restores
  `camera.ini` (waiting up to 20 s more for a one-push that missed the 10 s,
  so its gains can't land on the restored file) and saves the ONVIF replies
  for `run.sh check`; it fails if the probe with the fixture's `MessageID`
  gets no reply rather than keep an old `probe.xml`.
- **End to end** (not yet run): BlueOS on a Raspberry Pi on the camera's LAN,
  with MCM and 4K Cam Manager installed. Success is the camera in the
  extension's list, its stream in MCM, and the widget's controls and one-push
  changing the camera. (Docker on the Mac was the plan, but Docker isn't
  installed there.)
