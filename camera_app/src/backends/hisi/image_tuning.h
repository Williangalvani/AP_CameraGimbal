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
