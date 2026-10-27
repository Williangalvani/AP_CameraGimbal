#include "../APC_FourKCam.h"
#include <cassert>

using namespace apc_fourkcam;

static bool parse(const char *text, Field *fields, size_t *count)
{
    return parse_object(text, strlen(text), fields, 16, count);
}

static const Update *change(const Changes &changes, const char *key)
{
    for (size_t i = 0; i < changes.count; i++)
        if (!strcmp(changes.updates[i].key, key)) return &changes.updates[i];
    return NULL;
}

static void test_json()
{
    Field fields[16];
    size_t count = 99;
    assert(parse("", fields, &count) && count == 0);
    assert(parse(" \n", fields, &count) && count == 0);
    assert(parse("{}", fields, &count) && count == 0);
    assert(parse("{\"channel\":0,\"pixel_list\":[{\"width\":3840,\"height\":2160}],\"name\":\"a,}\","
                 "\"awb_red\":-3,\"flag\":true, \"rate\" : 25.5 }", fields, &count) && count == 6);
    assert(find(fields, count, "channel") && find(fields, count, "channel")->value == 0);
    assert(find(fields, count, "pixel_list") == NULL);
    assert(find(fields, count, "name") == NULL);
    assert(find(fields, count, "flag") == NULL);
    assert(find(fields, count, "awb_red")->value == -3);
    assert(find(fields, count, "rate")->value == 25);
    assert(!parse("{\"a\":}", fields, &count));
    assert(!parse("{\"a\":1", fields, &count));
    assert(!parse("[1]", fields, &count));
    assert(!parse("{\"a\":1}x", fields, &count));

    static const char *const unterminated[] = {"{\"a\":12", "{\"a\":\"x\\", "{\"a\":-", "{\"a\":[1,"};
    for (const char *text : unterminated) {
        const size_t length = strlen(text);
        char *exact = (char *)malloc(length);
        memcpy(exact, text, length);
        assert(!parse_object(exact, length, fields, 16, &count));
        free(exact);
    }
}

static void test_conversions()
{
    assert(level_from_percent(50) == 128 && level_from_percent(0) == 0 && level_from_percent(100) == 255);
    assert(percent_from_level(128) == 50 && percent_from_level(204) == 80 && percent_from_level(300) == 100);
    assert(red_gain(6) == 346 && red_level(350) == 6 && red_level(500) == 16);
    assert(red_level(100) == 0 && red_level(4095) == 255);
    for (long level = 0; level <= 255; level++) {
        assert(red_level(red_gain(level)) == level);
        assert(green_blue_level(green_blue_gain(level)) == level);
    }
    assert(green_blue_gain(0) == 256 && green_blue_level(504) == 31 && green_blue_level(725) == 59);
    assert(exposure_time("auto") == 60 && exposure_time("1/250") == 200);
    assert(exposure_time("1/750") == 800 && exposure_time("1/30") == 30);
    assert(shutter_denominator(200) == 250 && shutter_denominator(800) == 750);
    assert(shutter_denominator(12) == 30 && shutter_denominator(34464) == 2000);
    static const char *const shutters[] = {"1/30", "1/50", "1/100", "1/250", "1/500", "1/750", "1/1000", "1/2000"};
    for (const char *shutter : shutters) {
        char back[16];
        snprintf(back, sizeof(back), "1/%u", shutter_denominator(exposure_time(shutter)));
        assert(!strcmp(back, shutter));
    }
}

static Image test_image()
{
    Image image;
    image.wb_red = 500;
    image.wb_green = 256;
    image.wb_blue = 504;
    return image;
}

static void test_image_changes()
{
    Field fields[16];
    size_t count;
    Image image = test_image();

    Changes changes;
    assert(parse("{\"auto_awb\":1,\"awb_red\":16,\"awb_blue\":40}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 2 && !strcmp(change(changes, "wb_blue")->value, "576"));
    assert(!strcmp(change(changes, "white_balance")->value, "manual"));

    changes = Changes();
    strcpy(image.white_balance, "daylight");
    assert(parse("{\"auto_awb\":1}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"awb_red\":20}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "wb_red")->value, "556") && !strcmp(change(changes, "white_balance")->value, "manual"));

    changes = Changes();
    assert(parse("{\"auto_awb\":0,\"awb_red\":20}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "white_balance")->value, "auto"));

    image = test_image();
    changes = Changes();
    assert(parse("{\"brightness\":128,\"contrast\":200,\"awb_auto_mode\":1}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 2 && !strcmp(change(changes, "contrast")->value, "78"));
    assert(!strcmp(change(changes, "wb_water")->value, "blue"));

    changes = Changes();
    assert(parse("{\"auto_exposureEx\":1,\"exposure_time\":400}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "1/500"));
    changes = Changes();
    assert(parse("{\"exposure_time\":800}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);

    strcpy(image.shutter, "1/500");
    changes = Changes();
    assert(parse("{\"exposure_time\":400}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"exposure_time\":800}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "1/750"));
    changes = Changes();
    assert(parse("{\"auto_exposureEx\":0}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "auto"));
}

static void test_image_ex_changes()
{
    Field fields[16];
    size_t count;
    Image image = test_image();
    Changes changes;
    assert(parse("{\"onceAWB\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.one_push && changes.count == 0);

    changes = Changes();
    assert(parse("{\"mirror\":0,\"flip\":0,\"onceAWB\":0}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(!changes.one_push && changes.count == 0);
    assert(parse("{\"mirror\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"mirror\":1,\"flip\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "orientation")->value, "inverted"));
    assert(!strcmp(changes.updates[0].section, "mount"));
}

static const char *const main_names[] = {"3840x2160", "1920x1080", "1280x720", "960x540", "854x480"};
static const char *const sub_names[] = {"1920x1080", "1280x720", "854x480"};

static Stream main_stream()
{
    Stream stream;
    stream.width = 3840;
    stream.height = 2160;
    stream.bitrate_kbps = 12000;
    stream.configurable = true;
    stream.min_frame_rate = 5;
    stream.max_bitrate_kbps = 20000;
    return stream;
}

static void test_venc()
{
    Field fields[16];
    size_t count;
    Changes changes;
    assert(parse("{\"channel\":0,\"encode_type\":5,\"pic_width\":1920,\"pic_height\":1080,"
                 "\"bitrate\":12000,\"frame_rate\":25,\"gop\":60}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 3 && !strcmp(change(changes, "codec")->value, "h265"));
    assert(!strcmp(change(changes, "resolution")->value, "1920x1080"));
    assert(!strcmp(change(changes, "frame_rate")->value, "25"));

    changes = Changes();
    assert(parse("{\"bitrate\":30000,\"frame_rate\":60}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "bitrate_kbps")->value, "20000"));

    Stream sub;
    sub.width = 1280;
    sub.height = 720;
    sub.bitrate_kbps = 2048;
    changes = Changes();
    assert(parse("{\"channel\":1,\"pic_width\":640,\"pic_height\":480,\"bitrate\":32,\"frame_rate\":5}", fields, &count));
    venc_changes(fields, count, sub, sub_names, 3, "stream.sub", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "resolution")->value, "854x480"));
    assert(!strcmp(changes.updates[0].section, "stream.sub"));
}

static void test_replies()
{
    APC_StringBuffer out;
    out.append("{");
    image_adjustment(out, Image());
    finish(out, 0, "", {"aa-bb-cc-dd-ee-ff", "AABBCCDDEEFF", "192.168.2.10"});
    assert(strstr(out.data(), "\"brightness\":128,"));
    assert(strstr(out.data(), "\"auto_exposureEx\":0,\"exposure_time\":60,"));
    assert(strstr(out.data(), "\"auto_awb\":0,\"awb_red\":0,\"awb_green\":0,\"awb_blue\":0,\"awb_auto_mode\":0,"));
    assert(strstr(out.data(), "\"code\":0,\"device_mac\":\"aa-bb-cc-dd-ee-ff\",\"device_id\":\"AABBCCDDEEFF\","
                              "\"device_ip\":\"192.168.2.10\",\"log\":\"\",\"sign_tby\":\"\"}"));

    out.reset();
    out.append("{");
    Image inverted;
    inverted.inverted = true;
    image_adjustment_ex(out, inverted);
    venc(out, 0, main_stream(), main_names, 2);
    finish(out, -1, "say \"hi\"\\\n", {"m", "i", "p"});
    assert(strstr(out.data(), "{\"mirror\":1,\"flip\":1,\"onceAWB\":0,"));
    assert(strstr(out.data(), "\"pixel_list\":[{\"width\":3840,\"height\":2160},{\"width\":1920,\"height\":1080}],"));
    assert(strstr(out.data(), "\"pic_width\":3840,\"pic_height\":2160,\"rc_mode\":1,\"bitrate\":12000,"
                              "\"max_framerate\":30,\"frame_rate\":30,\"gop\":60,"));
    assert(strstr(out.data(), "\"code\":-1,") && strstr(out.data(), "\"log\":\"say \\\"hi\\\"\\\\\\u000a\""));
}

int main()
{
    test_json();
    test_conversions();
    test_image_changes();
    test_image_ex_changes();
    test_venc();
    test_replies();
    puts("test_fourkcam: ok");
    return 0;
}
