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
