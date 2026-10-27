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
authentication and a JSON body.

## Behaviour

Everything below is in `hisi-web`, behind `APCAM_HAVE_4KCAM_COMPAT` (set in
`target_hisi.h`), and is served only while **Allow 4K Cam Manager** is on.

### Discovery

A child process of `hisi-web` joins `239.255.255.250:3702` on every IPv4
interface and answers `Probe` messages with a `ProbeMatch`, sent back to the
prober. It exits with its parent (`PR_SET_PDEATHSIG`), so the supervisor's
restart of `hisi-web` restarts it too. It reads **Allow 4K Cam Manager** for
each probe, since saving the setting doesn't restart the web. Interfaces that
come up later are joined within 30 s. Every probe is answered, whatever types
or scopes it asks for. If port 3702 can't be bound, the child logs
`WS-Discovery unavailable` and exits; it starts again only with `hisi-web`.
The match carries:

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
| `GetStreamUri` | `rtsp://<address>:<CAMERA_APP_RTSP_PORT or 8554>/video1` or `/video2` |

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
`auto_exposureEx`, `mirror`, `flip`) count only when 0 or 1. A field sent with
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
| `restart` | restarts camera-app the way the web UI does (not a reboot) |

Field mapping:

| Vendor field | This firmware |
|---|---|
| `brightness`, `contrast`, `saturation` (0–255) | `image.*` (0–100), scaled and rounded |
| `auto_awb` 0 / 1 | `white_balance` `auto` / `manual`. Reads report 1 for every non-auto mode; 1 switches only from `auto`, so a preset or colour temperature stays unless the gains change |
| `awb_red`, `awb_green`, `awb_blue` (0–255) | `wb_red` = r×15+256, `wb_green` = (g+32)×8, `wb_blue` = (b+32)×8, the vendor's slider formula, and `white_balance` `manual`. Reads invert it and clamp to 0–255 |
| `awb_auto_mode` 0 / 1 | new `image.wb_water` `green` / `blue` |
| `onceAWB` 1 | one-push with `wb_water`, through the same `camera.ini.wb.command` as the Photos page. The reply comes back at once; the extension then polls `getImageAdjustment` until the manual gains change and settle. Nothing reads the result, so camera-app's `camera.ini.wb.status` stays until the next one-push |
| `auto_exposureEx` 0 / 1, `exposure_time` | `shutter` `auto` / the nearest manual shutter |
| `mirror` and `flip` | both 1 ↔ `mount.orientation = inverted`; both 0 ↔ `upright`. Other combinations are ignored |
| `encode_type` 1 / 5 | `codec` `h264` / `h265` |
| `pic_width` × `pic_height`, `pixel_list` | `resolution`, from the target's resolution list: the requested size, else one of the same height (the extension's recommended sub stream is 640×480) |
| `bitrate` (kbps) | `[stream.main] bitrate_kbps`, 1–20000. Reads of 0 (automatic), and of the sub stream, report the automatic rate for the resolution |
| `frame_rate` | `[stream.main] frame_rate`, 5–30; the sub stream reports 30 |
| `max_framerate` | 30 |
| anything else, including `set_default` | accepted and ignored; reads report the vendor's defaults |

Resolution and codec changes need camera-app to restart. As with the vendor,
they apply on the extension's **Save and restart camera**, which sends
`restart`.

### Settings

| INI | Web | Default |
|---|---|---|
| `[compat] fourkcam_manager` | **Allow 4K Cam Manager** | `true` |
| `[compat] onvif_password` | **ONVIF password** | `blue` |
| `[image] wb_water` | **One-push water** (green / blue) | `green` |

With **Allow 4K Cam Manager** off, discovery doesn't answer and
`POST /onvif/device_service` and `POST /action/*` return 404. These are the
only routes that skip the web login, and only while the setting is on, since
the extension cannot log in; other methods and paths need the login as usual.

These settings fail closed. Defaults apply only to keys absent from
`camera.ini`; if the file can't be read, or a value doesn't fit, the routes
return 404, discovery doesn't answer and every ONVIF login fails, as it does
with an empty password. An `/action` request that can't read `camera.ini` gets
`code` −1.

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
  macOS or Linux on their own (`make -C web tests/test_onvif
  tests/test_fourkcam`, run from `web/`); the rest of `make -C web test` runs
  on Linux.
- **Schemas**: `web/tests/onvif-check` builds onvif-rs at MCM's revision on a
  Linux host with cargo and parses replies the way MCM does.
  `run.sh HOST requests` regenerates the request fixtures; `run.sh HOST check
  DIR` checks saved replies.
- **On the camera** (`hisi-web` bind-mounted in RAM):
  `web/tests/check_4kcam_compat.py`, run from a machine on the camera's LAN,
  probes WS-Discovery, runs each ONVIF operation with a correct digest, a wrong
  one and none, reads and writes each `/action` (checking the extension's
  types), runs `onceAWB` (manual gains within 10 s), turns the setting off
  (404, no probe reply) and restarts camera-app. It backs up and restores
  `camera.ini` and saves the ONVIF replies for `run.sh check`.
- **End to end** (not yet run): BlueOS on a Raspberry Pi on the camera's LAN,
  with MCM and 4K Cam Manager installed. Success is the camera in the
  extension's list, its stream in MCM, and the widget's controls and one-push
  changing the camera. (Docker on the Mac was the plan, but Docker isn't
  installed there.)
