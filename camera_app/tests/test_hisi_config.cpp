#include "camera_app/config.h"
#include "apcam/APC_Config.h"

#include <assert.h>
#include <initializer_list>
#include <limits.h>
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
    assert(config.brightness == 50 && config.saturation == 40 && config.contrast == 50);
    assert(config.hue == 128 && config.sharpness == 64 && config.gamma == 20 && config.backlight == 50);
    assert(config.wb_style_red == 0 && config.wb_style_green == 0 && config.wb_style_blue == 0);
    assert(config.wb_water == CA_WATER_GREEN && config.max_gain == 64 && config.gain_mode == CA_GAIN_AUTO);
    assert(config.max_again == 255 && config.max_dgain == 0 && !config.manual_again && config.again == 128);
    assert(!config.manual_dgain && config.dgain == 128 && config.ae_strategy == CA_AE_PRIOR_HIGHLIGHT);
    assert(config.max_exposure == 30 && !config.slow_shutter && config.iris_mode == CA_IRIS_AUTO);
    // Bayer TNR off against ghosting; VI 3DNR as the VI start-up leaves it
    assert(!config.tnr && config.tnr_strength == 255 && config.vi_3dnr && config.vi_3dnr_strength == 100);

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

    // temporal noise reduction: no MAVLink names, live like the other image settings
    write_file(path, "[image]\ntnr = false\ntnr_strength = 0\nvi_3dnr = false\nvi_3dnr_strength = 0\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(!config.tnr && config.tnr_strength == 0 && !config.vi_3dnr && config.vi_3dnr_strength == 0);
    write_file(path, "[image]\ntnr = true\ntnr_strength = 255\nvi_3dnr = true\nvi_3dnr_strength = 100\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.tnr && config.tnr_strength == 255 && config.vi_3dnr && config.vi_3dnr_strength == 100);
    static const char *const nr_keys[] = {"tnr", "tnr_strength", "vi_3dnr", "vi_3dnr_strength"};
    for (const char *key : nr_keys) {
        const APC_Config::Field *field = APC_Config::find("image", key);
        assert(field != NULL && field->param_name == NULL);
    }
    assert(APC_Config::find("image", "tnr_strength")->minimum == 0 &&
           APC_Config::find("image", "tnr_strength")->maximum == 255);
    assert(APC_Config::find("image", "vi_3dnr_strength")->minimum == 0 &&
           APC_Config::find("image", "vi_3dnr_strength")->maximum == 100);
    struct ca_config nr;
    ca_config_defaults(&config);
    static void (*const nr_changes[])(struct ca_config *) = {
        [](struct ca_config *c) { c->tnr = true; },
        [](struct ca_config *c) { c->tnr_strength = 64U; },
        [](struct ca_config *c) { c->vi_3dnr = false; },
        [](struct ca_config *c) { c->vi_3dnr_strength = 30U; },
    };
    for (auto change : nr_changes) {
        nr = config;
        change(&nr);
        assert(!ca_config_image_equal(&config, &nr));
        ca_config_defaults(&running);
        ca_config_copy_image(&running, &nr);
        assert(ca_config_image_equal(&running, &nr));
        ca_config_defaults(&running);
        ca_config_copy_4kcam(&running, &nr);
        assert(ca_config_image_equal(&running, &nr));
    }

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

    // keyframe interval in ms to frames per GOP at the stream's frame rate
    static const unsigned rates[] = {30U, 25U, 15U, 5U};
    for (unsigned fps : rates) assert(apcam_gop_frames(APCAM_DEFAULT_KEYINT_MS, fps) == fps);
    assert(apcam_gop_frames(500U, 30U) == 15U && apcam_gop_frames(30U, 30U) == 1U);
    assert(apcam_gop_frames(10000U, 30U) == 300U && apcam_gop_frames(10000U, 5U) == 50U);
    assert(apcam_gop_frames(1050U, 30U) == 32U && apcam_gop_frames(1049U, 30U) == 31U);  // 31.5 and 31.47
    assert(apcam_gop_frames(100U, 25U) == 3U && apcam_gop_frames(60U, 25U) == 2U);      // 2.5 and 1.5 round up
    assert(apcam_gop_frames(30U, 5U) == 1U && apcam_gop_frames(0U, 30U) == 1U && apcam_gop_frames(1000U, 0U) == 1U);
    assert(apcam_gop_frames(10000U, 240U) == 2400U && apcam_gop_frames(UINT_MAX, UINT_MAX) == APCAM_GOP_MAX_FRAMES);
    // and back, as the 4K Cam Manager's gop is saved: within the bounds it gives the frames again
    for (unsigned fps = 5U; fps <= 30U; fps++) {
        for (unsigned frames = 1U; frames <= 300U; frames++) {
            const unsigned ms = apcam_keyint_ms(frames, fps);
            assert(ms >= APCAM_KEYINT_MIN_MS && ms <= APCAM_KEYINT_MAX_MS);
            if ((unsigned long)frames * 1000UL <= (unsigned long)APCAM_KEYINT_MAX_MS * fps)
                assert(apcam_gop_frames(ms, fps) == frames);
        }
    }
    assert(apcam_keyint_ms(15U, 30U) == 500U && apcam_keyint_ms(1U, 30U) == 33U && apcam_keyint_ms(32U, 30U) == 1067U);
    assert(apcam_keyint_ms(0U, 30U) == APCAM_KEYINT_MIN_MS && apcam_keyint_ms(301U, 30U) == APCAM_KEYINT_MAX_MS);
    assert(apcam_keyint_ms(1U, 0U) == 1000U);

    // [stream.*] keyframe_interval_ms: VIDEO_MAIN_KEYMS / VIDEO_SUB_KEYMS, live like the bitrate
    ca_config_defaults(&config);
    assert(config.main_keyint_ms == 1000U && config.sub_keyint_ms == 1000U);
    const int main_keyint = ca_config_param_find("VIDEO_MAIN_KEYMS");
    const int sub_keyint = ca_config_param_find("VIDEO_SUB_KEYMS");
    assert(main_keyint >= 0 && sub_keyint >= 0 && main_keyint != sub_keyint);
    assert(!strcmp(ca_config_param_name((size_t)main_keyint), "VIDEO_MAIN_KEYMS"));
    assert(!strcmp(ca_config_param_name((size_t)sub_keyint), "VIDEO_SUB_KEYMS"));
    assert(strlen("VIDEO_MAIN_KEYMS") <= 16U);
    const APC_Config::Field *main_field = APC_Config::find("stream.main", "keyframe_interval_ms");
    const APC_Config::Field *sub_field = APC_Config::find("stream.sub", "keyframe_interval_ms");
    assert(main_field != NULL && main_field->minimum == 30 && main_field->maximum == 10000);
    assert(sub_field != NULL && sub_field->minimum == 30 && sub_field->maximum == 10000);
    assert(ca_config_param_get(&config, (size_t)main_keyint) == 1000);
    write_file(path, "[stream.main]\nkeyframe_interval_ms = 500\n[stream.sub]\nkeyframe_interval_ms = 10000\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.main_keyint_ms == 500U && config.sub_keyint_ms == 10000U);
    assert(ca_config_param_get(&config, (size_t)sub_keyint) == 10000);
    assert(ca_config_param_save(&config, path, (size_t)main_keyint, 30) == 0 && config.main_keyint_ms == 30U);
    assert(ca_config_param_save(&config, path, (size_t)sub_keyint, 2500) == 0 && config.sub_keyint_ms == 2500U);
    assert(ca_config_param_save(&config, path, (size_t)main_keyint, 29) < 0 && config.main_keyint_ms == 30U);
    assert(ca_config_param_save(&config, path, (size_t)sub_keyint, 10001) < 0 && config.sub_keyint_ms == 2500U);
    assert(ca_config_param_save(&config, path, (size_t)sub_keyint, 1000.5f) < 0);
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.main_keyint_ms == 30U && config.sub_keyint_ms == 2500U);
    assigned = config;
    assert(ca_config_param_assign(&assigned, (size_t)main_keyint, 10000) == 0 && assigned.main_keyint_ms == 10000U);
    assert(ca_config_param_assign(&assigned, (size_t)main_keyint, 0) < 0 && assigned.main_keyint_ms == 10000U);
    // each alone makes the image settings differ and is copied with them, not with the 4K Cam Manager's
    assert(!ca_config_image_equal(&config, &assigned));
    ca_config_copy_image(&assigned, &config);
    assert(ca_config_image_equal(&config, &assigned) && assigned.main_keyint_ms == 30U);
    assigned.sub_keyint_ms = 1000U;
    assert(!ca_config_image_equal(&config, &assigned));
    ca_config_copy_image(&assigned, &config);
    assert(assigned.sub_keyint_ms == 2500U);
    ca_config_defaults(&running);
    ca_config_copy_4kcam(&running, &config);
    assert(running.main_keyint_ms == 1000U && running.sub_keyint_ms == 1000U);

    // [stream.*] max_iframe_ratio and rtp_pacing_mbps: no MAVLink names, live like the bitrate
    ca_config_defaults(&config);
    assert(config.main_max_iframe_ratio == 0U && config.sub_max_iframe_ratio == 0U);
    assert(config.main_rtp_pacing_mbps == 60U && config.sub_rtp_pacing_mbps == 0U);
    static const struct { const char *section, *key; int maximum; } stream_keys[] = {
        {"stream.main", "max_iframe_ratio", 100}, {"stream.sub", "max_iframe_ratio", 100},
        {"stream.main", "rtp_pacing_mbps", 1000}, {"stream.sub", "rtp_pacing_mbps", 1000},
    };
    for (const auto &key : stream_keys) {
        const APC_Config::Field *field = APC_Config::find(key.section, key.key);
        assert(field != NULL && field->param_name == NULL);
        assert(field->minimum == 0 && field->maximum == key.maximum);
    }
    write_file(path, "[stream.main]\nmax_iframe_ratio = 4\nrtp_pacing_mbps = 0\n"
                     "[stream.sub]\nmax_iframe_ratio = 100\nrtp_pacing_mbps = 1000\n");
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.main_max_iframe_ratio == 4U && config.sub_max_iframe_ratio == 100U);
    assert(config.main_rtp_pacing_mbps == 0U && config.sub_rtp_pacing_mbps == 1000U);
    static void (*const stream_changes[])(struct ca_config *) = {
        [](struct ca_config *c) { c->main_max_iframe_ratio = 3U; },
        [](struct ca_config *c) { c->sub_max_iframe_ratio = 0U; },
        [](struct ca_config *c) { c->main_rtp_pacing_mbps = 80U; },
        [](struct ca_config *c) { c->sub_rtp_pacing_mbps = 20U; },
    };
    for (auto change : stream_changes) {
        assigned = config;
        change(&assigned);
        assert(!ca_config_image_equal(&config, &assigned));
        ca_config_defaults(&running);
        ca_config_copy_4kcam(&running, &assigned);
        assert(ca_config_image_equal(&running, &assigned));
    }

    // [stream.*] super_frame_i_kbits applies live; slice_rows and low_delay rebuild the pipeline
    ca_config_defaults(&config);
    assert(config.main_super_frame_i_kbits == 0U && config.sub_super_frame_i_kbits == 0U);
    assert(config.main_slice_rows == 0U && config.sub_slice_rows == 0U);
    assert(!config.main_low_delay && !config.sub_low_delay);
    static const struct { const char *section, *key; int maximum; } low_latency_keys[] = {
        {"stream.main", "super_frame_i_kbits", 100000}, {"stream.sub", "super_frame_i_kbits", 100000},
        {"stream.main", "slice_rows", 135}, {"stream.sub", "slice_rows", 135},
    };
    for (const auto &key : low_latency_keys) {
        const APC_Config::Field *field = APC_Config::find(key.section, key.key);
        assert(field != NULL && field->param_name == NULL && field->kind == APC_Config::Kind::Uint);
        assert(field->minimum == 0 && field->maximum == key.maximum);
    }
    for (const char *section : {"stream.main", "stream.sub"}) {
        const APC_Config::Field *field = APC_Config::find(section, "low_delay");
        assert(field != NULL && field->param_name == NULL && field->kind == APC_Config::Kind::Bool);
    }
    write_file(path, "[stream.main]\nsuper_frame_i_kbits = 4000\nslice_rows = 17\nlow_delay = true\n"
                     "[stream.sub]\nsuper_frame_i_kbits = 100000\nslice_rows = 135\nlow_delay = false\n");
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.main_super_frame_i_kbits == 4000U && config.sub_super_frame_i_kbits == 100000U);
    assert(config.main_slice_rows == 17U && config.sub_slice_rows == 135U);
    assert(config.main_low_delay && !config.sub_low_delay);
    static void (*const live_changes[])(struct ca_config *) = {
        [](struct ca_config *c) { c->main_super_frame_i_kbits = 0U; },
        [](struct ca_config *c) { c->sub_super_frame_i_kbits = 2000U; },
    };
    for (auto change : live_changes) {
        assigned = config;
        change(&assigned);
        assert(!ca_config_image_equal(&config, &assigned) && ca_config_pipeline_equal(&config, &assigned));
        ca_config_defaults(&running);
        ca_config_copy_4kcam(&running, &assigned);
        assert(ca_config_image_equal(&running, &assigned));
    }
    static void (*const pipeline_changes[])(struct ca_config *) = {
        [](struct ca_config *c) { c->main_slice_rows = 9U; },
        [](struct ca_config *c) { c->sub_slice_rows = 0U; },
        [](struct ca_config *c) { c->main_low_delay = false; },
        [](struct ca_config *c) { c->sub_low_delay = true; },
        [](struct ca_config *c) { c->main_codec = CA_VIDEO_H265; },
        [](struct ca_config *c) { c->sub_resolution = CA_VIDEO_1080P; },
    };
    for (auto change : pipeline_changes) {
        assigned = config;
        change(&assigned);
        // not an image change, so apply_image never sees it as applied
        assert(ca_config_image_equal(&config, &assigned) && !ca_config_pipeline_equal(&config, &assigned));
        // hot reload keeps the running values until the pipeline is rebuilt
        running = config;
        ca_config_copy_4kcam(&running, &assigned);
        assert(ca_config_pipeline_equal(&running, &config));
        ca_config_copy_pipeline(&running, &assigned);
        assert(ca_config_pipeline_equal(&running, &assigned));
    }

    const APC_Config::Field *hue = APC_Config::find("image", "hue");
    assert(hue != NULL && hue->param_name == NULL);

    static const char *const invalid[] = {
        "[image]\nhue = 256\n",          "[image]\nmax_exposure = 11\n", "[image]\nmax_exposure = 8001\n",
        "[image]\ngain_mode = fixed\n",  "[image]\nwb_water = red\n",    "[image]\nae_strategy = middle\n",
        "[image]\nslow_shutter = maybe\n", "[image]\nshutter = 1/34464\n",
        "[image]\ntnr = 1\n", "[image]\ntnr_strength = 256\n", "[image]\ntnr_strength = -1\n",
        "[image]\nvi_3dnr = off\n", "[image]\nvi_3dnr_strength = 101\n", "[image]\nvi_3dnr_strength = 50%\n",
        "[image]\niris_mode = manual\n", "[image]\niris_mode = 1\n", "[image]\niris_mode = Priority\n",
        "[stream.main]\nkeyframe_interval_ms = 29\n", "[stream.main]\nkeyframe_interval_ms = 10001\n",
        "[stream.sub]\nkeyframe_interval_ms = 0\n", "[stream.sub]\nkeyframe_interval_ms = 1s\n",
        "[stream.sub]\nkeyframe_interval_ms = -500\n",
        "[stream.main]\nmax_iframe_ratio = 101\n", "[stream.sub]\nmax_iframe_ratio = -1\n",
        "[stream.main]\nmax_iframe_ratio = 4x\n", "[stream.main]\nrtp_pacing_mbps = 1001\n",
        "[stream.sub]\nrtp_pacing_mbps = -1\n", "[stream.sub]\nrtp_pacing_mbps = 1.5\n",
        "[stream.main]\nsuper_frame_i_kbits = 100001\n", "[stream.sub]\nsuper_frame_i_kbits = -1\n",
        "[stream.main]\nslice_rows = 136\n", "[stream.sub]\nslice_rows = 8.5\n",
        "[stream.main]\nlow_delay = 1\n", "[stream.sub]\nlow_delay = on\n",
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
