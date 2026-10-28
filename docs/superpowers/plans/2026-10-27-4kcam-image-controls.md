# 4K Cam Manager image controls, vendor-like: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the 4K Cam Manager widget's ignored Image color, White balance and Exposure controls act on the hisi camera's ISP the way the vendor firmware does, and read back what was set.

**Architecture:** New `[image]` keys hold the extension's own values (0–255, 0/1, 1/x s). The web server's `/action` mapping (`web/APC_FourKCam.h`) passes them through to `camera.ini`; camera-app loads them (`APC_Config`), treats them as live image settings, and `hisi/isp.cpp` converts them to SDK values with pure functions in a new host-testable header `hisi/image_tuning.h`.

**Tech Stack:** C++17, Hi3519DV500 B040 MPP SDK (`ss_mpi_isp_*`), host tests with `assert`, GNU make.

Spec: `docs/superpowers/specs/2026-10-27-4kcam-image-controls-design.md`.

## Global Constraints

- Only targets with `APCAM_HAVE_4KCAM_COMPAT` (hisi) get the new keys; every other target builds and behaves exactly as before.
- New keys and defaults (the vendor firmware's): `hue` 128, `sharpness` 128, `gamma` 20, `backlight` 50, `wb_style_red`/`wb_style_green`/`wb_style_blue` 0, `wb_water` `green`, `max_gain` 12, `gain_mode` `auto`, `max_again` 32, `max_dgain` 0, `manual_again` false, `again` 128, `manual_dgain` false, `dgain` 128, `ae_strategy` `highlight`, `max_exposure` 25, `slow_shutter` false. Levels are 0–255; `max_exposure` is 12–8000.
- The new keys are **not** MAVLink parameters (`param_name` NULL).
- No raw vendor tables are committed: gamma is generated from 30 knots; sharpening uses the 16-entry per-ISO tables and one 32-entry luma weight curve given in Task 1. Comments never say where numbers came from.
- A field sent with the value reads report changes nothing (the existing echo rule in `APC_FourKCam.h`).
- Comments only state constraints the code can't show; match the surrounding code's style and density.
- Implementers never contact the camera (192.168.2.10) or the BlueOS Pi, never push, and never flash. Commit on the current branch only.
- Cross toolchain prefix: `X=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux-`.

---

### Task 1: ISP tuning formulas (`hisi/image_tuning.h`)

**Files:**
- Create: `camera_app/src/backends/hisi/image_tuning.h`
- Create: `camera_app/tests/test_hisi_tuning.cpp`
- Modify: `camera_app/Makefile` (new `hisi-image-test` target next to `timezone-test` at line ~482, and add it to the `test:` prerequisites at line ~815)
- Modify: `.gitignore` (next to `/camera_app/tests/test_timezone`, line ~45)

**Interfaces:**
- Produces, in namespace `ca_hisi_tuning`: `iso_bins` (16), `luma_bins` (32), `gamma_nodes` (1025), `gamma_knot_count` (30), `gamma_knots[]`, `gamma_curves[3][]`, `csc_hue(unsigned) -> uint8_t`, `sharpen_detail(unsigned sharpness, unsigned bin) -> uint8_t`, tables `sharpen_texture_freq`, `sharpen_edge_freq`, `sharpen_over_shoot`, `sharpen_under_shoot`, `sharpen_shoot_sup_adj`, `sharpen_max_gain`, `sharpen_luma_weight`, `gamma_curve(unsigned) -> int`, `gamma_table(int curve, uint16_t *table)`, `cast_gain(unsigned) -> uint16_t`, `sys_gain_max/again_max/ispd_gain_max/manual_gain(unsigned) -> uint32_t`, `exposure_max_us(unsigned) -> uint32_t`, `struct Water { uint16_t static_wb[4]; int32_t curve[6]; uint16_t temps[4]; uint16_t ccm[4][9]; }`, `waters[2]` (index 0 green, 1 blue).

- [ ] **Step 1: Write the failing test** `camera_app/tests/test_hisi_tuning.cpp`

```cpp
#include "image_tuning.h"

#include <cassert>
#include <cstdio>

using namespace ca_hisi_tuning;

static int ccm_value(uint16_t value)
{
    return (value & 0x8000U) ? -(int)(value & 0x7fffU) : (int)value;
}

int main()
{
    assert(csc_hue(0) == 0 && csc_hue(128) == 50 && csc_hue(255) == 100 && csc_hue(300) == 100);

    assert(sharpen_detail(128, 0) == 135 && sharpen_detail(128, 15) == 20);
    assert(sharpen_detail(0, 0) == 71 && sharpen_detail(255, 0) == 198);
    assert(sharpen_detail(0, 15) == 0 && sharpen_detail(255, 15) == 83 && sharpen_detail(999, 15) == 83);

    assert(gamma_curve(0) == 0 && gamma_curve(20) == 0 && gamma_curve(31) == 0);
    assert(gamma_curve(32) == 1 && gamma_curve(64) == 2 && gamma_curve(95) == 2 && gamma_curve(96) == 1);
    assert(gamma_curve(128) == 0 && gamma_curve(160) == 1 && gamma_curve(192) == 0 && gamma_curve(223) == 0);
    assert(gamma_curve(224) == -1 && gamma_curve(255) == -1 && gamma_curve(999) == -1);

    uint16_t table[gamma_nodes];
    for (int curve = 0; curve < 3; curve++) {
        gamma_table(curve, table);
        assert(table[0] == 0 && table[gamma_nodes - 1] == 4095);
        for (unsigned k = 0; k < gamma_knot_count; k++) assert(table[gamma_knots[k]] == gamma_curves[curve][k]);
        for (unsigned x = 1; x < gamma_nodes; x++) assert(table[x] >= table[x - 1]);
    }
    gamma_table(0, table);
    assert(table[2] == 17 && table[20] == 165 && table[100] == 1100);
    gamma_table(2, table);
    assert(table[2] == 19 && table[20] == 195 && table[100] == 1142);

    assert(cast_gain(0) == 256 && cast_gain(128) == 320 && cast_gain(255) == 383 && cast_gain(999) == 383);
    assert(sys_gain_max(0) == 1024 && sys_gain_max(12) == 590848 && sys_gain_max(255) == 12534784);
    assert(again_max(0) == 1024 && again_max(1) == 1024 && again_max(2) == 1536 && again_max(32) == 24576);
    assert(ispd_gain_max(0) == 1024 && ispd_gain_max(1) == 1024 && ispd_gain_max(3) == 3072);
    assert(manual_gain(0) == 1024 && manual_gain(7) == 1024 && manual_gain(8) == 2048);
    assert(manual_gain(128) == 32768 && manual_gain(255) == 64512);
    assert(exposure_max_us(12) == 83333 && exposure_max_us(25) == 40000 && exposure_max_us(8000) == 125);

    assert(waters[1].static_wb[0] == 500 && waters[1].static_wb[3] == 504 && waters[0].static_wb[0] == 469);
    for (const Water &water : waters) {
        for (unsigned t = 0; t < 4; t++) {
            if (t > 0) assert(water.temps[t] < water.temps[t - 1]);
            for (unsigned row = 0; row < 3; row++) {
                int sum = 0;
                for (unsigned column = 0; column < 3; column++) sum += ccm_value(water.ccm[t][row * 3 + column]);
                assert(sum == 256);
            }
        }
    }
    puts("test_hisi_tuning: ok");
    return 0;
}
```

- [ ] **Step 2: Add the make target and ignore the binary**

In `camera_app/Makefile`, after the `tests/test_timezone` rule:

```make
.PHONY: hisi-image-test
hisi-image-test: tests/test_hisi_tuning
	./tests/test_hisi_tuning

tests/test_hisi_tuning: tests/test_hisi_tuning.cpp src/backends/hisi/image_tuning.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(APP_CXXFLAGS) -Isrc/backends/hisi -o $@ $<
```

Add `hisi-image-test` after `timezone-test` in the `test:` prerequisite list. If the Makefile's `clean` rule lists `tests/test_timezone`, add `tests/test_hisi_tuning` beside it. In `.gitignore`, add `/camera_app/tests/test_hisi_tuning` after `/camera_app/tests/test_timezone`.

- [ ] **Step 3: Run it to verify it fails**

Run: `make -C camera_app hisi-image-test`
Expected: compile error, `image_tuning.h` not found.

- [ ] **Step 4: Write `camera_app/src/backends/hisi/image_tuning.h`**

```cpp
#pragma once
/* The 4K Cam Manager's image controls as the vendor firmware applies them to
 * this camera's ISP: settings in the extension's units in, SDK values out.
 * Gains are in 1/1024 (1024 = 1x). No SDK types, so host tests can check it. */
#include <cstdint>

namespace ca_hisi_tuning {

constexpr unsigned iso_bins = 16;
constexpr unsigned luma_bins = 32;
constexpr unsigned gamma_nodes = 1025;
constexpr unsigned gamma_knot_count = 30;

inline unsigned level(unsigned value) { return value > 255U ? 255U : value; }

/* csc hue is 0–100 with 50 neutral */
inline uint8_t csc_hue(unsigned hue) { return (uint8_t)(level(hue) * 100U / 255U); }

constexpr uint8_t sharpen_detail_base[iso_bins] = {135, 128, 100, 80, 76, 70, 70, 60, 40, 30, 20, 20, 20, 20, 20, 20};
constexpr uint16_t sharpen_texture_freq[iso_bins] = {100, 86, 72, 64, 56, 56, 56, 56, 54, 52, 50, 50, 50, 50, 50, 50};
constexpr uint16_t sharpen_edge_freq[iso_bins] = {256, 230, 200, 160, 140, 128, 128, 128,
                                                  128, 128, 128, 128, 128, 128, 128, 128};
constexpr uint8_t sharpen_over_shoot[iso_bins] = {90, 80, 80, 75, 75, 75, 75, 75, 60, 60, 60, 60, 60, 60, 60, 60};
constexpr uint8_t sharpen_under_shoot[iso_bins] = {80, 80, 80, 70, 70, 70, 70, 70, 70, 70, 60, 60, 60, 60, 60, 60};
constexpr uint8_t sharpen_shoot_sup_adj[iso_bins] = {7, 7, 8, 8, 8, 8, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint16_t sharpen_max_gain[iso_bins] = {50, 40, 35, 30, 30, 26, 20, 20, 18, 18, 18, 18, 18, 18, 18, 18};
/* by luma bin, the same at every ISO */
constexpr uint8_t sharpen_luma_weight[luma_bins] = {0,  2,  3,  5,  7,  8,  10, 12, 14, 15, 17,
                                                    18, 20, 21, 22, 23, 24, 24, 25, 25, 26, 26,
                                                    26, 27, 27, 27, 27, 27, 27, 27, 27, 27};

inline uint8_t sharpen_detail(unsigned sharpness, unsigned bin)
{
    const int value = (int)sharpen_detail_base[bin] + (int)(level(sharpness) / 2U) - 64;
    return (uint8_t)(value < 0 ? 0 : value);
}

/* Three curves, linear between knots; 12-bit output. */
constexpr uint16_t gamma_knots[gamma_knot_count] = {0,   4,   8,   12,  16,  24,  32,  40,  48,  64,
                                                    80,  96,  112, 128, 160, 192, 224, 256, 320, 384,
                                                    448, 512, 576, 640, 704, 768, 832, 896, 960, 1024};
constexpr uint16_t gamma_curves[3][gamma_knot_count] = {
    {0,    34,   65,   96,   128,  202,  285,  365,  454,  678,  849,  1049, 1252, 1433, 1723,
     1967, 2173, 2349, 2638, 2898, 3040, 3168, 3246, 3308, 3371, 3448, 3554, 3693, 3884, 4095},
    {0,    54,   108,  166,  226,  327,  425,  510,  602,  809,  972,  1151, 1315, 1493, 1716,
     1939, 2115, 2268, 2527, 2743, 2936, 3092, 3230, 3355, 3474, 3608, 3738, 3862, 3981, 4095},
    {0,    38,   75,   112,  151,  238,  331,  440,  544,  719,  911,  1096, 1278, 1465, 1721,
     1965, 2156, 2328, 2517, 2694, 2829, 2952, 3084, 3222, 3379, 3534, 3664, 3793, 3960, 4095},
};

/* The curve a gamma setting selects, or -1 for 224–255, which the vendor
 * doesn't support: the curve then stays as it was. */
inline int gamma_curve(unsigned gamma)
{
    static constexpr int8_t curves[8] = {0, 1, 2, 1, 0, 1, 0, -1};
    return curves[level(gamma) >> 5];
}

inline void gamma_table(int curve, uint16_t *table)
{
    const uint16_t *y = gamma_curves[curve];
    for (unsigned k = 0; k + 1 < gamma_knot_count; k++) {
        const unsigned x0 = gamma_knots[k], x1 = gamma_knots[k + 1], width = x1 - x0;
        for (unsigned x = x0; x < x1; x++)
            table[x] = (uint16_t)(y[k] + ((unsigned)(y[k + 1] - y[k]) * (x - x0) + width / 2U) / width);
    }
    table[gamma_nodes - 1] = y[gamma_knot_count - 1];
}

/* colour tone cast gain in 1/256, 1.0–1.5x */
inline uint16_t cast_gain(unsigned style) { return (uint16_t)(level(style) / 2U + 256U); }

inline uint32_t at_least_1x(uint32_t gain) { return gain < 1024U ? 1024U : gain; }
inline uint32_t sys_gain_max(unsigned max_gain) { return level(max_gain) * 48U * 1024U + 1024U; }
inline uint32_t again_max(unsigned max_again) { return at_least_1x(level(max_again) * 768U); }
inline uint32_t ispd_gain_max(unsigned max_dgain) { return at_least_1x(level(max_dgain) * 1024U); }
/* manual gains go in whole steps of 1x */
inline uint32_t manual_gain(unsigned gain) { return at_least_1x(level(gain) / 4U * 1024U); }

inline uint32_t exposure_max_us(unsigned max_exposure) { return max_exposure == 0U ? 0U : 1000000U / max_exposure; }

/* Auto white balance calibration and colour matrices (sign-magnitude 8.8,
 * warmest last) for green and blue water. */
struct Water {
    uint16_t static_wb[4];
    int32_t curve[6];
    uint16_t temps[4];
    uint16_t ccm[4][9];
};
constexpr Water waters[2] = {
    {{469, 256, 256, 514},
     {107, -12, -160, 199298, 128, -148524},
     {6074, 4800, 3880, 2800},
     {{0x1c4, 0x80b3, 0x8011, 0x806f, 0x1f7, 0x8088, 0x4, 0x80db, 0x1d7},
      {0x1c9, 0x80c9, 0x0, 0x807b, 0x1ee, 0x8073, 0x8001, 0x811d, 0x21e},
      {0x23f, 0x8145, 0x6, 0x808d, 0x1b3, 0x8026, 0xf, 0x813d, 0x22e},
      {0x217, 0x80ff, 0x8018, 0x8092, 0x1bb, 0x8029, 0x8012, 0x825b, 0x36d}}},
    {{500, 256, 256, 504},
     {106, -5, -154, 268831, 128, -230203},
     {6074, 5086, 3759, 2364},
     {{0x1d2, 0x80c8, 0x800a, 0x8091, 0x226, 0x8095, 0x8001, 0x80f6, 0x1f7},
      {0x1d6, 0x80d0, 0x8006, 0x809d, 0x229, 0x808c, 0x3, 0x8116, 0x213},
      {0x1f8, 0x80fd, 0x5, 0x80aa, 0x214, 0x806a, 0x10, 0x816f, 0x25f},
      {0x236, 0x8131, 0x8005, 0x80c4, 0x1d3, 0x800f, 0x8005, 0x8318, 0x41d}}},
};

}  // namespace ca_hisi_tuning
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make -C camera_app hisi-image-test`
Expected: `test_hisi_tuning: ok`

- [ ] **Step 6: Commit**

```bash
git add camera_app/src/backends/hisi/image_tuning.h camera_app/tests/test_hisi_tuning.cpp camera_app/Makefile .gitignore
git commit -m "hisi: the 4K Cam Manager's image controls as ISP values"
```

---

### Task 2: The new `[image]` settings in camera-app

**Files:**
- Modify: `include/apcam/config.h` (enums after `enum ca_white_balance` at line ~99; fields after `unsigned wb_temp;` at line ~225)
- Modify: `include/apcam/APC_Config.h` (option arrays after `wb_options` at line ~160; fields after the `APCAM_HAVE_WB_TUNING` block ending at line ~311)
- Modify: `camera_app/src/config.cpp` (defaults after `config->wb_temp = ...` at line ~87; `IMAGE_FIELDS` at line ~543)
- Create: `camera_app/tests/test_hisi_config.cpp`
- Modify: `camera_app/Makefile` (`hisi-image-test` from Task 1), `.gitignore`

**Interfaces:**
- Produces: `enum ca_gain_mode { CA_GAIN_AUTO = 0, CA_GAIN_MANUAL = 1 }`, `enum ca_ae_priority { CA_AE_PRIOR_HIGHLIGHT = 0, CA_AE_PRIOR_LOWLIGHT = 1 }`, `enum ca_water { CA_WATER_GREEN = 0, CA_WATER_BLUE = 1 }`, and in `struct ca_config` (only `#if defined(APCAM_HAVE_4KCAM_COMPAT) && APCAM_HAVE_4KCAM_COMPAT`): `unsigned hue, sharpness, gamma, backlight, wb_style_red, wb_style_green, wb_style_blue; enum ca_water wb_water; unsigned max_gain; enum ca_gain_mode gain_mode; unsigned max_again, max_dgain; bool manual_again; unsigned again; bool manual_dgain; unsigned dgain; enum ca_ae_priority ae_strategy; unsigned max_exposure; bool slow_shutter;`. All are compared and copied by `ca_config_image_equal` / `ca_config_copy_image`.

- [ ] **Step 1: Write the failing test** `camera_app/tests/test_hisi_config.cpp`

```cpp
#include "camera_app/config.h"
#include "apcam/APC_Config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

int main()
{
    char path[] = "/tmp/camera-app-hisi-config-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    char error[160] = "";
    struct ca_config config;

    ca_config_defaults(&config);
    assert(config.hue == 128 && config.sharpness == 128 && config.gamma == 20 && config.backlight == 50);
    assert(config.wb_style_red == 0 && config.wb_style_green == 0 && config.wb_style_blue == 0);
    assert(config.wb_water == CA_WATER_GREEN && config.max_gain == 12 && config.gain_mode == CA_GAIN_AUTO);
    assert(config.max_again == 32 && config.max_dgain == 0 && !config.manual_again && config.again == 128);
    assert(!config.manual_dgain && config.dgain == 128 && config.ae_strategy == CA_AE_PRIOR_HIGHLIGHT);
    assert(config.max_exposure == 25 && !config.slow_shutter);

    write_file(path, "[image]\nhue = 200\nsharpness = 0\ngamma = 255\nbacklight = 90\n"
                     "wb_style_red = 10\nwb_style_green = 20\nwb_style_blue = 255\nwb_water = blue\n"
                     "max_gain = 3\ngain_mode = manual\nmax_again = 64\nmax_dgain = 8\n"
                     "manual_again = true\nagain = 40\nmanual_dgain = true\ndgain = 12\n"
                     "ae_strategy = lowlight\nmax_exposure = 8000\nslow_shutter = true\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.hue == 200 && config.sharpness == 0 && config.gamma == 255 && config.backlight == 90);
    assert(config.wb_style_red == 10 && config.wb_style_green == 20 && config.wb_style_blue == 255);
    assert(config.wb_water == CA_WATER_BLUE && config.max_gain == 3 && config.gain_mode == CA_GAIN_MANUAL);
    assert(config.max_again == 64 && config.max_dgain == 8 && config.manual_again && config.again == 40);
    assert(config.manual_dgain && config.dgain == 12 && config.ae_strategy == CA_AE_PRIOR_LOWLIGHT);
    assert(config.max_exposure == 8000 && config.slow_shutter);

    // live image settings: they compare and copy with the others
    struct ca_config other;
    ca_config_defaults(&other);
    assert(!ca_config_image_equal(&config, &other));
    ca_config_copy_image(&other, &config);
    assert(ca_config_image_equal(&config, &other));
    assert(other.slow_shutter && other.max_exposure == 8000 && other.wb_water == CA_WATER_BLUE);

    const APC_Config::Field *hue = APC_Config::find("image", "hue");
    assert(hue != NULL && hue->param_name == NULL);

    static const char *const invalid[] = {
        "[image]\nhue = 256\n",          "[image]\nmax_exposure = 11\n", "[image]\nmax_exposure = 8001\n",
        "[image]\ngain_mode = fixed\n",  "[image]\nwb_water = red\n",    "[image]\nae_strategy = middle\n",
        "[image]\nslow_shutter = maybe\n",
    };
    for (const char *text : invalid) {
        write_file(path, text);
        ca_config_defaults(&config);
        assert(ca_config_load(&config, path, error, sizeof(error)) < 0);
    }
    unlink(path);
    puts("test_hisi_config: ok");
    return 0;
}
```

- [ ] **Step 2: Add it to `hisi-image-test`**

In `camera_app/Makefile`, make the target from Task 1 read:

```make
.PHONY: hisi-image-test
hisi-image-test: tests/test_hisi_tuning tests/test_hisi_config
	./tests/test_hisi_tuning
	./tests/test_hisi_config
```

and add after the `tests/test_hisi_tuning` rule:

```make
tests/test_hisi_config: tests/test_hisi_config.cpp src/config.cpp include/camera_app/config.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) -I../include -Iinclude -DAPCAM_TARGET=APCAM_TARGET_HISI $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^) -lm
```

Add `/camera_app/tests/test_hisi_config` to `.gitignore` (and to `clean` if Task 1 added `test_hisi_tuning` there).

- [ ] **Step 3: Run it to verify it fails**

Run: `make -C camera_app hisi-image-test`
Expected: compile errors, `struct ca_config` has no member `hue`.

- [ ] **Step 4: Add the enums and fields to `include/apcam/config.h`**

After `enum ca_white_balance { ... };`:

```c
/* The 4K Cam Manager's gain mode, exposure priority and water colour. */
enum ca_gain_mode {
    CA_GAIN_AUTO = 0,
    CA_GAIN_MANUAL = 1,
};

enum ca_ae_priority {
    CA_AE_PRIOR_HIGHLIGHT = 0,
    CA_AE_PRIOR_LOWLIGHT = 1,
};

enum ca_water {
    CA_WATER_GREEN = 0,
    CA_WATER_BLUE = 1,
};
```

After `unsigned wb_temp;` in `struct ca_config`:

```c
#if defined(APCAM_HAVE_4KCAM_COMPAT) && APCAM_HAVE_4KCAM_COMPAT
    /* The 4K Cam Manager's image controls in its own units: levels 0–255,
     * max_exposure x in 1/x s (docs/4kcam-manager-compat.md). */
    unsigned hue;
    unsigned sharpness;
    unsigned gamma;
    unsigned backlight;
    unsigned wb_style_red;
    unsigned wb_style_green;
    unsigned wb_style_blue;
    enum ca_water wb_water;
    unsigned max_gain;
    enum ca_gain_mode gain_mode;
    unsigned max_again;
    unsigned max_dgain;
    bool manual_again;
    unsigned again;
    bool manual_dgain;
    unsigned dgain;
    enum ca_ae_priority ae_strategy;
    unsigned max_exposure;
    bool slow_shutter;
#endif
```

- [ ] **Step 5: Add the schema entries to `include/apcam/APC_Config.h`**

After the `wb_options` array:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
    inline static constexpr ca_config_option water_options[] = {
        {"green", CA_WATER_GREEN}, {"blue", CA_WATER_BLUE},
    };
    inline static constexpr ca_config_option gain_mode_options[] = {
        {"auto", CA_GAIN_AUTO}, {"manual", CA_GAIN_MANUAL},
    };
    inline static constexpr ca_config_option ae_strategy_options[] = {
        {"highlight", CA_AE_PRIOR_HIGHLIGHT}, {"lowlight", CA_AE_PRIOR_LOWLIGHT},
    };
#endif
```

After the `#if APCAM_HAVE_WB_TUNING ... #endif` block of `wb_red`…`wb_temp` fields:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
        {"image", "hue", Kind::Uint, offsetof(struct ca_config, hue),
         sizeof(((struct ca_config *)0)->hue), NULL, 0U, 0, 255, NULL},
        {"image", "sharpness", Kind::Uint, offsetof(struct ca_config, sharpness),
         sizeof(((struct ca_config *)0)->sharpness), NULL, 0U, 0, 255, NULL},
        {"image", "gamma", Kind::Uint, offsetof(struct ca_config, gamma),
         sizeof(((struct ca_config *)0)->gamma), NULL, 0U, 0, 255, NULL},
        {"image", "backlight", Kind::Uint, offsetof(struct ca_config, backlight),
         sizeof(((struct ca_config *)0)->backlight), NULL, 0U, 0, 255, NULL},
        {"image", "wb_style_red", Kind::Uint, offsetof(struct ca_config, wb_style_red),
         sizeof(((struct ca_config *)0)->wb_style_red), NULL, 0U, 0, 255, NULL},
        {"image", "wb_style_green", Kind::Uint, offsetof(struct ca_config, wb_style_green),
         sizeof(((struct ca_config *)0)->wb_style_green), NULL, 0U, 0, 255, NULL},
        {"image", "wb_style_blue", Kind::Uint, offsetof(struct ca_config, wb_style_blue),
         sizeof(((struct ca_config *)0)->wb_style_blue), NULL, 0U, 0, 255, NULL},
        {"image", "wb_water", Kind::Enum, offsetof(struct ca_config, wb_water),
         sizeof(((struct ca_config *)0)->wb_water), water_options,
         sizeof(water_options) / sizeof(water_options[0]), 0, 0, NULL},
        {"image", "max_gain", Kind::Uint, offsetof(struct ca_config, max_gain),
         sizeof(((struct ca_config *)0)->max_gain), NULL, 0U, 0, 255, NULL},
        {"image", "gain_mode", Kind::Enum, offsetof(struct ca_config, gain_mode),
         sizeof(((struct ca_config *)0)->gain_mode), gain_mode_options,
         sizeof(gain_mode_options) / sizeof(gain_mode_options[0]), 0, 0, NULL},
        {"image", "max_again", Kind::Uint, offsetof(struct ca_config, max_again),
         sizeof(((struct ca_config *)0)->max_again), NULL, 0U, 0, 255, NULL},
        {"image", "max_dgain", Kind::Uint, offsetof(struct ca_config, max_dgain),
         sizeof(((struct ca_config *)0)->max_dgain), NULL, 0U, 0, 255, NULL},
        {"image", "manual_again", Kind::Bool, offsetof(struct ca_config, manual_again),
         sizeof(((struct ca_config *)0)->manual_again), NULL, 0U, 0, 0, NULL},
        {"image", "again", Kind::Uint, offsetof(struct ca_config, again),
         sizeof(((struct ca_config *)0)->again), NULL, 0U, 0, 255, NULL},
        {"image", "manual_dgain", Kind::Bool, offsetof(struct ca_config, manual_dgain),
         sizeof(((struct ca_config *)0)->manual_dgain), NULL, 0U, 0, 0, NULL},
        {"image", "dgain", Kind::Uint, offsetof(struct ca_config, dgain),
         sizeof(((struct ca_config *)0)->dgain), NULL, 0U, 0, 255, NULL},
        {"image", "ae_strategy", Kind::Enum, offsetof(struct ca_config, ae_strategy),
         sizeof(((struct ca_config *)0)->ae_strategy), ae_strategy_options,
         sizeof(ae_strategy_options) / sizeof(ae_strategy_options[0]), 0, 0, NULL},
        {"image", "max_exposure", Kind::Uint, offsetof(struct ca_config, max_exposure),
         sizeof(((struct ca_config *)0)->max_exposure), NULL, 0U, 12, 8000, NULL},
        {"image", "slow_shutter", Kind::Bool, offsetof(struct ca_config, slow_shutter),
         sizeof(((struct ca_config *)0)->slow_shutter), NULL, 0U, 0, 0, NULL},
#endif
```

- [ ] **Step 6: Defaults and live comparison in `camera_app/src/config.cpp`**

After `config->wb_temp = APCAM_DEFAULT_WB_TEMP;`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
    config->hue = 128U;
    config->sharpness = 128U;
    config->gamma = 20U;
    config->backlight = 50U;
    config->wb_water = CA_WATER_GREEN;
    config->max_gain = 12U;
    config->gain_mode = CA_GAIN_AUTO;
    config->max_again = 32U;
    config->again = 128U;
    config->dgain = 128U;
    config->ae_strategy = CA_AE_PRIOR_HIGHLIGHT;
    config->max_exposure = 25U;
#endif
```

(`ca_config_defaults` starts with `memset(config, 0, ...)`, so the zero and false defaults need no line.)

Replace the `IMAGE_FIELDS` definition with:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
#define FOURKCAM_FIELDS(X) X(hue) X(sharpness) X(gamma) X(backlight) X(wb_style_red) X(wb_style_green) \
    X(wb_style_blue) X(wb_water) X(max_gain) X(gain_mode) X(max_again) X(max_dgain) X(manual_again) \
    X(again) X(manual_dgain) X(dgain) X(ae_strategy) X(max_exposure) X(slow_shutter)
#else
#define FOURKCAM_FIELDS(X)
#endif
#define IMAGE_FIELDS(X) X(brightness) X(saturation) X(contrast) X(exposure_compensation) \
    X(iso) X(shutter) X(metering) X(white_balance) X(wb_red) X(wb_green) X(wb_blue) \
    X(wb_temp) PHOTO_FIELDS(X) ENCODER_FIELDS(X) FOURKCAM_FIELDS(X)
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make -C camera_app hisi-image-test && make -C camera_app tests/test_config && ./camera_app/tests/test_config`
Expected: `test_hisi_tuning: ok`, `test_hisi_config: ok`, and `test_config` exits 0 (the default target is unchanged).

- [ ] **Step 8: Commit**

```bash
git add include/apcam/config.h include/apcam/APC_Config.h camera_app/src/config.cpp camera_app/tests/test_hisi_config.cpp camera_app/Makefile .gitignore
git commit -m "camera-app: the 4K Cam Manager's image controls as live [image] settings"
```

---

### Task 3: Apply them on the ISP (`hisi/isp.cpp`)

**Files:**
- Modify: `camera_app/src/backends/hisi/isp.cpp`
- Modify: `camera_app/src/backends/hisi/isp.h:9`
- Modify: `camera_app/src/backends/hisi/media.cpp:446` and `:760`

**Interfaces:**
- Consumes: everything `ca_hisi_tuning` produces (Task 1) and the `struct ca_config` fields and enums from Task 2.
- Produces: `td_s32 ca_hisi_apply_isp_config(const struct ca_config *config);` (the `bool live` parameter is removed: exposure is now applied at boot as well).

No host test can link the SDK; this task is verified by the cross build and, later, on the camera by the controller.

- [ ] **Step 1: Includes and build-time checks**

At the top of `isp.cpp`, add `#include "image_tuning.h"` after `#include "pipeline.h"`, and after the includes:

```cpp
static_assert(APCAM_HAVE_4KCAM_COMPAT, "the ISP applies the 4K Cam Manager's image settings");
static_assert(OT_ISP_AUTO_ISO_NUM == ca_hisi_tuning::iso_bins &&
              OT_ISP_SHARPEN_LUMA_NUM == ca_hisi_tuning::luma_bins &&
              OT_ISP_GAMMA_NODE_NUM == ca_hisi_tuning::gamma_nodes &&
              OT_ISP_CCM_MATRIX_SIZE == 9 && OT_ISP_CCM_MATRIX_NUM >= 4 &&
              OT_ISP_BAYER_CHN_NUM == 4 && OT_ISP_AWB_CURVE_PARA_NUM == 6,
              "SDK table sizes");
```

- [ ] **Step 2: Hue in `set_csc`**

Add after `attr.contr = (td_u8)config->contrast;`:

```cpp
    attr.hue = ca_hisi_tuning::csc_hue(config->hue);
```

- [ ] **Step 3: Replace `set_exposure`**

```cpp
static td_s32 set_exposure(ot_vi_pipe pipe, const struct ca_config *config)
{
    using namespace ca_hisi_tuning;
    static const td_u32 exposure_us[] = {
        0U, 33333U, 20000U, 10000U, 4000U, 2000U, 1333U, 1000U, 500U,
    };
    ot_isp_exposure_attr attr;
    td_s32 result = ss_mpi_isp_get_exposure_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;

    // a fixed ISO fixes every gain, whatever the gain mode
    const bool manual_gains = config->iso == CA_ISO_AUTO && config->gain_mode == CA_GAIN_MANUAL;
    attr.op_type = config->iso != CA_ISO_AUTO || config->shutter != CA_SHUTTER_AUTO || manual_gains
        ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    attr.auto_attr.ev_bias = ev_bias(config->exposure_compensation);
    attr.auto_attr.compensation = (td_u8)config->backlight;
    attr.auto_attr.sys_gain_range.max = sys_gain_max(config->max_gain);
    attr.auto_attr.a_gain_range.max = again_max(config->max_again);
    attr.auto_attr.ispd_gain_range.max = ispd_gain_max(config->max_dgain);
    attr.auto_attr.ae_strategy_mode = config->ae_strategy == CA_AE_PRIOR_LOWLIGHT
        ? OT_ISP_AE_EXP_LOWLIGHT_PRIOR : OT_ISP_AE_EXP_HIGHLIGHT_PRIOR;
    attr.auto_attr.ae_mode = config->slow_shutter ? OT_ISP_AE_MODE_SLOW_SHUTTER : OT_ISP_AE_MODE_FIX_FRAME_RATE;
    if (config->iso == CA_ISO_AUTO) {
        const bool again = manual_gains && config->manual_again;
        const bool dgain = manual_gains && config->manual_dgain;
        attr.manual_attr.a_gain_op_type = again ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
        attr.manual_attr.d_gain_op_type = dgain ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
        attr.manual_attr.ispd_gain_op_type = dgain ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
        attr.manual_attr.a_gain = manual_gain(config->again);
        attr.manual_attr.d_gain = 1024U;
        attr.manual_attr.isp_d_gain = manual_gain(config->dgain);
    } else {
        td_u32 gain = 1024U << ((unsigned)config->iso - 1U);
        attr.manual_attr.a_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.d_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.a_gain = gain;
        attr.manual_attr.d_gain = 1024U;
        attr.manual_attr.isp_d_gain = 1024U;
    }
    if (config->shutter == CA_SHUTTER_AUTO) {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_AUTO;
        attr.auto_attr.exp_time_range.max = exposure_max_us(config->max_exposure);
    } else {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.exp_time = exposure_us[config->shutter];
    }
    return ss_mpi_isp_set_exposure_attr(pipe, &attr);
}
```

- [ ] **Step 4: White balance calibration by water colour**

Delete `calibrated_static_wb`, `calibrated_curve` and the comment above them. In `set_white_balance`, replace the two `memcpy` lines with:

```cpp
    const ca_hisi_tuning::Water &water = ca_hisi_tuning::waters[config->wb_water == CA_WATER_BLUE ? 1 : 0];
    for (unsigned i = 0; i < OT_ISP_BAYER_CHN_NUM; i++) attr.auto_attr.static_wb[i] = water.static_wb[i];
    for (unsigned i = 0; i < OT_ISP_AWB_CURVE_PARA_NUM; i++) attr.auto_attr.curve_para[i] = water.curve[i];
```

- [ ] **Step 5: The tuning setters**

Add before `ca_hisi_apply_isp_config`:

```cpp
static td_s32 set_colour_matrices(ot_vi_pipe pipe, enum ca_water colour)
{
    const ca_hisi_tuning::Water &water = ca_hisi_tuning::waters[colour == CA_WATER_BLUE ? 1 : 0];
    ot_isp_color_matrix_attr attr;
    td_s32 result = ss_mpi_isp_get_ccm_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.op_type = OT_OP_MODE_AUTO;
    attr.auto_attr.ccm_tab_num = 4U;
    for (unsigned t = 0; t < 4U; t++) {
        attr.auto_attr.ccm_tab[t].color_temp = water.temps[t];
        for (unsigned i = 0; i < OT_ISP_CCM_MATRIX_SIZE; i++) attr.auto_attr.ccm_tab[t].ccm[i] = water.ccm[t][i];
    }
    for (unsigned i = 0; i < OT_ISP_CCM_MATRIX_SIZE; i++) attr.manual_attr.ccm[i] = water.ccm[1][i];
    return ss_mpi_isp_set_ccm_attr(pipe, &attr);
}

static td_s32 set_colour_tone(ot_vi_pipe pipe, const struct ca_config *config)
{
    const ot_isp_color_tone_attr attr = {ca_hisi_tuning::cast_gain(config->wb_style_red),
                                         ca_hisi_tuning::cast_gain(config->wb_style_green),
                                         ca_hisi_tuning::cast_gain(config->wb_style_blue)};
    return ss_mpi_isp_set_color_tone_attr(pipe, &attr);
}

static td_s32 set_gamma(ot_vi_pipe pipe, unsigned gamma)
{
    const int curve = ca_hisi_tuning::gamma_curve(gamma);
    if (curve < 0) return TD_SUCCESS;
    static ot_isp_gamma_attr attr;  // 2 KB, called under the ISP lock
    td_s32 result = ss_mpi_isp_get_gamma_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.enable = TD_TRUE;
    attr.curve_type = OT_ISP_GAMMA_CURVE_USER_DEFINE;
    ca_hisi_tuning::gamma_table(curve, attr.table);
    return ss_mpi_isp_set_gamma_attr(pipe, &attr);
}

static td_s32 set_sharpen(ot_vi_pipe pipe, unsigned sharpness)
{
    using namespace ca_hisi_tuning;
    static ot_isp_sharpen_attr attr;  // about 17 KB, called under the ISP lock
    td_s32 result = ss_mpi_isp_get_sharpen_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.enable = TD_TRUE;
    for (unsigned bin = 0; bin < iso_bins; bin++) {
        attr.auto_attr.texture_freq[bin] = sharpen_texture_freq[bin];
        attr.auto_attr.edge_freq[bin] = sharpen_edge_freq[bin];
        attr.auto_attr.over_shoot[bin] = sharpen_over_shoot[bin];
        attr.auto_attr.under_shoot[bin] = sharpen_under_shoot[bin];
        attr.auto_attr.shoot_sup_strength[bin] = 0U;
        attr.auto_attr.shoot_sup_adj[bin] = sharpen_shoot_sup_adj[bin];
        attr.auto_attr.max_sharp_gain[bin] = sharpen_max_gain[bin];
        attr.auto_attr.detail_ctrl[bin] = sharpen_detail(sharpness, bin);
        for (unsigned luma = 0; luma < luma_bins; luma++)
            attr.auto_attr.luma_wgt[luma][bin] = sharpen_luma_weight[luma];
    }
    return ss_mpi_isp_set_sharpen_attr(pipe, &attr);
}

/* The picture's look: a value the SDK refuses keeps the previous one and is
 * logged, rather than stopping the video. */
static void set_tuning(ot_vi_pipe pipe, const struct ca_config *config)
{
    const struct {
        const char *name;
        td_s32 result;
    } steps[] = {
        {"colour matrices", set_colour_matrices(pipe, config->wb_water)},
        {"colour tone", set_colour_tone(pipe, config)},
        {"gamma", set_gamma(pipe, config->gamma)},
        {"sharpening", set_sharpen(pipe, config->sharpness)},
    };
    for (const auto &step : steps)
        if (step.result != TD_SUCCESS) ca_log("ISP %s failed: 0x%x", step.name, (unsigned)step.result);
}
```

- [ ] **Step 6: Replace `ca_hisi_apply_isp_config`**

```cpp
td_s32 ca_hisi_apply_isp_config(const struct ca_config *config)
{
    const ot_vi_pipe pipe = CA_HISI_PIPE;
    td_s32 result;

    if (config == NULL) return TD_FAILURE;
    if ((result = set_csc(pipe, config)) != TD_SUCCESS ||
        (result = set_exposure(pipe, config)) != TD_SUCCESS ||
        (result = set_metering(pipe, config->metering)) != TD_SUCCESS ||
        (result = set_white_balance(pipe, config)) != TD_SUCCESS) {
        ca_log("ISP configuration failed for pipe %d: 0x%x", pipe, (unsigned)result);
        return result;
    }
    set_tuning(pipe, config);
    ca_log("ISP configured brightness=%d saturation=%d contrast=%d ev=%d "
           "iso=%d shutter=%d metering=%d white_balance=%d (R %u G %u B %u, %u K)",
           config->brightness, config->saturation, config->contrast,
           config->exposure_compensation, config->iso, config->shutter,
           config->metering, config->white_balance, config->wb_red, config->wb_green,
           config->wb_blue, config->wb_temp);
    ca_log("ISP tuning hue=%u sharpness=%u gamma=%u backlight=%u style=%u/%u/%u water=%s "
           "max_gain=%u gain_mode=%s max_again=%u max_dgain=%u again=%s:%u dgain=%s:%u "
           "ae=%s max_exposure=1/%u slow_shutter=%s",
           config->hue, config->sharpness, config->gamma, config->backlight, config->wb_style_red,
           config->wb_style_green, config->wb_style_blue, config->wb_water == CA_WATER_BLUE ? "blue" : "green",
           config->max_gain, config->gain_mode == CA_GAIN_MANUAL ? "manual" : "auto", config->max_again,
           config->max_dgain, config->manual_again ? "on" : "off", config->again,
           config->manual_dgain ? "on" : "off", config->dgain,
           config->ae_strategy == CA_AE_PRIOR_LOWLIGHT ? "lowlight" : "highlight", config->max_exposure,
           config->slow_shutter ? "on" : "off");
    return TD_SUCCESS;
}
```

- [ ] **Step 7: Drop the `live` parameter**

In `isp.h`: `td_s32 ca_hisi_apply_isp_config(const struct ca_config *config);`. In `media.cpp`, change `ca_hisi_apply_isp_config(settings, false)` and `ca_hisi_apply_isp_config(settings, true)` to `ca_hisi_apply_isp_config(settings)`.

- [ ] **Step 8: Cross-build to verify it compiles**

Run:
```bash
X=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux-
(cd camera_app && make CAMERA_BACKEND=hisi CROSS_COMPILE=$X -j8 2>&1 | tail -20)
```
Expected: links `camera_app/build/hisi/camera-app` with no warnings or errors. Also rerun `make -C camera_app hisi-image-test` (still passes).

- [ ] **Step 9: Commit**

```bash
git add camera_app/src/backends/hisi/isp.cpp camera_app/src/backends/hisi/isp.h camera_app/src/backends/hisi/media.cpp
git commit -m "hisi: apply the 4K Cam Manager's image controls the way the vendor firmware does"
```

---

### Task 4: The new settings on the web Settings page

**Files:**
- Modify: `web/APC_Translations.h` (enum `string_id` entries and `result[...]` strings, beside the existing `S_P_WB_WATER`, `S_H_WB_WATER`, `S_OPT_WATER_*`)
- Modify: `web/mt11-web.cpp` (option arrays after `wb_water_options` at line ~653; `replacement_parameters` after the `wb_water` entry at line ~781; `replacement_defaults` after `"green",` at line ~909)

**Interfaces:**
- Produces: web parameters with section `image` and keys exactly as Task 2 (`hue` … `slow_shutter`), whose defaults `default_value("image", key)` returns (Task 5 relies on this for `set_default` and for reading absent keys).

- [ ] **Step 1: Strings**

Add these ids to `enum string_id` next to the existing ones of the same prefix (`S_OPT_*` with the options, `S_P_*` with the parameter names, `S_H_*` with the help texts): `S_OPT_MANUAL, S_OPT_AE_HIGHLIGHT, S_OPT_AE_LOWLIGHT, S_P_HUE, S_P_SHARPNESS, S_P_GAMMA, S_P_BACKLIGHT, S_P_WB_STYLE_RED, S_P_WB_STYLE_GREEN, S_P_WB_STYLE_BLUE, S_P_MAX_GAIN, S_P_GAIN_MODE, S_P_MAX_AGAIN, S_P_MAX_DGAIN, S_P_MANUAL_AGAIN, S_P_AGAIN, S_P_MANUAL_DGAIN, S_P_DGAIN, S_P_AE_STRATEGY, S_P_MAX_EXPOSURE, S_P_SLOW_SHUTTER, S_H_FOURKCAM_LEVEL, S_H_GAMMA, S_H_BACKLIGHT, S_H_WB_STYLE, S_H_MAX_GAIN, S_H_GAIN_MODE, S_H_MAX_AGAIN, S_H_MAX_DGAIN, S_H_MANUAL_GAIN, S_H_GAIN_VALUE, S_H_AE_STRATEGY, S_H_MAX_EXPOSURE, S_H_SLOW_SHUTTER`.

Add their text (English, Chinese, Japanese), each beside its group:

```cpp
    result[S_OPT_MANUAL] = {"Manual", "手动", "手動"};
    result[S_OPT_AE_HIGHLIGHT] = {"Highlights", "高光优先", "ハイライト優先"};
    result[S_OPT_AE_LOWLIGHT] = {"Low light", "低照度优先", "低照度優先"};
    result[S_P_HUE] = {"Hue", "色调", "色相"};
    result[S_P_SHARPNESS] = {"Sharpness", "锐度", "シャープネス"};
    result[S_P_GAMMA] = {"Gamma", "伽马", "ガンマ"};
    result[S_P_BACKLIGHT] = {"Backlight compensation", "背光补偿", "逆光補正"};
    result[S_P_WB_STYLE_RED] = {"White balance style red", "白平衡风格红", "ホワイトバランス スタイル 赤"};
    result[S_P_WB_STYLE_GREEN] = {"White balance style green", "白平衡风格绿", "ホワイトバランス スタイル 緑"};
    result[S_P_WB_STYLE_BLUE] = {"White balance style blue", "白平衡风格蓝", "ホワイトバランス スタイル 青"};
    result[S_P_MAX_GAIN] = {"Maximum system gain", "最大系统增益", "最大システムゲイン"};
    result[S_P_GAIN_MODE] = {"Gain mode", "增益模式", "ゲインモード"};
    result[S_P_MAX_AGAIN] = {"Maximum analog gain", "最大模拟增益", "最大アナログゲイン"};
    result[S_P_MAX_DGAIN] = {"Maximum digital gain", "最大数字增益", "最大デジタルゲイン"};
    result[S_P_MANUAL_AGAIN] = {"Manual analog gain", "手动模拟增益", "手動アナログゲイン"};
    result[S_P_AGAIN] = {"Analog gain", "模拟增益", "アナログゲイン"};
    result[S_P_MANUAL_DGAIN] = {"Manual digital gain", "手动数字增益", "手動デジタルゲイン"};
    result[S_P_DGAIN] = {"Digital gain", "数字增益", "デジタルゲイン"};
    result[S_P_AE_STRATEGY] = {"Exposure priority", "曝光优先", "露出優先"};
    result[S_P_MAX_EXPOSURE] = {"Maximum exposure", "最长曝光", "最大露出時間"};
    result[S_P_SLOW_SHUTTER] = {"Slow shutter", "慢快门", "スローシャッター"};
    result[S_H_FOURKCAM_LEVEL] = {"0–255, as 4K Cam Manager sets it; 128 is the default.",
                                  "0–255，与 4K Cam Manager 的设置相同；默认 128。",
                                  "0–255。4K Cam Manager と同じ値で、既定値は 128 です。"};
    result[S_H_GAMMA] = {"Tone curve, 0–255 in steps of 32: 0–31, 128–159 and 192–223 give the default curve, "
                         "32–63, 96–127 and 160–191 lift the shadows most, 64–95 a little; 224–255 keep the current curve.",
                         "色调曲线，0–255，每 32 为一档：0–31、128–159、192–223 为默认曲线；32–63、96–127、160–191 "
                         "暗部提亮最多；64–95 稍微提亮；224–255 保持当前曲线。",
                         "トーンカーブ。0–255 を 32 刻みで選びます。0–31、128–159、192–223 は既定のカーブ、32–63、"
                         "96–127、160–191 は暗部を最も持ち上げ、64–95 は少し持ち上げます。224–255 は現在のカーブのままです。"};
    result[S_H_BACKLIGHT] = {"Brightness that automatic exposure aims for, 0–255 (default 50).",
                             "自动曝光的目标亮度，0–255（默认 50）。",
                             "自動露出が目標とする明るさ。0–255（既定値 50）。"};
    result[S_H_WB_STYLE] = {"Extra gain on this colour after colour correction: 0 is none, 255 is 1.5×.",
                            "色彩校正后对该颜色的额外增益：0 为无，255 为 1.5 倍。",
                            "色補正後にこの色へ加えるゲイン。0 はなし、255 は 1.5 倍です。"};
    result[S_H_MAX_GAIN] = {"Limit on the total automatic gain: (48 × value + 1)×.",
                            "自动总增益上限：(48 × 数值 + 1) 倍。",
                            "自動総ゲインの上限：(48 × 値 + 1) 倍。"};
    result[S_H_GAIN_MODE] = {"Manual lets the manual analog and digital gain settings fix those gains. A fixed ISO "
                             "overrides both.",
                             "手动时，可由手动模拟增益和手动数字增益固定相应增益。固定 ISO 优先于两者。",
                             "手動にすると、手動アナログゲインと手動デジタルゲインでそれぞれのゲインを固定できます。ISO を固定すると両方より優先されます。"};
    result[S_H_MAX_AGAIN] = {"Limit on automatic analog gain: 0.75 × value, at least 1×.",
                             "自动模拟增益上限：0.75 × 数值，至少 1 倍。",
                             "自動アナログゲインの上限：0.75 × 値、最低 1 倍。"};
    result[S_H_MAX_DGAIN] = {"Limit on automatic ISP digital gain: value ×, at least 1×.",
                             "自动 ISP 数字增益上限：数值倍，至少 1 倍。",
                             "自動 ISP デジタルゲインの上限：値の倍数、最低 1 倍。"};
    result[S_H_MANUAL_GAIN] = {"With gain mode manual, fix this gain at the value below.",
                               "增益模式为手动时，将此增益固定为下方的数值。",
                               "ゲインモードが手動のとき、このゲインを下の値に固定します。"};
    result[S_H_GAIN_VALUE] = {"Fixed gain when enabled: value ÷ 4 in whole steps, at least 1×.",
                              "启用时的固定增益：数值 ÷ 4（取整），至少 1 倍。",
                              "有効時の固定ゲイン：値 ÷ 4（整数）、最低 1 倍。"};
    result[S_H_AE_STRATEGY] = {"Which part of the picture automatic exposure keeps visible.",
                               "自动曝光优先保留画面的哪一部分。",
                               "自動露出が優先して見えるようにする部分。"};
    result[S_H_MAX_EXPOSURE] = {"Longest automatic exposure, as x in 1/x s (12–8000). Exposures longer than a frame "
                                "need slow shutter.",
                                "自动曝光的最长时间，以 1/x 秒中的 x 表示（12–8000）。长于一帧的曝光需要开启慢快门。",
                                "自動露出の最長時間。1/x 秒の x で指定します（12–8000）。1 フレームより長い露出にはスローシャッターが必要です。"};
    result[S_H_SLOW_SHUTTER] = {"Let automatic exposure lower the frame rate in the dark, up to the maximum exposure.",
                                "允许自动曝光在暗处降低帧率，最长到最长曝光。",
                                "暗い場所で自動露出がフレームレートを下げられるようにします（最大露出時間まで）。"};
```

Replace the existing `S_H_WB_WATER` text with:

```cpp
    result[S_H_WB_WATER] = {"Water that auto white balance and 4K Cam Manager's one-push are tuned for. The Photos page asks each time.",
                            "自动白平衡和 4K Cam Manager 一键白平衡所针对的水色。照片页面每次单独选择。",
                            "自動ホワイトバランスと 4K Cam Manager のワンプッシュが対象とする水の色。写真ページでは毎回選びます。"};
```

- [ ] **Step 2: Options, parameters and defaults in `web/mt11-web.cpp`**

Inside the existing `#if APCAM_HAVE_4KCAM_COMPAT` block with `wb_water_options`, add:

```cpp
static const struct option gain_mode_options[] = {
    {"auto", S_OPT_AUTO}, {"manual", S_OPT_MANUAL},
};
static const struct option ae_strategy_options[] = {
    {"highlight", S_OPT_AE_HIGHLIGHT}, {"lowlight", S_OPT_AE_LOWLIGHT},
};
```

In `replacement_parameters`, directly after the `wb_water` entry (before `fourkcam_manager`):

```cpp
    {"hue", "image", "hue", S_P_HUE, S_H_FOURKCAM_LEVEL, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"sharpness", "image", "sharpness", S_P_SHARPNESS, S_H_FOURKCAM_LEVEL, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"gamma", "image", "gamma", S_P_GAMMA, S_H_GAMMA, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"backlight", "image", "backlight", S_P_BACKLIGHT, S_H_BACKLIGHT, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"wb_style_red", "image", "wb_style_red", S_P_WB_STYLE_RED, S_H_WB_STYLE, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"wb_style_green", "image", "wb_style_green", S_P_WB_STYLE_GREEN, S_H_WB_STYLE, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"wb_style_blue", "image", "wb_style_blue", S_P_WB_STYLE_BLUE, S_H_WB_STYLE, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"max_gain", "image", "max_gain", S_P_MAX_GAIN, S_H_MAX_GAIN, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"gain_mode", "image", "gain_mode", S_P_GAIN_MODE, S_H_GAIN_MODE, PARAM_ENUM, 0, 0, 0, gain_mode_options, 2},
    {"max_again", "image", "max_again", S_P_MAX_AGAIN, S_H_MAX_AGAIN, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"max_dgain", "image", "max_dgain", S_P_MAX_DGAIN, S_H_MAX_DGAIN, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"manual_again", "image", "manual_again", S_P_MANUAL_AGAIN, S_H_MANUAL_GAIN,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"again", "image", "again", S_P_AGAIN, S_H_GAIN_VALUE, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"manual_dgain", "image", "manual_dgain", S_P_MANUAL_DGAIN, S_H_MANUAL_GAIN,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"dgain", "image", "dgain", S_P_DGAIN, S_H_GAIN_VALUE, PARAM_INTEGER, 0, 255, 1, NULL, 0},
    {"ae_strategy", "image", "ae_strategy", S_P_AE_STRATEGY, S_H_AE_STRATEGY,
     PARAM_ENUM, 0, 0, 0, ae_strategy_options, 2},
    {"max_exposure", "image", "max_exposure", S_P_MAX_EXPOSURE, S_H_MAX_EXPOSURE, PARAM_INTEGER, 12, 8000, 1, NULL, 0},
    {"slow_shutter", "image", "slow_shutter", S_P_SLOW_SHUTTER, S_H_SLOW_SHUTTER,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
```

In `replacement_defaults`, the `#if APCAM_HAVE_4KCAM_COMPAT` line becomes (same order as the table):

```cpp
    "green", "128", "128", "20", "50", "0", "0", "0", "12", "auto", "32", "0", "false", "128", "false", "128",
    "highlight", "25", "false", "true", "blue",
```

- [ ] **Step 3: Build and test**

Run:
```bash
X=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux-
make -C web CAMERA_BACKEND=hisi HISI_CROSS_COMPILE=$X hisi-web 2>&1 | tail -5
make -C web tests/test_fourkcam tests/test_webroot && ./web/tests/test_fourkcam && ./web/tests/test_webroot
make -C web mt11-web 2>&1 | tail -3
```
Expected: `hisi-web` links (the `static_assert` that `replacement_defaults` matches `replacement_parameters` holds); the tests print ok; the default `mt11-web` still builds (none of this is compiled there).

- [ ] **Step 4: Commit**

```bash
git add web/APC_Translations.h web/mt11-web.cpp
git commit -m "web: the 4K Cam Manager's image controls on the Settings page"
```

---

### Task 5: Map the extension's fields (`APC_FourKCam.h`)

**Files:**
- Modify: `web/APC_FourKCam.h`
- Modify: `web/mt11-web.cpp` (`fourkcam_image` at line ~4812)
- Modify: `web/tests/test_fourkcam.cpp`
- Modify: `web/tests/check_4kcam_compat.py`
- Modify: `docs/4kcam-manager-compat.md`

**Interfaces:**
- Consumes: Task 4's web defaults for every new key (via `effective_value` / `default_value`).
- Produces, in namespace `apc_fourkcam`: `vendor_max_exposures[]`; `struct Level { const char *field; const char *key; long Image::*value; }` and `raw_levels[]`; `struct Switch { const char *field; const char *key; bool Image::*value; const char *off; const char *on; }`, `switches[]` and `ex_switches[]`; new `Image` members; `Changes::updates[32]`.

- [ ] **Step 1: Write the failing tests** in `web/tests/test_fourkcam.cpp`

Add after `parse`:

```cpp
static bool parse_wide(const char *text, Field *fields, size_t *count)
{
    return parse_object(text, strlen(text), fields, 64, count);
}
```

Add this test function before `main` and call it from `main` after `test_echo();`:

```cpp
static void test_vendor_image_fields()
{
    Field fields[64];
    size_t count;
    Image image = test_image();

    // reads report the stored values in the extension's types
    APC_StringBuffer out;
    image.hue = 300;
    image.lowlight = true;
    image.max_exposure = 500;
    image_adjustment(out, image);
    const char *text = out.data();
    assert(out.valid() && strstr(text, "\"hue\":255,") && strstr(text, "\"sharpness\":128,"));
    assert(strstr(text, "\"gamma\":20,") && strstr(text, "\"blc_level\":50,") && strstr(text, "\"max_sys_gain\":12,"));
    assert(strstr(text, "\"auto_AGain_max\":32,") && strstr(text, "\"auto_DGain_max\":0,"));
    assert(strstr(text, "\"awb_style_red\":0,") && strstr(text, "\"manual_AGain\":128,") && strstr(text, "\"manual_DGain\":128,"));
    assert(strstr(text, "\"auto_gain_mode\":0,") && strstr(text, "\"manual_AGain_enable\":0,"));
    assert(strstr(text, "\"manual_DGain_enable\":0,") && strstr(text, "\"max_exposure\":400,"));
    const char *strategy = strstr(text, "\"AE_strategy_mode\":1,");
    assert(strategy != NULL && strstr(strategy + 1, "\"AE_strategy_mode\"") == NULL);

    // levels are saved as sent, clamped to 0–255
    image = test_image();
    Changes changes;
    assert(parse("{\"hue\":200,\"blc_level\":60,\"awb_style_blue\":999,\"manual_DGain\":-5,\"sharpness\":128}",
                 fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 4 && !strcmp(change(changes, "hue")->value, "200"));
    assert(!strcmp(change(changes, "backlight")->value, "60") && !strcmp(change(changes, "wb_style_blue")->value, "255"));
    assert(!strcmp(change(changes, "dgain")->value, "0"));

    // switches count only when 0 or 1
    changes = Changes();
    assert(parse("{\"auto_gain_mode\":1,\"manual_AGain_enable\":1,\"manual_DGain_enable\":0,\"AE_strategy_mode\":1}",
                 fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 3 && !strcmp(change(changes, "gain_mode")->value, "manual"));
    assert(!strcmp(change(changes, "manual_again")->value, "true"));
    assert(!strcmp(change(changes, "ae_strategy")->value, "lowlight"));
    changes = Changes();
    assert(parse("{\"auto_gain_mode\":2,\"AE_strategy_mode\":-1,\"manual_AGain_enable\":7}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);

    // max_exposure takes the extension's values
    changes = Changes();
    assert(parse("{\"max_exposure\":30}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "max_exposure")->value, "30"));
    changes = Changes();
    assert(parse("{\"max_exposure\":3}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "max_exposure")->value, "12"));
    changes = Changes();
    image.max_exposure = 500;
    assert(parse("{\"max_exposure\":400}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"max_exposure\":8000}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "max_exposure")->value, "8000"));

    // slow shutter is an advanced field
    image = test_image();
    changes = Changes();
    assert(parse("{\"low_farme_rate\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(change(changes, "slow_shutter")->value, "true"));
    image.slow_shutter = true;
    changes = Changes();
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 0);
    out.reset();
    image_adjustment_ex(out, image);
    assert(strstr(out.data(), "\"low_farme_rate\":1,"));

    // a whole reply with every new field set echoes back unchanged
    image = test_image();
    image.sharpness = 7;
    image.wb_style_green = 99;
    image.manual_gain = true;
    image.manual_dgain = true;
    image.max_exposure = 500;
    out.reset();
    out.append("{");
    image_adjustment(out, image);
    finish(out, 0, "", {"aa-bb-cc-dd-ee-ff", "AABBCCDDEEFF", "192.168.2.10"});
    changes = Changes();
    assert(out.valid() && parse_object(out.data(), out.size(), fields, 64, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
}
```

In `test_reset`, change `Field fields[16];` to `Field fields[64];`, and replace the request that touches every key with:

```cpp
    assert(parse_wide("{\"brightness\":0,\"contrast\":0,\"saturation\":0,\"awb_red\":1,\"awb_green\":1,\"awb_blue\":1,"
                      "\"awb_auto_mode\":1,\"auto_exposureEx\":1,\"exposure_time\":30,\"hue\":1,\"sharpness\":1,"
                      "\"gamma\":1,\"blc_level\":1,\"awb_style_red\":1,\"awb_style_green\":1,\"awb_style_blue\":1,"
                      "\"max_sys_gain\":1,\"auto_gain_mode\":1,\"auto_AGain_max\":1,\"auto_DGain_max\":1,"
                      "\"manual_AGain_enable\":1,\"manual_AGain\":1,\"manual_DGain_enable\":1,\"manual_DGain\":1,"
                      "\"AE_strategy_mode\":1,\"max_exposure\":12}", fields, &count));
```

(the first four `assert(parse(...))` lines of `test_reset` keep using `parse`; they fit in 64 too.)

In `test_echo`, the two `Field fields[16];` become `Field fields[64];` and both `parse_object(..., fields, 16, &count)` become `parse_object(..., fields, 64, &count)`; change its comment to `/* A client echoing a whole reply back parses at the server's capacity (64) and changes nothing. */`.

- [ ] **Step 2: Run them to verify they fail**

Run: `make -C web tests/test_fourkcam && ./web/tests/test_fourkcam`
Expected: compile errors (`Image` has no member `hue`, `lowlight`, `max_exposure`, `slow_shutter`).

- [ ] **Step 3: Implement in `web/APC_FourKCam.h`**

After `constexpr unsigned shutters[] = ...;`:

```cpp
/* [image] max_exposure as the extension's BaseMaxExposureValue, 1/x s. */
constexpr unsigned vendor_max_exposures[] = {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000};
```

Add to `struct Image`, after `bool inverted = false;`:

```cpp
    long hue = 128;
    long sharpness = 128;
    long gamma = 20;
    long backlight = 50;
    long wb_style_red = 0;
    long wb_style_green = 0;
    long wb_style_blue = 0;
    long max_gain = 12;
    long max_again = 32;
    long max_dgain = 0;
    long again = 128;
    long dgain = 128;
    bool manual_gain = false;
    bool manual_again = false;
    bool manual_dgain = false;
    bool lowlight = false;
    long max_exposure = 25;
    bool slow_shutter = false;
```

After `struct Image`:

```cpp
/* Vendor 0–255 fields kept as the [image] key of the same value. */
struct Level {
    const char *field;
    const char *key;
    long Image::*value;
};
constexpr Level raw_levels[] = {
    {"hue", "hue", &Image::hue},
    {"sharpness", "sharpness", &Image::sharpness},
    {"gamma", "gamma", &Image::gamma},
    {"blc_level", "backlight", &Image::backlight},
    {"awb_style_red", "wb_style_red", &Image::wb_style_red},
    {"awb_style_green", "wb_style_green", &Image::wb_style_green},
    {"awb_style_blue", "wb_style_blue", &Image::wb_style_blue},
    {"max_sys_gain", "max_gain", &Image::max_gain},
    {"auto_AGain_max", "max_again", &Image::max_again},
    {"auto_DGain_max", "max_dgain", &Image::max_dgain},
    {"manual_AGain", "again", &Image::again},
    {"manual_DGain", "dgain", &Image::dgain},
};

/* Vendor 0/1 fields kept as an [image] key with two values. */
struct Switch {
    const char *field;
    const char *key;
    bool Image::*value;
    const char *off;
    const char *on;
};
constexpr Switch switches[] = {
    {"auto_gain_mode", "gain_mode", &Image::manual_gain, "auto", "manual"},
    {"manual_AGain_enable", "manual_again", &Image::manual_again, "false", "true"},
    {"manual_DGain_enable", "manual_dgain", &Image::manual_dgain, "false", "true"},
    {"AE_strategy_mode", "ae_strategy", &Image::lowlight, "highlight", "lowlight"},
};
/* setImageAdjustmentEx's */
constexpr Switch ex_switches[] = {
    {"low_farme_rate", "slow_shutter", &Image::slow_shutter, "false", "true"},
};

template <size_t N>
inline void append_switches(APC_StringBuffer &out, const Image &image, const Switch (&list)[N])
{
    for (const Switch &item : list) out.appendf("\"%s\":%d,", item.field, image.*item.value ? 1 : 0);
}
```

Change `Changes::updates[16]` to `updates[32]`.

Replace `image_adjustment` and `image_adjustment_ex`:

```cpp
inline void image_adjustment(APC_StringBuffer &out, const Image &image)
{
    out.appendf("\"brightness\":%ld,\"contrast\":%ld,\"saturation\":%ld,"
                "\"auto_exposureEx\":%d,\"exposure_time\":%u,\"max_exposure\":%u,"
                "\"auto_awb\":%d,\"awb_red\":%ld,\"awb_green\":%ld,\"awb_blue\":%ld,\"awb_auto_mode\":%d,"
                "\"antiFog\":0,\"frameTurbo_pro\":0,\"rotate\":0,",
                level_from_percent(image.brightness), level_from_percent(image.contrast),
                level_from_percent(image.saturation), strcmp(image.shutter, "auto") != 0,
                exposure_time(image.shutter), nearest(image.max_exposure, vendor_max_exposures),
                strcmp(image.white_balance, "auto") != 0,
                red_level(image.wb_red), green_blue_level(image.wb_green), green_blue_level(image.wb_blue),
                image.blue_water);
    for (const Level &level : raw_levels) out.appendf("\"%s\":%ld,", level.field, clamp(image.*level.value, 0, 255));
    append_switches(out, image, switches);
}

inline void image_adjustment_ex(APC_StringBuffer &out, const Image &image)
{
    out.appendf("\"mirror\":%d,\"flip\":%d,\"onceAWB\":0,", image.inverted, image.inverted);
    append_switches(out, image, ex_switches);
}
```

Add before `image_adjustment_changes`:

```cpp
template <size_t N>
inline void switch_changes(const Field *fields, size_t count, const Image &image, const Switch (&list)[N],
                           Changes &changes)
{
    for (const Switch &item : list) {
        const Field *field = find_flag(fields, count, item.field);
        if (field != NULL && (field->value == 1) != image.*item.value)
            changes.set("image", item.key, field->value == 1 ? item.on : item.off);
    }
}
```

At the end of `image_adjustment_changes` (after the shutter block):

```cpp
    for (const Level &level : raw_levels) {
        const Field *field = find(fields, count, level.field);
        if (field != NULL && clamp(field->value, 0, 255) != image.*level.value)
            changes.set_number("image", level.key, clamp(field->value, 0, 255));
    }
    switch_changes(fields, count, image, switches, changes);

    const Field *max_exposure = find(fields, count, "max_exposure");
    if (max_exposure != NULL) {
        const unsigned wanted = nearest(max_exposure->value, vendor_max_exposures);
        if ((long)wanted != image.max_exposure &&
            max_exposure->value != (long)nearest(image.max_exposure, vendor_max_exposures))
            changes.set_number("image", "max_exposure", wanted);
    }
```

At the end of `image_adjustment_ex_changes`: `switch_changes(fields, count, image, ex_switches, changes);`

Replace `image_adjustment_keys` with:

```cpp
/* The [image] keys image_adjustment_changes writes; set_default 1 resets them. */
constexpr const char *image_adjustment_keys[] = {
    "brightness", "contrast", "saturation", "white_balance", "wb_red", "wb_green", "wb_blue", "wb_water",
    "shutter", "hue", "sharpness", "gamma", "backlight", "wb_style_red", "wb_style_green", "wb_style_blue",
    "max_gain", "gain_mode", "max_again", "max_dgain", "manual_again", "again", "manual_dgain", "dgain",
    "ae_strategy", "max_exposure"};
static_assert(sizeof(image_adjustment_keys) / sizeof(image_adjustment_keys[0]) <=
                  sizeof(Changes::updates) / sizeof(Changes::updates[0]),
              "a reset fits in one Changes");
```

- [ ] **Step 4: Read the new keys in `fourkcam_image` (`web/mt11-web.cpp`)**

Add before the `mount` orientation lines:

```cpp
    for (const auto &level : apc_fourkcam::raw_levels) {
        effective_value(config, "image", level.key, value, sizeof(value));
        image.*level.value = strtol(value, NULL, 10);
    }
    for (const auto &item : apc_fourkcam::switches) {
        effective_value(config, "image", item.key, value, sizeof(value));
        image.*item.value = strcmp(value, item.on) == 0;
    }
    for (const auto &item : apc_fourkcam::ex_switches) {
        effective_value(config, "image", item.key, value, sizeof(value));
        image.*item.value = strcmp(value, item.on) == 0;
    }
    effective_value(config, "image", "max_exposure", value, sizeof(value));
    image.max_exposure = strtol(value, NULL, 10);
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:
```bash
make -C web tests/test_fourkcam && ./web/tests/test_fourkcam
X=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux-
make -C web CAMERA_BACKEND=hisi HISI_CROSS_COMPILE=$X hisi-web 2>&1 | tail -5
```
Expected: `test_fourkcam: ok`; `hisi-web` links.

- [ ] **Step 6: The live check script** (`web/tests/check_4kcam_compat.py`; run later on the camera by the controller, so only check it parses)

Extend the type tables:

```python
IMAGE_TYPES = {
    'brightness': U8, 'contrast': U8, 'saturation': U8, 'antiFog': {0, 1},
    'frameTurbo_pro': {0, 1, 2}, 'AE_strategy_mode': {0, 1}, 'auto_exposureEx': {0, 1},
    'exposure_time': {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464},
    'max_exposure': {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000},
    'auto_awb': {0, 1}, 'awb_red': U8, 'awb_green': U8, 'awb_blue': U8, 'awb_auto_mode': {0, 1},
    'rotate': {0, 1, 2, 3}, 'hue': U8, 'sharpness': U8, 'gamma': U8, 'blc_level': U8,
    'awb_style_red': U8, 'awb_style_green': U8, 'awb_style_blue': U8, 'max_sys_gain': U8,
    'auto_gain_mode': {0, 1}, 'auto_AGain_max': U8, 'auto_DGain_max': U8, 'manual_AGain_enable': {0, 1},
    'manual_AGain': U8, 'manual_DGain_enable': {0, 1}, 'manual_DGain': U8,
}
IMAGE_EX_TYPES = {'mirror': {0, 1}, 'flip': {0, 1}, 'onceAWB': U8, 'low_farme_rate': {0, 1}}
```

In `check_actions`, just before the `# set_default restores the web's defaults` block, add:

```python
    vendor = {'hue': 140, 'sharpness': 200, 'gamma': 40, 'blc_level': 70, 'awb_style_red': 30,
              'awb_style_green': 0, 'awb_style_blue': 60, 'max_sys_gain': 4, 'auto_gain_mode': 1,
              'auto_AGain_max': 40, 'auto_DGain_max': 2, 'manual_AGain_enable': 1, 'manual_AGain': 20,
              'manual_DGain_enable': 0, 'manual_DGain': 8, 'AE_strategy_mode': 1, 'max_exposure': 50}
    round_trip(args, check, 'setImageAdjustment', vendor, 'getImageAdjustment', None, vendor)
    round_trip(args, check, 'setImageAdjustmentEx', {'low_farme_rate': 1}, 'getImageAdjustmentEx', None,
               {'low_farme_rate': 1})
    round_trip(args, check, 'setImageAdjustmentEx', {'low_farme_rate': 0}, 'getImageAdjustmentEx', None,
               {'low_farme_rate': 0})
```

and extend the `set_default` round trip's expected reads with the defaults:

```python
    round_trip(args, check, 'setImageAdjustment', {'set_default': 1, 'brightness': 10}, 'getImageAdjustment', None,
               {'brightness': 128, 'contrast': 128, 'saturation': 128, 'auto_awb': 0, 'awb_auto_mode': 0,
                'auto_exposureEx': 0, 'hue': 128, 'sharpness': 128, 'gamma': 20, 'blc_level': 50,
                'awb_style_red': 0, 'awb_style_green': 0, 'awb_style_blue': 0, 'max_sys_gain': 12,
                'auto_gain_mode': 0, 'auto_AGain_max': 32, 'auto_DGain_max': 0, 'manual_AGain_enable': 0,
                'manual_AGain': 128, 'manual_DGain_enable': 0, 'manual_DGain': 128, 'AE_strategy_mode': 0,
                'max_exposure': 25})
```

Run: `python3 -m py_compile web/tests/check_4kcam_compat.py`
Expected: no output.

- [ ] **Step 7: Docs** (`docs/4kcam-manager-compat.md`)

In the field mapping table, insert after the `awb_auto_mode` row:

```markdown
| `hue`, `sharpness`, `gamma` (0–255) | `image.*`, the same value. On the ISP: hue × 100 / 255 (128 is neutral); sharpness moves each ISO step's detail by sharpness / 2 − 64 around the vendor's tuning; gamma picks one of three tone curves by gamma / 32 (224–255 keep the current one) |
| `blc_level` (0–255) | `image.backlight`: the auto-exposure brightness target (the vendor's name aside, not black level) |
| `awb_style_red`, `awb_style_green`, `awb_style_blue` (0–255) | `image.wb_style_*`: a gain of 1 + value / 512 on that colour after colour correction |
```

change the `awb_auto_mode` row's right cell to: `` `image.wb_water` `green` / `blue`: the auto white balance calibration and colour matrices for that water, and the one-push target ``

and insert after the `auto_exposureEx` row:

```markdown
| `max_exposure` (1/x s) | `image.max_exposure`, one of the extension's 13 values (others snap to the nearest); the longest automatic exposure. Reads report the nearest of them |
| `max_sys_gain`, `auto_AGain_max`, `auto_DGain_max` (0–255) | `image.max_gain`, `max_again`, `max_dgain`: automatic gain limits of (48 × value + 1)×, 0.75 × value and value × (each at least 1×) |
| `auto_gain_mode` 0 / 1 | `image.gain_mode` `auto` / `manual`. Manual lets the two enables below fix those gains; exposure time stays automatic unless `auto_exposureEx` is 1. A fixed `iso` (Photos page, MAVLink) overrides it |
| `manual_AGain_enable`, `manual_AGain`, `manual_DGain_enable`, `manual_DGain` | `image.manual_again`, `again`, `manual_dgain`, `dgain`: with `gain_mode` manual, analog or ISP digital gain fixed at value / 4 in whole steps (at least 1×) |
| `AE_strategy_mode` 0 / 1 | `image.ae_strategy` `highlight` / `lowlight` priority |
| `low_farme_rate` 0 / 1 (`setImageAdjustmentEx`) | `image.slow_shutter`: auto exposure may lower the frame rate in the dark |
```

Change the `set_default` row's key list to: `resets every [image] key above that setImageAdjustment maps (not slow_shutter or orientation) to the web's defaults`, keeping the rest of the row. In the Settings table, add one row: `` | `[image] hue`, `sharpness`, `gamma`, `backlight`, `wb_style_*`, `max_gain`, `gain_mode`, `max_again`, `max_dgain`, `manual_again`, `again`, `manual_dgain`, `dgain`, `ae_strategy`, `max_exposure`, `slow_shutter` | Video tab, in the units above | vendor defaults: 128, 128, 20, 50, 0, 12, auto, 32, 0, false, 128, false, 128, highlight, 25, false | `` and change the `wb_water` row's Web cell to `**One-push water** (green / blue), which also sets the auto white balance calibration`.

- [ ] **Step 8: Commit**

```bash
git add web/APC_FourKCam.h web/mt11-web.cpp web/tests/test_fourkcam.cpp web/tests/check_4kcam_compat.py docs/4kcam-manager-compat.md
git commit -m "web: 4K Cam Manager's hue, sharpness, gamma, backlight, WB style, gain and exposure limits"
```

---

## After the tasks (controller, not a subagent)

On the camera, in RAM only, through the Pi (192.168.191.33): build both binaries and the appfs image, bind-mount them, restart camera-app through the web, then run `check_4kcam_compat.py` and the frame measurements listed in the spec's Testing section, and the full `make -C web test` / `make -C camera_app test` on willpc.
