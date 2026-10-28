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

    // a reload copies MAVLink parameters one by one, and these have none
    struct ca_config running;
    ca_config_defaults(&running);
    config.brightness = running.brightness + 1;
    ca_config_copy_4kcam(&running, &config);
    assert(running.hue == 200 && running.wb_water == CA_WATER_BLUE && running.slow_shutter);
    assert(running.max_exposure == 8000 && running.gain_mode == CA_GAIN_MANUAL);
    assert(running.brightness + 1 == config.brightness);

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
