#pragma once
/* The camera protocol of Blue Robotics' 4K Cam Manager, as the vendor's xipc
 * answers it: POST /action/<name> with a flat JSON object. Names, ranges and
 * enums follow the extension's br4kcam_commands types; the settings are this
 * firmware's camera.ini values (docs/4kcam-manager-compat.md). A field sent
 * with the value reads report changes nothing, so a client echoing a whole
 * reply back doesn't move settings that don't map exactly. */
#include "APC_StringBuffer.h"
#include <climits>
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

constexpr unsigned max_depth = 16;

/* Past any JSON value starting at p, nested ones included: scalars end at the
 * next ',' or '}' of the enclosing object. NULL if it doesn't end, nests
 * deeper than max_depth or closes a bracket of the other kind. */
inline const char *skip_value(const char *p, const char *end)
{
    char closers[max_depth];
    unsigned depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skip_string(p, end);
            if (p == NULL) return NULL;
            if (depth == 0U) return p;
            continue;
        }
        if (*p == '{' || *p == '[') {
            if (depth == max_depth) return NULL;
            closers[depth++] = *p == '{' ? '}' : ']';
        } else if (*p == '}' || *p == ']') {
            if (depth == 0U) return p;
            if (*p != closers[--depth]) return NULL;
            if (depth == 0U) return p + 1;
        } else if (depth == 0U && *p == ',') {
            return p;
        }
        p++;
    }
    return depth == 0U ? p : NULL;
}

/* The scalar from p to end, spaces after it allowed: true, false or null, or
 * a number -digits[.digits] (the fraction truncated, overflow saturated). */
inline bool parse_scalar(const char *p, const char *end, bool *number, long *value)
{
    *number = false;
    *value = 0;
    static const char *const words[] = {"true", "false", "null"};
    for (const char *word : words) {
        const size_t length = strlen(word);
        if ((size_t)(end - p) >= length && memcmp(p, word, length) == 0) return skip_space(p + length, end) == end;
    }
    const bool negative = p < end && *p == '-';
    if (negative) p++;
    const char *digits = p;
    long magnitude = 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++)
        magnitude = magnitude > (LONG_MAX - 9) / 10 ? LONG_MAX : magnitude * 10 + (*p - '0');
    if (p == digits) return false;
    if (p < end && *p == '.') {
        const char *fraction = ++p;
        while (p < end && *p >= '0' && *p <= '9') p++;
        if (p == fraction) return false;
    }
    if (skip_space(p, end) != end) return false;
    *number = true;
    *value = negative ? -magnitude : magnitude;
    return true;
}

/* One flat JSON object. Numbers are kept; strings, nested values and
 * literals are checked and skipped, so a whole echoed reply fits. More
 * numbers than capacity is an error; keys too long for Field are ignored. An
 * empty body is an empty object: reads have no body. The text needn't be
 * NUL-terminated. */
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
        if (value_end == NULL || value_end == p) return false;
        bool number = false;
        long value = 0;
        if (*p != '"' && *p != '{' && *p != '[' && !parse_scalar(p, value_end, &number, &value)) return false;
        if (number && key_length < sizeof(fields[0].key)) {
            if (*count == capacity) return false;
            Field &field = fields[(*count)++];
            memcpy(field.key, key, key_length);
            field.key[key_length] = '\0';
            field.number = number;
            field.value = value;
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

/* A field of a 0/1 enum; other values are as if it wasn't sent. */
inline const Field *find_flag(const Field *fields, size_t count, const char *key)
{
    const Field *field = find(fields, count, key);
    return field != NULL && (field->value == 0 || field->value == 1) ? field : NULL;
}

inline long clamp(long value, long low, long high)
{
    return value < low ? low : value > high ? high : value;
}

inline long u16(long value) { return clamp(value, 0, 65535); }

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

/* channel is the extension's 0–2 enum and the sizes and rates its u16s, so
 * the reply always deserializes. */
inline void venc(APC_StringBuffer &out, long channel, const Stream &stream,
                 const char *const *resolutions, size_t count)
{
    out.appendf("\"channel\":%ld,\"encode_type\":%d,\"encode_profile\":2,\"pixel_list\":[",
                clamp(channel, 0, 2), stream.h265 ? 5 : 1);
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        unsigned width, height;
        if (sscanf(resolutions[i], "%ux%u", &width, &height) != 2) continue;
        out.appendf("%s{\"width\":%ld,\"height\":%ld}", first ? "" : ",", u16(width), u16(height));
        first = false;
    }
    out.appendf("],\"pic_width\":%ld,\"pic_height\":%ld,\"rc_mode\":1,\"bitrate\":%ld,"
                "\"max_framerate\":%ld,\"frame_rate\":%ld,\"gop\":%ld,",
                u16(stream.width), u16(stream.height), u16(stream.bitrate_kbps), u16(stream.max_frame_rate),
                u16(stream.frame_rate), u16(stream.frame_rate * 2L));
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
        if (field != NULL && field->value != level_from_percent(level.percent) &&
            percent_from_level(field->value) != level.percent)
            changes.set_number("image", level.key, percent_from_level(field->value));
    }

    const size_t before_gains = changes.count;
    const Field *red = find(fields, count, "awb_red");
    const Field *green = find(fields, count, "awb_green");
    const Field *blue = find(fields, count, "awb_blue");
    if (red != NULL && clamp(red->value, 0, 255) != red_level(image.wb_red))
        changes.set_number("image", "wb_red", red_gain(red->value));
    if (green != NULL && clamp(green->value, 0, 255) != green_blue_level(image.wb_green))
        changes.set_number("image", "wb_green", green_blue_gain(green->value));
    if (blue != NULL && clamp(blue->value, 0, 255) != green_blue_level(image.wb_blue))
        changes.set_number("image", "wb_blue", green_blue_gain(blue->value));
    const bool gains_changed = changes.count > before_gains;

    // auto_awb 1 means "not auto": a preset or colour temperature stays, unless the gains move
    const Field *awb = find_flag(fields, count, "auto_awb");
    const bool automatic = strcmp(image.white_balance, "auto") == 0;
    if (awb != NULL && awb->value == 0) {
        if (!automatic) changes.set("image", "white_balance", "auto");
    } else if ((awb != NULL && awb->value == 1 && automatic) ||
               (gains_changed && strcmp(image.white_balance, "manual") != 0)) {
        changes.set("image", "white_balance", "manual");
    }

    const Field *water = find_flag(fields, count, "awb_auto_mode");
    if (water != NULL && (water->value == 1) != image.blue_water)
        changes.set("image", "wb_water", water->value == 1 ? "blue" : "green");

    const Field *mode = find_flag(fields, count, "auto_exposureEx");
    const Field *time = find(fields, count, "exposure_time");
    const bool was_manual = strcmp(image.shutter, "auto") != 0;
    const bool manual = mode != NULL ? mode->value == 1 : was_manual;
    if (!manual && was_manual) {
        changes.set("image", "shutter", "auto");
    } else if (manual) {
        const long wanted = time != NULL ? time->value : (long)exposure_time(image.shutter);
        char shutter[16];
        snprintf(shutter, sizeof(shutter), "1/%u", shutter_denominator(wanted));
        if (!was_manual || (wanted != (long)exposure_time(image.shutter) && strcmp(shutter, image.shutter) != 0))
            changes.set("image", "shutter", shutter);
    }
}

/* The [image] keys image_adjustment_changes writes; set_default 1 resets them. */
constexpr const char *image_adjustment_keys[] = {"brightness", "contrast", "saturation", "white_balance", "wb_red",
                                                 "wb_green", "wb_blue", "wb_water", "shutter"};

inline bool resets(const Field *fields, size_t count)
{
    const Field *field = find(fields, count, "set_default");
    return field != NULL && field->value == 1;
}

inline void image_adjustment_ex_changes(const Field *fields, size_t count, const Image &image, Changes &changes)
{
    const Field *once = find(fields, count, "onceAWB");
    if (once != NULL && once->value == 1) changes.one_push = true;
    /* mount.orientation only has upright and inverted (mirror and flip both),
     * and the extension sends mirror or flip alone: either one that differs
     * from it turns the image. */
    const Field *mirror = find_flag(fields, count, "mirror");
    const Field *flip = find_flag(fields, count, "flip");
    if ((mirror != NULL && (mirror->value == 1) != image.inverted) ||
        (flip != NULL && (flip->value == 1) != image.inverted))
        changes.set("mount", "orientation", image.inverted ? "upright" : "inverted");
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
    // a maximum of 0 means the bitrate isn't a setting
    const Field *bitrate = find(fields, count, "bitrate");
    if (bitrate != NULL && stream.max_bitrate_kbps > 0) {
        const long value = clamp(bitrate->value, 1, stream.max_bitrate_kbps);
        if (bitrate->value != (long)stream.bitrate_kbps && value != (long)stream.bitrate_kbps)
            changes.set_number(section, "bitrate_kbps", value);
    }
    const Field *rate = find(fields, count, "frame_rate");
    if (rate != NULL) {
        const long value = clamp(rate->value, stream.min_frame_rate, (long)stream.max_frame_rate);
        if (rate->value != (long)stream.frame_rate && value != (long)stream.frame_rate)
            changes.set_number(section, "frame_rate", value);
    }
}

}  // namespace apc_fourkcam
