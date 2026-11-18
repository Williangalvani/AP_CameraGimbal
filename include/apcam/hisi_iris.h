#pragma once
#include "hisi_firmware.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The DC iris as camera-app found it and the iris priority it applied, for
// the web UI: camera-app writes "<ready path>.iris" (temp file, then rename)
// once its media starts and after every image apply. Shared by camera-app's
// hisi backend and the web, so no SDK types.
namespace apcam_hisi_iris {

// U35.5 board, whose lens has a DC iris
constexpr apcam_hisi_firmware::device_code DEVICE_CODE = {19, 2, 51};
constexpr const char *STATUS_SUFFIX = ".iris";

// CAMERA_APP_DC_IRIS: "1" or "0" decides; anything else is ignored.
inline bool override_valid(const char *override)
{
    return override != nullptr && (std::strcmp(override, "1") == 0 || std::strcmp(override, "0") == 0);
}

// Whether camera-app sets up the DC iris. override: NULL when unset; code:
// NULL when updatetype.ini has none.
inline bool dc_iris_expected(const char *override, const apcam_hisi_firmware::device_code *code)
{
    if (override_valid(override)) return override[0] == '1';
    return code != nullptr && code->dev == DEVICE_CODE.dev && code->sdk == DEVICE_CODE.sdk &&
           code->platform == DEVICE_CODE.platform;
}

// The shutters are 0 when iris priority is not applied.
struct status {
    long pid;
    bool dc_iris;
    bool priority_active;
    uint32_t shutter_requested_us;
    uint32_t shutter_applied_us;
};

inline int format_status(char *buffer, size_t size, const status &s)
{
    return std::snprintf(buffer, size,
                         "pid=%ld\ndc_iris=%d\npriority_active=%d\nshutter_requested_us=%lu\n"
                         "shutter_applied_us=%lu\n",
                         s.pid, s.dc_iris ? 1 : 0, s.priority_active ? 1 : 0,
                         (unsigned long)s.shutter_requested_us, (unsigned long)s.shutter_applied_us);
}

// key=value lines; unknown keys are skipped. False without a pid and dc_iris,
// or with a malformed value.
inline bool parse_status(const char *text, status *s)
{
    status found = {0, false, false, 0U, 0U};
    unsigned seen = 0;
    while (text != nullptr && *text != '\0') {
        const char *end = std::strchr(text, '\n');
        const size_t length = end != nullptr ? (size_t)(end - text) : std::strlen(text);
        const char *equals = (const char *)std::memchr(text, '=', length);
        if (equals != nullptr) {
            const size_t key_length = (size_t)(equals - text);
            char value[24];
            const size_t value_length = length - key_length - 1U;
            if (value_length == 0 || value_length >= sizeof(value)) return false;
            std::memcpy(value, equals + 1, value_length);
            value[value_length] = '\0';
            if (value[value_length - 1] == '\r') value[value_length - 1] = '\0';
            char *rest;
            const unsigned long number = std::strtoul(value, &rest, 10);
            const bool numeric = value[0] >= '0' && value[0] <= '9' && *rest == '\0';
            const auto is = [&](const char *key) {
                return std::strlen(key) == key_length && std::strncmp(text, key, key_length) == 0;
            };
            const auto flag = [&](bool *destination) {
                if (!numeric || number > 1U) return false;
                *destination = number == 1U;
                return true;
            };
            if (is("pid")) {
                if (!numeric || number == 0U || number > 0x7fffffffUL) return false;
                found.pid = (long)number;
                seen |= 1U;
            } else if (is("dc_iris")) {
                if (!flag(&found.dc_iris)) return false;
                seen |= 2U;
            } else if (is("priority_active")) {
                if (!flag(&found.priority_active)) return false;
            } else if (is("shutter_requested_us") || is("shutter_applied_us")) {
                if (!numeric || number > 0xffffffffUL) return false;
                (is("shutter_requested_us") ? found.shutter_requested_us : found.shutter_applied_us) =
                    (uint32_t)number;
            }
        }
        text = end != nullptr ? end + 1 : nullptr;
    }
    if (seen != 3U) return false;
    *s = found;
    return true;
}

// N of "1/N s", to the nearest whole number; 0 for 0
inline unsigned shutter_denominator(uint32_t us)
{
    return us == 0U ? 0U : (unsigned)((1000000UL + us / 2U) / us);
}

}  // namespace apcam_hisi_iris
