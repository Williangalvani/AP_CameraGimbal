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
    assert(config.max_exposure == 25 && !config.slow_shutter && config.iris_mode == CA_IRIS_AUTO);

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

    // iris priority is a MAVLink parameter, saved and loaded through camera.ini
    write_file(path, "[image]\niris_mode = priority\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0 && config.iris_mode == CA_IRIS_PRIORITY);
    const int iris = ca_config_param_find("IMG_IRIS_MODE");
    assert(iris >= 0 && !strcmp(ca_config_param_name((size_t)iris), "IMG_IRIS_MODE"));
    assert(ca_config_param_get(&config, (size_t)iris) == CA_IRIS_PRIORITY);
    const ca_config_option *iris_options;
    int iris_min, iris_max;
    assert(ca_config_param_options((size_t)iris, &iris_options, &iris_min, &iris_max) == 2);
    assert(!strcmp(iris_options[0].name, "auto") && iris_options[0].value == CA_IRIS_AUTO);
    assert(!strcmp(iris_options[1].name, "priority") && iris_options[1].value == CA_IRIS_PRIORITY);
    assert(ca_config_param_save(&config, path, (size_t)iris, CA_IRIS_AUTO) == 0 && config.iris_mode == CA_IRIS_AUTO);
    ca_config_defaults(&config);
    config.iris_mode = CA_IRIS_PRIORITY;
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0 && config.iris_mode == CA_IRIS_AUTO);
    assert(ca_config_param_save(&config, path, (size_t)iris, CA_IRIS_PRIORITY) == 0);
    assert(config.iris_mode == CA_IRIS_PRIORITY);
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0 && config.iris_mode == CA_IRIS_PRIORITY);
    assert(ca_config_param_save(&config, path, (size_t)iris, 2) < 0 && config.iris_mode == CA_IRIS_PRIORITY);
    assert(ca_config_param_save(&config, path, (size_t)iris, -1) < 0);
    struct ca_config assigned = config;
    assert(ca_config_param_assign(&assigned, (size_t)iris, CA_IRIS_AUTO) == 0 && assigned.iris_mode == CA_IRIS_AUTO);
    // it alone makes the image settings differ, and is copied with them but not with the 4K Cam Manager's
    assert(!ca_config_image_equal(&config, &assigned));
    ca_config_copy_image(&assigned, &config);
    assert(ca_config_image_equal(&config, &assigned) && assigned.iris_mode == CA_IRIS_PRIORITY);
    ca_config_defaults(&running);
    ca_config_copy_4kcam(&running, &config);
    assert(running.iris_mode == CA_IRIS_AUTO);

    write_file(path, "[image]\nshutter = 1/100000\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0 && config.shutter == CA_SHUTTER_1_100000);
    write_file(path, "[image]\nshutter = 1/12\n");
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0 && config.shutter == CA_SHUTTER_1_12);

    // bitrate 0 (automatic) is the hisi rule for the main resolution, as both the
    // encoder and MAVLink's VIDEO_STREAM_INFORMATION use it
#ifndef APCAM_AUTO_MAIN_BITRATE_KBPS
#error "hisi must define APCAM_AUTO_MAIN_BITRATE_KBPS"
#endif
    static const struct { enum ca_video_resolution resolution; unsigned kbps; } automatic[] = {
        {CA_VIDEO_2160P, 12000U}, {CA_VIDEO_1080P, 4096U}, {CA_VIDEO_720P, 2048U}, {CA_VIDEO_480P, 2048U},
    };
    ca_config_defaults(&config);
    assert(config.main_bitrate_kbps == 0U);
    for (const auto &expected : automatic) {
        unsigned width, height;
        ca_video_resolution_size(expected.resolution, &width, &height);
        assert(APCAM_AUTO_MAIN_BITRATE_KBPS(width, height) == apcam_hisi_auto_bitrate_kbps(width, height));
        assert(apcam_main_bitrate_kbps(config.main_bitrate_kbps, width, height) == expected.kbps);
        assert(apcam_main_bitrate_kbps(8000U, width, height) == 8000U);
    }

    const APC_Config::Field *hue = APC_Config::find("image", "hue");
    assert(hue != NULL && hue->param_name == NULL);

    static const char *const invalid[] = {
        "[image]\nhue = 256\n",          "[image]\nmax_exposure = 11\n", "[image]\nmax_exposure = 8001\n",
        "[image]\ngain_mode = fixed\n",  "[image]\nwb_water = red\n",    "[image]\nae_strategy = middle\n",
        "[image]\nslow_shutter = maybe\n", "[image]\nshutter = 1/34464\n",
        "[image]\niris_mode = manual\n", "[image]\niris_mode = 1\n", "[image]\niris_mode = Priority\n",
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
