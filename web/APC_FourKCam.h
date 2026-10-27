#pragma once
/* The camera protocol of Blue Robotics' 4K Cam Manager, as the vendor's xipc
 * answers it: POST /action/<name> with a flat JSON object. Names, ranges and
 * enums follow the extension's br4kcam_commands types; the settings are this
 * firmware's camera.ini values (docs/4kcam-manager-compat.md). A field sent
 * with the value reads report changes nothing, so a client echoing a whole
 * reply back doesn't move settings that don't map exactly. */
#include "APC_StringBuffer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace apc_fourkcam {

struct Field {
    char key[32];
    long value;
    bool number;
};

inline const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* Past the closing quote of the string starting at p, or NULL. */
inline const char *skip_string(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\') {
            if (++p == end) return NULL;
            continue;
        }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

/* Past any JSON value starting at p, nested ones included: scalars end at the
 * next ',' or '}' of the enclosing object. NULL if it doesn't end. */
inline const char *skip_value(const char *p, const char *end)
{
    unsigned depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skip_string(p, end);
            if (p == NULL) return NULL;
            if (depth == 0U) return p;
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0U) return p;
            if (--depth == 0U) return p + 1;
        } else if (depth == 0U && *p == ',') {
            return p;
        }
        p++;
    }
    return depth == 0U ? p : NULL;
}

/* One flat JSON object. Numbers are kept (fractions truncated); other values
 * are skipped. An empty body is an empty object: reads have no body. The text
 * needn't be NUL-terminated. */
inline bool parse_object(const char *text, size_t length, Field *fields, size_t capacity, size_t *count)
{
    const char *end = text + length;
    const char *p = skip_space(text, end);
    *count = 0;
    if (p == end) return true;
    if (*p++ != '{') return false;
    p = skip_space(p, end);
    if (p < end && *p == '}') return skip_space(p + 1, end) == end;
    while (p < end) {
        if (*p != '"') return false;
        const char *key_end = skip_string(p, end);
        if (key_end == NULL) return false;
        const char *key = p + 1;
        const size_t key_length = (size_t)(key_end - key - 1);
        p = skip_space(key_end, end);
        if (p == end || *p++ != ':') return false;
        p = skip_space(p, end);
        const char *value_end = skip_value(p, end);
        // the delimiter at value_end also stops strtol inside the text
        if (value_end == NULL || value_end == p || value_end == end) return false;
        if (*count < capacity && key_length < sizeof(fields[0].key)) {
            Field &field = fields[(*count)++];
            memcpy(field.key, key, key_length);
            field.key[key_length] = '\0';
            field.number = *p == '-' || (*p >= '0' && *p <= '9');
            field.value = field.number ? strtol(p, NULL, 10) : 0;
        }
        p = skip_space(value_end, end);
        if (p == end) return false;
        if (*p == '}') return skip_space(p + 1, end) == end;
        if (*p++ != ',') return false;
        p = skip_space(p, end);
    }
    return false;
}

inline const Field *find(const Field *fields, size_t count, const char *key)
{
    for (size_t i = 0; i < count; i++)
        if (fields[i].number && strcmp(fields[i].key, key) == 0) return &fields[i];
    return NULL;
}

inline long clamp(long value, long low, long high)
{
    return value < low ? low : value > high ? high : value;
}

/* image.brightness/contrast/saturation are 0–100; the vendor's are 0–255. */
inline long level_from_percent(long percent) { return (clamp(percent, 0, 100) * 255 + 50) / 100; }
inline long percent_from_level(long level) { return (clamp(level, 0, 255) * 100 + 127) / 255; }

/* The vendor's manual white-balance sliders (0–255) as gains in 1/256. */
inline unsigned red_gain(long level) { return (unsigned)(clamp(level, 0, 255) * 15 + 256); }
inline unsigned green_blue_gain(long level) { return (unsigned)((clamp(level, 0, 255) + 32) * 8); }
inline long red_level(unsigned gain) { return gain <= 256U ? 0 : clamp(((long)gain - 256 + 7) / 15, 0, 255); }
inline long green_blue_level(unsigned gain) { return clamp(((long)gain + 4) / 8 - 32, 0, 255); }

/* exposure_time is x in 1/x s, one of the extension's BaseExposureTimeValue. */
constexpr unsigned vendor_exposures[] = {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464};
/* [image] shutter's manual choices, 1/x s. */
constexpr unsigned shutters[] = {30, 50, 100, 250, 500, 750, 1000, 2000};

template <size_t N>
inline unsigned nearest(long value, const unsigned (&choices)[N])
{
    const double wanted = (double)clamp(value, 1, 1000000);
    unsigned best = choices[0];
    double best_distance = INFINITY;
    for (unsigned choice : choices) {
        const double distance = fabs(log(choice / wanted));
        if (distance < best_distance) {
            best = choice;
            best_distance = distance;
        }
    }
    return best;
}

inline unsigned exposure_time(const char *shutter)
{
    unsigned denominator = 0;
    if (sscanf(shutter, "1/%u", &denominator) != 1 || denominator == 0U) return 60;
    return nearest((long)denominator, vendor_exposures);
}

inline unsigned shutter_denominator(long exposure) { return nearest(exposure, shutters); }

struct Image {
    long brightness = 50;
    long contrast = 50;
    long saturation = 50;
    char white_balance[16] = "auto";
    unsigned wb_red = 256;
    unsigned wb_green = 256;
    unsigned wb_blue = 256;
    bool blue_water = false;
    char shutter[16] = "auto";
    bool inverted = false;
};

struct Stream {
    unsigned width = 0;
    unsigned height = 0;
    bool h265 = false;
    unsigned bitrate_kbps = 0;  // in effect, also when camera.ini says automatic
    unsigned frame_rate = 30;
    unsigned max_frame_rate = 30;
    bool configurable = false;  // bitrate and frame rate are settings
    long min_frame_rate = 30;
    long max_bitrate_kbps = 0;
};

struct Identity {
    const char *mac;  // "aa-bb-cc-dd-ee-ff", the vendor's form
    const char *id;
    const char *ip;
};

struct Update {
    const char *section;
    const char *key;
    char value[24];
};

struct Changes {
    Update updates[16];
    size_t count = 0;
    bool one_push = false;

    void set(const char *section, const char *key, const char *value)
    {
        Update *update = NULL;
        for (size_t i = 0; i < count && update == NULL; i++)
            if (!strcmp(updates[i].section, section) && !strcmp(updates[i].key, key)) update = &updates[i];
        if (update == NULL) {
            if (count == sizeof(updates) / sizeof(updates[0])) return;
            update = &updates[count++];
            update->section = section;
            update->key = key;
        }
        snprintf(update->value, sizeof(update->value), "%s", value);
    }

    void set_number(const char *section, const char *key, long value)
    {
        char text[24];
        snprintf(text, sizeof(text), "%ld", value);
        set(section, key, text);
    }
};

inline void append_json_string(APC_StringBuffer &out, const char *text)
{
    out.append("\"");
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') out.appendf("\\%c", *p);
        else if (*p < 0x20U) out.appendf("\\u%04x", *p);
        else out.append_n((const char *)p, 1);
    }
    out.append("\"");
}

inline void image_adjustment(APC_StringBuffer &out, const Image &image)
{
    out.appendf("\"brightness\":%ld,\"contrast\":%ld,\"saturation\":%ld,"
                "\"auto_exposureEx\":%d,\"exposure_time\":%u,"
                "\"auto_awb\":%d,\"awb_red\":%ld,\"awb_green\":%ld,\"awb_blue\":%ld,\"awb_auto_mode\":%d,"
                "\"antiFog\":0,\"frameTurbo_pro\":0,\"AE_strategy_mode\":0,\"rotate\":0,",
                level_from_percent(image.brightness), level_from_percent(image.contrast),
                level_from_percent(image.saturation), strcmp(image.shutter, "auto") != 0,
                exposure_time(image.shutter), strcmp(image.white_balance, "auto") != 0,
                red_level(image.wb_red), green_blue_level(image.wb_green), green_blue_level(image.wb_blue),
                image.blue_water);
}

inline void image_adjustment_ex(APC_StringBuffer &out, const Image &image)
{
    out.appendf("\"mirror\":%d,\"flip\":%d,\"onceAWB\":0,", image.inverted, image.inverted);
}

inline void venc(APC_StringBuffer &out, long channel, const Stream &stream,
                 const char *const *resolutions, size_t count)
{
    out.appendf("\"channel\":%ld,\"encode_type\":%d,\"encode_profile\":2,\"pixel_list\":[",
                channel, stream.h265 ? 5 : 1);
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        unsigned width, height;
        if (sscanf(resolutions[i], "%ux%u", &width, &height) != 2) continue;
        out.appendf("%s{\"width\":%u,\"height\":%u}", first ? "" : ",", width, height);
        first = false;
    }
    out.appendf("],\"pic_width\":%u,\"pic_height\":%u,\"rc_mode\":1,\"bitrate\":%u,"
                "\"max_framerate\":%u,\"frame_rate\":%u,\"gop\":%u,",
                stream.width, stream.height, stream.bitrate_kbps, stream.max_frame_rate,
                stream.frame_rate, stream.frame_rate * 2U);
}

inline void finish(APC_StringBuffer &out, int code, const char *log, const Identity &identity)
{
    out.appendf("\"code\":%d,\"device_mac\":\"%s\",\"device_id\":\"%s\",\"device_ip\":\"%s\",\"log\":",
                code, identity.mac, identity.id, identity.ip);
    append_json_string(out, log);
    out.append(",\"sign_tby\":\"\"}");
}

inline void image_adjustment_changes(const Field *fields, size_t count, const Image &image, Changes &changes)
{
    const struct {
        const char *key;
        long percent;
    } levels[] = {{"brightness", image.brightness}, {"contrast", image.contrast}, {"saturation", image.saturation}};
    for (const auto &level : levels) {
        const Field *field = find(fields, count, level.key);
        if (field != NULL && field->value != level_from_percent(level.percent))
            changes.set_number("image", level.key, percent_from_level(field->value));
    }

    const size_t before_gains = changes.count;
    const Field *red = find(fields, count, "awb_red");
    const Field *green = find(fields, count, "awb_green");
    const Field *blue = find(fields, count, "awb_blue");
    if (red != NULL && red->value != red_level(image.wb_red))
        changes.set_number("image", "wb_red", red_gain(red->value));
    if (green != NULL && green->value != green_blue_level(image.wb_green))
        changes.set_number("image", "wb_green", green_blue_gain(green->value));
    if (blue != NULL && blue->value != green_blue_level(image.wb_blue))
        changes.set_number("image", "wb_blue", green_blue_gain(blue->value));
    const bool gains_changed = changes.count > before_gains;

    // auto_awb 1 means "not auto": a preset or colour temperature stays, unless the gains move
    const Field *awb = find(fields, count, "auto_awb");
    const bool automatic = strcmp(image.white_balance, "auto") == 0;
    if (awb != NULL && awb->value == 0) {
        if (!automatic) changes.set("image", "white_balance", "auto");
    } else if ((awb != NULL && awb->value == 1 && automatic) ||
               (gains_changed && strcmp(image.white_balance, "manual") != 0)) {
        changes.set("image", "white_balance", "manual");
    }

    const Field *water = find(fields, count, "awb_auto_mode");
    if (water != NULL && (water->value == 1) != image.blue_water)
        changes.set("image", "wb_water", water->value == 1 ? "blue" : "green");

    const Field *mode = find(fields, count, "auto_exposureEx");
    const Field *time = find(fields, count, "exposure_time");
    const bool was_manual = strcmp(image.shutter, "auto") != 0;
    const bool manual = mode != NULL ? mode->value == 1 : was_manual;
    if (!manual && was_manual) {
        changes.set("image", "shutter", "auto");
    } else if (manual) {
        const long wanted = time != NULL ? time->value : (long)exposure_time(image.shutter);
        if (!was_manual || wanted != (long)exposure_time(image.shutter)) {
            char shutter[16];
            snprintf(shutter, sizeof(shutter), "1/%u", shutter_denominator(wanted));
            changes.set("image", "shutter", shutter);
        }
    }
}

inline void image_adjustment_ex_changes(const Field *fields, size_t count, const Image &image, Changes &changes)
{
    const Field *once = find(fields, count, "onceAWB");
    if (once != NULL && once->value == 1) changes.one_push = true;
    // mount.orientation only has upright and inverted, so only mirror and flip together map
    const Field *mirror = find(fields, count, "mirror");
    const Field *flip = find(fields, count, "flip");
    const long mirrored = mirror != NULL ? mirror->value : (long)image.inverted;
    const long flipped = flip != NULL ? flip->value : (long)image.inverted;
    if ((mirror != NULL || flip != NULL) && mirrored == flipped && (mirrored == 1) != image.inverted)
        changes.set("mount", "orientation", mirrored == 1 ? "inverted" : "upright");
}

/* A resolution from the target's list: the requested size, else one of the
 * same height (the extension's recommended sub streams are 640x480). */
inline const char *pick_resolution(long width, long height, const char *const *resolutions, size_t count)
{
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            unsigned w, h;
            if (sscanf(resolutions[i], "%ux%u", &w, &h) == 2 && (long)h == height &&
                (pass == 1 || (long)w == width)) return resolutions[i];
        }
    }
    return NULL;
}

inline void venc_changes(const Field *fields, size_t count, const Stream &stream,
                         const char *const *resolutions, size_t resolution_count,
                         const char *section, Changes &changes)
{
    const Field *encode = find(fields, count, "encode_type");
    if (encode != NULL && (encode->value == 1 || encode->value == 5) && (encode->value == 5) != stream.h265)
        changes.set(section, "codec", encode->value == 5 ? "h265" : "h264");

    const Field *width = find(fields, count, "pic_width");
    const Field *height = find(fields, count, "pic_height");
    if (width != NULL && height != NULL &&
        (width->value != (long)stream.width || height->value != (long)stream.height)) {
        const char *chosen = pick_resolution(width->value, height->value, resolutions, resolution_count);
        char current[24];
        snprintf(current, sizeof(current), "%ux%u", stream.width, stream.height);
        if (chosen != NULL && strcmp(chosen, current) != 0) changes.set(section, "resolution", chosen);
    }

    if (!stream.configurable) return;
    const Field *bitrate = find(fields, count, "bitrate");
    if (bitrate != NULL && bitrate->value != (long)stream.bitrate_kbps)
        changes.set_number(section, "bitrate_kbps", clamp(bitrate->value, 1, stream.max_bitrate_kbps));
    const Field *rate = find(fields, count, "frame_rate");
    if (rate != NULL) {
        const long value = clamp(rate->value, stream.min_frame_rate, (long)stream.max_frame_rate);
        if (value != (long)stream.frame_rate) changes.set_number(section, "frame_rate", value);
    }
}

}  // namespace apc_fourkcam
