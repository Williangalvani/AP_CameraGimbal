# 4K Cam Manager image controls, vendor-like: design

Status: draft for review, 2026-10-27.

## Goal

The 4K Cam Manager extension's image widget sends about twenty
`setImageAdjustment` fields that this firmware accepts and ignores. Make the ones
in the **Image color**, **White balance** and **Exposure** groups act on the
picture the way the vendor firmware does (same ISP call, same value mapping,
same defaults), and make reads report what was set. The Advanced-tab extras
(defog, frame turbo, rotate, noise reduction, anti-flicker, power frequency,
WDR, highlight compensation, lens correction) are out of scope.

The vendor behaviour comes from static analysis of the vendor application,
kept privately outside this repository. Only behaviour (formulas, defaults,
which SDK call) goes into this repository, as with the white-balance calibration
already in `hisi/isp.cpp`.

## Decisions

- **Settings hold the extension's own values** (0–255 sliders, 0/1 enums,
  the 1/x s exposure enum), and `camera-app` applies the vendor's formulas.
  Reads then return exactly what was written, so sliders don't jump. The web
  server mostly passes values through.
- **No raw vendor tables are committed.** The three gamma curves are generated
  in code from about 33 fitted control points each, interpolated with a
  monotone cubic; the sharpening luma weights are a generated ramp. Target
  error: within 1% of full scale of the vendor curve at every point.
- **The vendor's boot look is reproduced** (sharpening, gamma, colour matrices,
  AE target), even when no control has been moved. The vendor's 200 ms
  ISO-driven task (DRC strength, ISP black level, saturation by ISO, 3DNR
  variants) is **not** reproduced in this change.
- **Not MAVLink parameters.** The new keys have no parameter name (like
  `stream.*.alias`), so the MAVLink parameter list and camera definition don't
  change. They do appear on the web Settings page.

## Settings

New `[image]` keys, only on targets with `APCAM_HAVE_4KCAM_COMPAT` (hisi),
read by camera-app (APC_Config table, `struct ca_config`, and `IMAGE_FIELDS`,
so they apply live like brightness) and listed in the web parameter table
(Settings page, en/zh/ja strings, defaults used by `set_default`). Defaults are
the vendor's `SensorDefault.ini`.

| Key | Vendor field | Range | Default |
|---|---|---|---|
| `hue` | `hue` | 0–255 | 128 |
| `sharpness` | `sharpness` | 0–255 | 128 |
| `gamma` | `gamma` | 0–255 | 20 |
| `backlight` | `blc_level` | 0–255 | 50 |
| `wb_style_red`, `wb_style_green`, `wb_style_blue` | `awb_style_*` | 0–255 | 0 |
| `max_gain` | `max_sys_gain` | 0–255 | 12 |
| `gain_mode` | `auto_gain_mode` | `auto` / `manual` | `auto` |
| `max_again` | `auto_AGain_max` | 0–255 | 32 |
| `max_dgain` | `auto_DGain_max` | 0–255 | 0 |
| `manual_again` | `manual_AGain_enable` | bool | false |
| `again` | `manual_AGain` | 0–255 | 128 |
| `manual_dgain` | `manual_DGain_enable` | bool | false |
| `dgain` | `manual_DGain` | 0–255 | 128 |
| `ae_strategy` | `AE_strategy_mode` | `highlight` / `lowlight` | `highlight` |
| `max_exposure` | `max_exposure` | the extension's 13 values, 1/x s | 25 |
| `slow_shutter` | `low_farme_rate` (setImageAdjustmentEx) | bool | false |

The existing web-only `[image] wb_water` (`green` / `blue`) moves into the
camera-app table as well, because it now changes continuous auto white balance.

## ISP mapping (camera-app, `hisi/isp.cpp`)

All are `ss_mpi_isp_*` get-modify-set on pipe 0. The struct layouts are the
B040 ones we already build against; no ABI bridging is needed.

- **hue**: `csc_attr.hue = v*100/255` (128 is neutral 50), in the existing
  `set_csc`.
- **sharpness**: `sharpen_attr`, enable on, auto tables for all 16 ISO bins:
  `detail_ctrl[i] = max(0, base[i] + v/2 − 64)` with the vendor's per-ISO base,
  plus its fixed texture/edge frequency, overshoot, undershoot, shoot
  suppression, max sharp gain and luma weight tables. `op_type` is left as the
  sensor driver set it; verify on hardware that it is auto.
- **gamma**: `gamma_attr`, user curve. Curve index `v>>5`: 0, 4, 6 → curve A;
  1, 3, 5 → curve B; 2 → curve C; 7 (224–255) keeps the previous curve, as the
  vendor does.
- **backlight**: `exposure_attr.auto_attr.compensation = v` (the AE luminance
  target; despite the name it is not black level). It combines with the
  existing `exposure_compensation` EV bias.
- **wb_style_***: `color_tone_attr.{red,green,blue}_cast_gain = v/2 + 256`
  (1.0–1.5×, after the colour matrix; independent of the WB mode).
- **wb_water**: auto-WB calibration (`static_wb`, `curve_para`) and the four
  colour matrices (`ccm_attr`, auto) for green or blue water. Today the code
  always uses the blue-water calibration with the sensor's matrices; green
  (the default) gets the vendor's green-water set. One-push keeps its current
  algorithms.
- **Gains and modes**, in the existing `set_exposure`:
  - `auto_attr.sys_gain_range.max = max_gain*48*1024 + 1024`
  - `auto_attr.a_gain_range.max = max(1024, max_again*768)`
  - `auto_attr.ispd_gain_range.max = max(1024, max_dgain*1024)`
  - `op_type` is manual when `gain_mode` is manual, `shutter` is manual or
    `iso` is fixed. The vendor lets the last-changed of exposure mode and gain
    mode win; we use this fixed rule instead, so the result doesn't depend on
    the order of requests.
  - When `iso` is auto and `gain_mode` is manual: `manual_again` sets
    `a_gain_op_type` manual with `a_gain = max(1024, (again/4)*1024)`;
    `manual_dgain` sets sensor `d_gain` to 1× and `ispd_gain_op_type` manual
    with `isp_d_gain = max(1024, (dgain/4)*1024)`. Disabled ones stay auto.
    A fixed `iso` keeps today's behaviour and overrides these.
  - `ae_strategy_mode` = highlight (0) / lowlight (1) prior.
  - `exp_time_range.max = 1000000/max_exposure` µs, when the shutter is auto.
  - `ae_mode` = slow shutter when `slow_shutter`, else fixed frame rate.
- **Boot**: the pipeline start applies all of the above (today exposure is only
  applied at boot when it differs from auto). Gamma, sharpening and colour
  matrices are re-applied only when their inputs change.

## Web (`APC_FourKCam.h`, `mt11-web.cpp`)

- `getImageAdjustment` reports the stored values instead of the constants it
  sends now (`AE_strategy_mode` etc.); `getImageAdjustmentEx` reports
  `low_farme_rate`.
- `setImageAdjustment` / `setImageAdjustmentEx` write a key only when the sent
  value differs from the stored one (the existing echo rule). Out-of-range
  numbers are clamped; 0/1 enums count only when 0 or 1.
- `max_exposure` accepts the extension's 13 values and snaps others to the
  nearest one, like `exposure_time`.
- `set_default` also resets the new keys; `Changes` grows to hold them all.
- `docs/4kcam-manager-compat.md` gets the new field rows and settings.

## Not changed

- `brightness`, `contrast`, `saturation` keep their 0–100 storage (they are
  MAVLink parameters), so a slider can still move by one step on read
  (160 reads back as 161).
- `exposure_time` still snaps to the shutter list's 8 values.
- The vendor's ISO-driven 200 ms task, night mode and noise-reduction variants.

## Testing

- `web/tests/test_fourkcam.cpp`: each new field's set, echo (no change),
  clamp, read-back, and `set_default`.
- `camera_app/tests/test_config.cpp`: the new keys parse, default and compare
  in `ca_config_image_equal`.
- A host test of the generated gamma curves against the fitted points
  (monotone, 0 and 4095 at the ends) and of the gain formulas.
- `web/tests/check_4kcam_compat.py` on the camera: set/read round trip of every
  new field, and restoring defaults.
- On the camera, in RAM only (deployed through the Pi), measured on stream_1
  frames: hue rotates colour; sharpness changes edge energy; the three gamma
  curves differ; backlight changes mean luma; style gains shift each channel;
  max gain and max exposure cap `query_exposure_info` in a dark scene; manual
  gains pin the reported gains; slow shutter lets exposure pass one frame time.
  Then the existing integration tests.
