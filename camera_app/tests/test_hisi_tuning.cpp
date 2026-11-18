#include "image_tuning.h"
#include "apcam/APC_Config.h"

#include <cassert>
#include <cstdio>
#include <cstring>

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
    assert(iso_gain(0) == 1024 && iso_gain(1) == 1024 && iso_gain(4) == 8192 && iso_gain(6) == 32768);
    // ISO 3200 and the default manual gains exceed the default caps
    assert(with_manual(again_max(32), iso_gain(6)) == 32768 && with_manual(again_max(32), 4096) == 24576);
    assert(with_manual(ispd_gain_max(0), manual_gain(8)) == 2048 && with_manual(ispd_gain_max(4), 2048) == 4096);
    assert(sys_gain(1024, 1024, 1024) == 1024 && sys_gain(32768, 1024, 1024) == 32768);
    assert(sys_gain(4096, 1024, 2048) == 8192 && sys_gain(64512, 128914, 64512) == 511659666U);
    assert(exposure_max_us(12) == 83333 && exposure_max_us(25) == 40000 && exposure_max_us(8000) == 125);

    // every [image] shutter choice, slowest first, is 1/N s; the 4K Cam Manager offers 15 of them
    assert(shutter_us(CA_SHUTTER_AUTO) == 0 && shutter_us(CA_SHUTTER_1_12) == 83333);
    assert(shutter_us(CA_SHUTTER_1_250) == 4000 && shutter_us(CA_SHUTTER_1_750) == 1333);
    assert(shutter_us(CA_SHUTTER_1_100000) == 10 && shutter_us((enum ca_shutter_mode)99) == 0);
    const APC_Config::Field *shutter = APC_Config::find("image", "shutter");
    assert(shutter != NULL);
    const ca_config_option *options = shutter->options;
    const size_t option_count = shutter->option_count;
    assert(option_count == CA_SHUTTER_1_100000 + 1U && !strcmp(options[0].name, "auto"));
    static const unsigned vendor[] = {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 100000};
    unsigned offered = 0, previous = 0;
    for (size_t i = 1; i < option_count; i++) {
        unsigned denominator = 0;
        char end;
        assert(sscanf(options[i].name, "1/%u%c", &denominator, &end) == 1 && denominator > previous);
        assert(shutter_us((enum ca_shutter_mode)options[i].value) == 1000000U / denominator);
        for (unsigned value : vendor) offered += value == denominator;
        previous = denominator;
    }
    assert(offered == sizeof(vendor) / sizeof(vendor[0]));

    // iris priority: a fixed shutter and ISO are pinned as the other modes fix them, whatever the gain mode
    struct ca_config config {};
    config.shutter = CA_SHUTTER_1_250;
    config.iso = CA_ISO_800;
    config.gain_mode = CA_GAIN_MANUAL;
    config.manual_again = config.manual_dgain = true;
    config.again = 40;
    config.dgain = 12;
    PinnedExposure pinned = iris_priority_exposure(config);
    assert(pinned.exp_time_us == 4000 && pinned.again == 8192 && pinned.dgain == 1024 && pinned.ispdgain == 1024);
    assert(pinned.sys_gain == 8192);
    config.iso = CA_ISO_3200;
    assert(iris_priority_exposure(config).again == 32768 && iris_priority_exposure(config).sys_gain == 32768);
    // an automatic shutter is pinned at 1/60 s
    config.shutter = CA_SHUTTER_AUTO;
    assert(iris_priority_exposure(config).exp_time_us == 16666);
    config.shutter = CA_SHUTTER_1_100000;
    assert(iris_priority_exposure(config).exp_time_us == 10);
    // ISO auto: the enabled manual gains, analog and ISP digital, 1x for the others
    config.iso = CA_ISO_AUTO;
    pinned = iris_priority_exposure(config);
    assert(pinned.again == 10240 && pinned.dgain == 1024 && pinned.ispdgain == 3072 && pinned.sys_gain == 30720);
    config.manual_dgain = false;
    pinned = iris_priority_exposure(config);
    assert(pinned.again == 10240 && pinned.ispdgain == 1024 && pinned.sys_gain == 10240);
    config.manual_again = false;
    config.manual_dgain = true;
    pinned = iris_priority_exposure(config);
    assert(pinned.again == 1024 && pinned.ispdgain == 3072 && pinned.sys_gain == 3072);
    // ISO auto and gain mode auto: 1x, whatever the manual gain settings say
    config.gain_mode = CA_GAIN_AUTO;
    config.manual_again = true;
    pinned = iris_priority_exposure(config);
    assert(pinned.again == 1024 && pinned.dgain == 1024 && pinned.ispdgain == 1024 && pinned.sys_gain == 1024);
    // the system gain is always the product of the pinned gains
    for (int iso = CA_ISO_AUTO; iso <= CA_ISO_3200; iso++) {
        for (unsigned gains = 0; gains < 4; gains++) {
            config.iso = (enum ca_iso_mode)iso;
            config.gain_mode = CA_GAIN_MANUAL;
            config.manual_again = gains & 1U;
            config.manual_dgain = gains & 2U;
            config.again = config.dgain = 255;
            pinned = iris_priority_exposure(config);
            assert(pinned.dgain == 1024 && pinned.again >= 1024 && pinned.ispdgain >= 1024);
            assert(pinned.sys_gain == sys_gain(pinned.again, pinned.dgain, pinned.ispdgain));
        }
    }

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
