#include "../APC_FourKCam.h"
#include <cassert>
#include <climits>

using namespace apc_fourkcam;

static bool parse(const char *text, Field *fields, size_t *count)
{
    return parse_object(text, strlen(text), fields, 16, count);
}

static bool parse_wide(const char *text, Field *fields, size_t *count)
{
    return parse_object(text, strlen(text), fields, 64, count);
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
                 "\"awb_red\":-3,\"flag\":true, \"rate\" : 25.5 }", fields, &count) && count == 3);
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

    static const char *const malformed[] = {"{\"auto_awb\":-}", "{\"brightness\":1abc}", "{\"a\":1 2}",
                                            "{\"a\":1\"x\"}", "{\"a\":[},\"b\":1}", "{\"a\":{]}", "{\"a\":1.}",
                                            "{\"a\":.5}", "{\"a\":+1}", "{\"a\":tru}", "{\"a\":\"x\"1}"};
    for (const char *text : malformed) assert(!parse(text, fields, &count));
    assert(parse("{\"a\":-0.75 ,\"b\":null,\"c\":false,\"d\":{\"e\":[1,{\"f\":\"]\"}]}}", fields, &count) && count == 1);
    assert(find(fields, count, "a")->value == 0 && find(fields, count, "b") == NULL);

    assert(parse("{\"big\":99999999999999999999,\"small\":-99999999999999999999}", fields, &count));
    assert(find(fields, count, "big")->value == LONG_MAX && find(fields, count, "small")->value <= -LONG_MAX);

    // keys longer than Field::key holds are ignored
    assert(parse("{\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":1,\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\":2,"
                 "\"cccccccccccccccccccccccccccccccccccccccccccccccc\":3,\"d\":4}", fields, &count) && count == 2);
    assert(find(fields, count, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")->value == 1 && find(fields, count, "d")->value == 4);

    const char *two = "{\"a\":1,\"b\":2}";
    const char *three = "{\"a\":1,\"b\":2,\"c\":3}";
    assert(parse_object(two, strlen(two), fields, 2, &count) && count == 2);
    assert(!parse_object(three, strlen(three), fields, 2, &count));
    const char *mixed = "{\"a\":1,\"s\":\"x\",\"n\":null,\"l\":[1,2],\"b\":2}";
    assert(parse_object(mixed, strlen(mixed), fields, 2, &count) && count == 2);
    assert(!parse("{\"a\":1,\"s\":\"x\\\"}", fields, &count));

    char nested[64];
    for (unsigned depth = 1; depth <= 17; depth++) {
        unsigned n = 0;
        n += (unsigned)snprintf(nested + n, sizeof(nested) - n, "{\"a\":");
        for (unsigned i = 0; i < depth; i++) nested[n++] = '[';
        for (unsigned i = 0; i < depth; i++) nested[n++] = ']';
        nested[n++] = '}';
        nested[n] = '\0';
        assert(parse(nested, fields, &count) == (depth <= max_depth));
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
    for (long percent = 0; percent <= 100; percent++) assert(percent_from_level(level_from_percent(percent)) == percent);
    assert(green_blue_gain(0) == 256 && green_blue_level(504) == 31 && green_blue_level(725) == 59);
    // every vendor exposure is a shutter of its own; 34464 is 1/100000 s
    static const unsigned vendor[] = {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464};
    static const char *const shutters[] = {"1/12", "1/25", "1/30", "1/50", "1/60", "1/100", "1/200", "1/400",
                                           "1/800", "1/1000", "1/2000", "1/4000", "1/8000", "1/10000", "1/100000"};
    for (size_t i = 0; i < sizeof(vendor) / sizeof(vendor[0]); i++) {
        char back[16];
        snprintf(back, sizeof(back), "1/%u", shutter_denominator(vendor[i]));
        assert(!strcmp(back, shutters[i]) && exposure_time(shutters[i]) == vendor[i]);
    }
    assert(exposure_time("auto") == 60 && exposure_time("1/250") == 200);
    assert(exposure_time("1/500") == 400 && exposure_time("1/750") == 800);
    // anything else is the nearest vendor exposure
    assert(shutter_denominator(250) == 200 && shutter_denominator(500) == 400 && shutter_denominator(1) == 12);
    assert(shutter_denominator(20000) == 10000 && shutter_denominator(50000) == 100000);
}

/* Each vendor exposure survives setImageAdjustment, then getImageAdjustment. */
static void test_exposure_round_trip()
{
    static const unsigned vendor[] = {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464};
    for (unsigned exposure : vendor) {
        Image image;
        char request[64];
        snprintf(request, sizeof(request), "{\"auto_exposureEx\":1,\"exposure_time\":%u}", exposure);
        Field fields[16];
        size_t count;
        Changes changes;
        assert(parse(request, fields, &count));
        image_adjustment_changes(fields, count, image, changes);
        assert(changes.count == 1 && !strcmp(changes.updates[0].key, "shutter"));
        snprintf(image.shutter, sizeof(image.shutter), "%.15s", changes.updates[0].value);

        APC_StringBuffer out;
        out.append("{");
        image_adjustment(out, image);
        out.append("}");
        char expected[64];
        snprintf(expected, sizeof(expected), "\"auto_exposureEx\":1,\"exposure_time\":%u,", exposure);
        assert(out.valid() && strstr(out.data(), expected));

        changes = Changes();
        image_adjustment_changes(fields, count, image, changes);
        assert(changes.count == 0);
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
    assert(!strcmp(change(changes, "shutter")->value, "1/400"));
    changes = Changes();
    assert(parse("{\"exposure_time\":800}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);

    strcpy(image.shutter, "1/500");
    changes = Changes();
    assert(parse("{\"exposure_time\":400}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    // an older config's 1/500 stays until the exposure moves
    assert(parse("{\"exposure_time\":800}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "1/800"));
    changes = Changes();
    assert(parse("{\"auto_exposureEx\":0}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "auto"));

    // only targets that differ from the current setting are changes
    changes = Changes();
    assert(parse("{\"brightness\":127,\"exposure_time\":400,\"awb_red\":300}", fields, &count));
    image.wb_red = red_gain(255);
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    // 500 isn't a vendor exposure: it is the nearest one, 400
    assert(parse("{\"exposure_time\":500}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(changes.updates[0].value, "1/400"));
    changes = Changes();
    assert(parse("{\"brightness\":99999999999999999999,\"saturation\":-99999999999999999999}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "brightness")->value, "100") && !strcmp(change(changes, "saturation")->value, "0"));

    // enums that only take 0 or 1 change nothing otherwise
    image = test_image();
    image.blue_water = true;
    strcpy(image.shutter, "1/500");
    strcpy(image.white_balance, "daylight");
    changes = Changes();
    assert(parse("{\"awb_auto_mode\":2,\"auto_exposureEx\":2,\"auto_awb\":-1}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
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
    // the extension sends mirror or flip alone; either one that differs turns the image
    assert(parse("{\"mirror\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(change(changes, "orientation")->value, "inverted"));
    assert(!strcmp(changes.updates[0].section, "mount"));
    changes = Changes();
    assert(parse("{\"mirror\":1,\"flip\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(change(changes, "orientation")->value, "inverted"));
    changes = Changes();
    assert(parse("{\"mirror\":0,\"flip\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(change(changes, "orientation")->value, "inverted"));

    image.inverted = true;
    changes = Changes();
    assert(parse("{\"flip\":0}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(change(changes, "orientation")->value, "upright"));
    changes = Changes();
    assert(parse("{\"mirror\":1,\"flip\":0}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 1 && !strcmp(change(changes, "orientation")->value, "upright"));
    changes = Changes();
    assert(parse("{\"mirror\":1,\"flip\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(parse("{\"mirror\":2,\"flip\":2}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(parse("{\"mirror\":1,\"flip\":7}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 0);
}

static void test_reset()
{
    Field fields[64];
    size_t count;
    assert(parse("{\"set_default\":1,\"brightness\":0}", fields, &count) && resets(fields, count));
    assert(parse("{\"set_default\":0}", fields, &count) && !resets(fields, count));
    assert(parse("{\"set_default\":2}", fields, &count) && !resets(fields, count));
    assert(parse("{}", fields, &count) && !resets(fields, count));

    // the reset covers every setting setImageAdjustment writes
    Image image = test_image();
    strcpy(image.white_balance, "daylight");
    Changes changes;
    assert(parse_wide("{\"brightness\":0,\"contrast\":0,\"saturation\":0,\"awb_red\":1,\"awb_green\":1,\"awb_blue\":1,"
                      "\"awb_auto_mode\":1,\"auto_exposureEx\":1,\"exposure_time\":30,\"hue\":1,\"sharpness\":1,"
                      "\"gamma\":1,\"blc_level\":1,\"awb_style_red\":1,\"awb_style_green\":1,\"awb_style_blue\":1,"
                      "\"max_sys_gain\":1,\"auto_gain_mode\":1,\"auto_AGain_max\":1,\"auto_DGain_max\":1,"
                      "\"manual_AGain_enable\":1,\"manual_AGain\":1,\"manual_DGain_enable\":1,\"manual_DGain\":1,"
                      "\"AE_strategy_mode\":1,\"max_exposure\":12}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == sizeof(image_adjustment_keys) / sizeof(image_adjustment_keys[0]));
    for (size_t i = 0; i < changes.count; i++) {
        bool covered = false;
        for (const char *key : image_adjustment_keys) covered = covered || !strcmp(changes.updates[i].key, key);
        assert(covered && !strcmp(changes.updates[i].section, "image"));
    }
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
    assert(changes.restart);

    /* a codec change, either way, restarts camera-app unless it switches live;
     * the current codec or another type doesn't */
    Stream h265 = main_stream();
    h265.h265 = true;
    changes = Changes();
    assert(parse("{\"channel\":0,\"encode_type\":1}", fields, &count));
    venc_changes(fields, count, h265, main_names, 5, "stream.main", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "codec")->value, "h264") && changes.restart);
    changes = Changes();
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 0 && !changes.restart);
    changes = Changes();
    assert(parse("{\"channel\":0,\"encode_type\":2}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 0 && !changes.restart);

    // the encoder profile is fixed: any encode_profile is accepted and changes nothing
    const char *const profile_requests[] = {"{\"channel\":0,\"encode_profile\":0}",
                                            "{\"channel\":0,\"encode_profile\":1}",
                                            "{\"channel\":0,\"encode_profile\":7}"};
    const Stream profile_streams[] = {main_stream(), h265};
    for (const char *request : profile_requests) {
        for (const Stream &stream : profile_streams) {
            changes = Changes();
            assert(parse(request, fields, &count));
            venc_changes(fields, count, stream, main_names, 5, "stream.main", changes);
            assert(changes.count == 0 && !changes.restart);
        }
    }

    changes = Changes();
    assert(parse("{\"bitrate\":30000,\"frame_rate\":60}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "bitrate_kbps")->value, "20000"));
    assert(!changes.restart);

    Stream at_max = main_stream();
    at_max.bitrate_kbps = 20000;
    changes = Changes();
    venc_changes(fields, count, at_max, main_names, 5, "stream.main", changes);
    assert(changes.count == 0);

    Stream no_max = main_stream();
    no_max.max_bitrate_kbps = 0;
    changes = Changes();
    assert(parse("{\"bitrate\":5000}", fields, &count));
    venc_changes(fields, count, no_max, main_names, 5, "stream.main", changes);
    assert(changes.count == 0);

    Stream fast = main_stream();
    fast.frame_rate = 60;
    changes = Changes();
    assert(parse("{\"frame_rate\":60}", fields, &count));
    venc_changes(fields, count, fast, main_names, 5, "stream.main", changes);
    assert(changes.count == 0);

    Stream sub;
    sub.width = 1280;
    sub.height = 720;
    sub.bitrate_kbps = 2048;
    changes = Changes();
    assert(parse("{\"channel\":1,\"pic_width\":640,\"pic_height\":480,\"bitrate\":32,\"frame_rate\":5}", fields, &count));
    venc_changes(fields, count, sub, sub_names, 3, "stream.sub", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "resolution")->value, "854x480"));
    assert(!strcmp(changes.updates[0].section, "stream.sub") && !changes.restart);

    changes = Changes();
    assert(parse("{\"channel\":1,\"encode_type\":5}", fields, &count));
    venc_changes(fields, count, sub, sub_names, 3, "stream.sub", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "codec")->value, "h265") && changes.restart);
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

    // a one-push saves the vendor's slider values as gains, which read back unchanged
    Image one_push;
    strcpy(one_push.white_balance, "manual");
    one_push.wb_red = red_gain(46);
    one_push.wb_green = green_blue_gain(1);
    one_push.wb_blue = green_blue_gain(55);
    assert(one_push.wb_red == 946 && one_push.wb_green == 264 && one_push.wb_blue == 696);
    out.reset();
    out.append("{");
    image_adjustment(out, one_push);
    assert(strstr(out.data(), "\"auto_awb\":1,\"awb_red\":46,\"awb_green\":1,\"awb_blue\":55,"));

    out.reset();
    out.append("{");
    Image inverted;
    inverted.inverted = true;
    image_adjustment_ex(out, inverted);
    venc(out, 0, main_stream(), main_names, 2);
    finish(out, -1, "say \"hi\"\\\n", {"m", "i", "p"});
    assert(strstr(out.data(), "{\"mirror\":1,\"flip\":1,\"onceAWB\":0,"));
    // the encoder's profile in the extension's enum: H.264 High (2), H.265 Main (1)
    assert(strstr(out.data(), "\"channel\":0,\"encode_type\":1,\"encode_profile\":2,"));
    assert(strstr(out.data(), "\"pixel_list\":[{\"width\":3840,\"height\":2160},{\"width\":1920,\"height\":1080}],"));
    assert(strstr(out.data(), "\"pic_width\":3840,\"pic_height\":2160,\"rc_mode\":1,\"bitrate\":12000,"
                              "\"max_framerate\":30,\"frame_rate\":30,\"gop\":60,"));
    assert(strstr(out.data(), "\"code\":-1,") && strstr(out.data(), "\"log\":\"say \\\"hi\\\"\\\\\\u000a\""));

    out.reset();
    Stream h265 = main_stream();
    h265.h265 = true;
    venc(out, 1, h265, main_names, 2);
    assert(strstr(out.data(), "\"channel\":1,\"encode_type\":5,\"encode_profile\":1,"));

    // venc fields stay in the extension's types: channel 0–2, the rest u16
    out.reset();
    Stream huge = main_stream();
    huge.width = 70000;
    huge.bitrate_kbps = 70000;
    huge.frame_rate = 40000;
    huge.max_frame_rate = 70000;
    static const char *const huge_names[] = {"70000x2160"};
    venc(out, 7, huge, huge_names, 1);
    assert(strstr(out.data(), "\"channel\":2,"));
    assert(strstr(out.data(), "\"pixel_list\":[{\"width\":65535,\"height\":2160}],\"pic_width\":65535,"));
    assert(strstr(out.data(), "\"bitrate\":65535,\"max_framerate\":65535,\"frame_rate\":40000,\"gop\":65535,"));
    out.reset();
    venc(out, -1, huge, huge_names, 0);
    assert(strstr(out.data(), "\"channel\":0,"));
}

/* A client echoing a whole reply back parses at the server's capacity (64) and changes nothing. */
static void test_echo()
{
    const Identity identity = {"aa-bb-cc-dd-ee-ff", "AABBCCDDEEFF", "192.168.2.10"};
    Image images[3];
    images[1] = test_image();
    images[1].brightness = 33;
    strcpy(images[1].white_balance, "daylight");
    strcpy(images[1].shutter, "1/500");
    images[1].blue_water = true;
    images[2] = images[1];
    strcpy(images[2].white_balance, "manual");
    strcpy(images[2].shutter, "1/60");
    images[2].saturation = 120;
    images[2].inverted = true;
    for (const Image &image : images) {
        APC_StringBuffer out;
        out.append("{");
        image_adjustment(out, image);
        finish(out, 0, "ok", identity);
        Field fields[64];
        size_t count;
        Changes changes;
        assert(out.valid() && parse_object(out.data(), out.size(), fields, 64, &count));
        image_adjustment_changes(fields, count, image, changes);
        assert(changes.count == 0);

        out.reset();
        out.append("{");
        image_adjustment_ex(out, image);
        finish(out, 0, "", identity);
        assert(out.valid() && parse_object(out.data(), out.size(), fields, 64, &count));
        image_adjustment_ex_changes(fields, count, image, changes);
        assert(changes.count == 0 && !changes.one_push);
    }

    Stream streams[3] = {main_stream(), main_stream(), Stream()};
    streams[1].h265 = true;
    streams[1].frame_rate = 60;
    streams[1].bitrate_kbps = 30000;
    streams[2].width = 1280;
    streams[2].height = 720;
    streams[2].bitrate_kbps = 2048;
    for (const Stream &stream : streams) {
        APC_StringBuffer out;
        out.append("{");
        venc(out, 0, stream, main_names, 5);
        finish(out, 0, "", identity);
        Field fields[64];
        size_t count;
        Changes changes;
        assert(out.valid() && parse_object(out.data(), out.size(), fields, 64, &count));
        venc_changes(fields, count, stream, main_names, 5, "stream.main", changes);
        assert(changes.count == 0);
    }
}

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

int main()
{
    test_json();
    test_conversions();
    test_exposure_round_trip();
    test_image_changes();
    test_image_ex_changes();
    test_reset();
    test_venc();
    test_replies();
    test_echo();
    test_vendor_image_fields();
    puts("test_fourkcam: ok");
    return 0;
}
