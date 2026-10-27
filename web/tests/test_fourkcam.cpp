#include "../APC_FourKCam.h"
#include <cassert>
#include <climits>

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

    // only targets that differ from the current setting are changes
    changes = Changes();
    assert(parse("{\"brightness\":127,\"exposure_time\":500,\"awb_red\":300}", fields, &count));
    image.wb_red = red_gain(255);
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
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
    Field fields[16];
    size_t count;
    assert(parse("{\"set_default\":1,\"brightness\":0}", fields, &count) && resets(fields, count));
    assert(parse("{\"set_default\":0}", fields, &count) && !resets(fields, count));
    assert(parse("{\"set_default\":2}", fields, &count) && !resets(fields, count));
    assert(parse("{}", fields, &count) && !resets(fields, count));

    // the reset covers every setting setImageAdjustment writes
    Image image = test_image();
    strcpy(image.white_balance, "daylight");
    Changes changes;
    assert(parse("{\"brightness\":0,\"contrast\":0,\"saturation\":0,\"awb_red\":1,\"awb_green\":1,\"awb_blue\":1,"
                 "\"awb_auto_mode\":1,\"auto_exposureEx\":1,\"exposure_time\":30}", fields, &count));
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

    changes = Changes();
    assert(parse("{\"bitrate\":30000,\"frame_rate\":60}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "bitrate_kbps")->value, "20000"));

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

/* A client echoing a whole reply back parses at capacity 16 and changes nothing. */
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
        Field fields[16];
        size_t count;
        Changes changes;
        assert(out.valid() && parse_object(out.data(), out.size(), fields, 16, &count));
        image_adjustment_changes(fields, count, image, changes);
        assert(changes.count == 0);

        out.reset();
        out.append("{");
        image_adjustment_ex(out, image);
        finish(out, 0, "", identity);
        assert(out.valid() && parse_object(out.data(), out.size(), fields, 16, &count));
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
        Field fields[16];
        size_t count;
        Changes changes;
        assert(out.valid() && parse_object(out.data(), out.size(), fields, 16, &count));
        venc_changes(fields, count, stream, main_names, 5, "stream.main", changes);
        assert(changes.count == 0);
    }
}

int main()
{
    test_json();
    test_conversions();
    test_image_changes();
    test_image_ex_changes();
    test_reset();
    test_venc();
    test_replies();
    test_echo();
    puts("test_fourkcam: ok");
    return 0;
}
