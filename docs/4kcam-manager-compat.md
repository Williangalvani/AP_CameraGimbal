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

A child process of `hisi-web` joins `239.255.255.250:3702` and answers `Probe`
messages with a `ProbeMatch`. It exits with its parent (`PR_SET_PDEATHSIG`), so
the supervisor's restart of `hisi-web` restarts it too. The match carries:

- `EndpointReference` `urn:uuid:<UUID derived from the eth0 MAC>`, stable across
  boots;
- type `dn:NetworkVideoTransmitter`;
- the vendor's scopes: `type/video_encoder`, `hardware/HW0100302`, `name/hd`,
  `Profile/Streaming`, `location/country`;
- `XAddrs` `http://<eth0 address>/onvif/device_service`.

### ONVIF

`POST /onvif/device_service` answers SOAP 1.2 for:

| Operation | Reply |
|---|---|
| `GetSystemDateAndTime` | the camera's UTC time |
| `GetDeviceInformation` | manufacturer `RadCam`, model `HDIPC`, firmware version the build hash, serial number the MAC, hardware ID `IPCamera` |
| `GetServices` | device management (ver10) and media (ver10), both at `/onvif/device_service` |
| `GetProfiles` | `main` and `sub`: video encoder configuration from `[stream.main]` / `[stream.sub]` (codec, resolution, frame rate, bitrate) |
| `GetStreamUri` | `rtsp://<eth0 address>:8554/video1` or `/video2` |

Other operations get a SOAP `ActionNotSupported` fault. Every operation except
`GetSystemDateAndTime` needs a valid UsernameToken `PasswordDigest` for user
`admin` and the configured password; a bad or missing one gets a
`NotAuthorized` fault. Replies must deserialize in MCM's `onvif-rs` schemas,
which check element names and namespaces, so they are checked against those
during development.

### Control protocol

`POST /action/<name>` returns HTTP 200 and a flat JSON object: the requested
fields under the vendor's key names, plus `code` (0 on success), `device_mac`,
`device_id`, `device_ip`, `log` and `sign_tby`. Unknown actions return `code` −1.
Settings are changed the same way the web's parameter form changes them, by
rewriting `camera.ini`, which camera-app picks up within 500 ms.

| Action | Supported |
|---|---|
| `getSysConfig` | identity fields only; the extension uses it as a liveness probe |
| `getImageAdjustment` / `setImageAdjustment` | table below |
| `getImageAdjustmentEx` / `setImageAdjustmentEx` | table below |
| `getVencConf` / `setVencConf` | `channel` 0 = main stream, 1 = sub stream; channel 2 is accepted and ignored |
| `restart` | restarts camera-app the way the web UI does (not a reboot) |

Field mapping:

| Vendor field | This firmware |
|---|---|
| `brightness`, `contrast`, `saturation` (0–255) | `image.*` (0–100), scaled and rounded |
| `auto_awb` 0 / 1 | `white_balance` `auto` / `manual`. Reads report 1 for every non-auto mode |
| `awb_red`, `awb_green`, `awb_blue` (0–255) | `wb_red` = r×15+256, `wb_green` = (g+32)×8, `wb_blue` = (b+32)×8, the vendor's slider formula. Reads invert it and clamp to 0–255 |
| `awb_auto_mode` 0 / 1 | new `image.wb_water` `green` / `blue` |
| `onceAWB` 1 | one-push with `wb_water`, through the same `camera.ini.wb.command` as the Photos page. The reply comes back at once; the extension then polls `getImageAdjustment` until the manual gains change and settle |
| `auto_exposureEx` 0 / 1, `exposure_time` | `shutter` `auto` / the nearest manual shutter |
| `mirror` and `flip` | both 1 ↔ `mount.orientation = inverted`; both 0 ↔ `upright`. Other combinations are ignored |
| `encode_type` 1 / 5 | `codec` `h264` / `h265` |
| `pic_width` × `pic_height`, `pixel_list` | `resolution`, from the target's resolution list |
| `bitrate` (kbps) | `[stream.main] bitrate_kbps`; the sub stream reports its fixed rate |
| `frame_rate` | `[stream.main] frame_rate`; the sub stream reports 30 |
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

With **Allow 4K Cam Manager** off, the discovery process doesn't run and
`/onvif/*` and `/action/*` return 404. These are the only routes that skip the
web login, and only while the setting is on, since the extension cannot log in.

## Cockpit

Through the extension, Cockpit gets the 4K Cam widget, the **4K Cam One-Push
White Balance** actions (all cameras or one) and the extension's joystick
suggestions. More camera actions, such as a photo or switching the water type,
would be added to the extension, not here.

## Testing

- **Scripted, against the camera** (build in RAM, as in `HANDOFF.md` §16): a
  WS-Discovery probe; each ONVIF operation with a correct digest, a wrong one
  and none; each `/action` read and write, checking the reply deserializes into
  the extension's types and the INI changes as the table says; `onceAWB`
  changing the manual gains within 2 s; the setting off giving 404 and no probe
  reply.
- **End to end, in Docker on the Mac**: MCM and 4K Cam Manager containers with
  host networking, the extension pointed at that MCM. Success is the camera in
  the extension's camera list, its stream in MCM, and the widget's controls and
  one-push changing the camera. MCM can only find the camera by multicast, so if
  Docker Desktop doesn't pass WS-Discovery, the end-to-end run moves to a Linux
  machine or BlueOS and the scripted probe covers discovery meanwhile.
