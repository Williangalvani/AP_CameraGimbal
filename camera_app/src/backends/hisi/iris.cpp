#include "iris.h"

#include "camera_app/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

using apcam_hisi_firmware::device_code;

const struct iris_register iris_registers[2] = {
    {"PWM clock", 0x11014598U, 0xffffff00U, 0x10U},
    {"PWM0 pin mux", 0x10260058U, 0U, 0x1105U},
};

uint32_t iris_register_value(const struct iris_register *reg, uint32_t current)
{
    return (current & reg->keep) | reg->value;
}

struct iris_decision iris_decide(const char *override, const device_code *code)
{
    struct iris_decision decision = {apcam_hisi_iris::dc_iris_expected(override, code), ""};
    char *reason = decision.reason;
    size_t size = sizeof(decision.reason);

    if (apcam_hisi_iris::override_valid(override)) {
        (void)snprintf(reason, size, "CAMERA_APP_DC_IRIS=%s", override);
        return decision;
    }
    if (override != NULL) {
        const int length = snprintf(reason, size, "CAMERA_APP_DC_IRIS=%.16s ignored; ", override);
        reason += length;
        size -= (size_t)length;
    }
    if (code == NULL) {
        (void)snprintf(reason, size, "no device code in updatetype.ini");
        return decision;
    }
    (void)snprintf(reason, size, decision.enable ? "device code %u/%u/%u (U35.5)" : "device code %u/%u/%u",
                   (unsigned)code->dev, (unsigned)code->sdk, (unsigned)code->platform);
    return decision;
}

static int write_register(int fd, const struct iris_register *reg)
{
    const long page = sysconf(_SC_PAGESIZE);
    const off_t base = (off_t)(reg->address & ~(uint32_t)(page - 1));
    void *map = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (map == MAP_FAILED) return -1;
    volatile uint32_t *word = (volatile uint32_t *)((char *)map + (reg->address - base));
    *word = iris_register_value(reg, *word);
    (void)munmap(map, (size_t)page);
    return 0;
}

int iris_write_registers(const char *mem)
{
    const int fd = open(mem, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        const int saved = errno;
        ca_log("DC iris: opening %s failed: %s", mem, strerror(saved));
        errno = saved;
        return -1;
    }
    int result = 0;
    for (const struct iris_register &reg : iris_registers) {
        result = write_register(fd, &reg);
        if (result < 0) {
            const int saved = errno;
            ca_log("DC iris: mapping %s at 0x%08x failed: %s", reg.name, (unsigned)reg.address,
                   strerror(saved));
            errno = saved;
            break;
        }
    }
    const int saved = errno;
    (void)close(fd);
    errno = saved;
    return result;
}

// NULL when unset or empty, as main.cpp reads the CAMERA_APP_* variables
static const char *environment_string(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : NULL;
}

bool iris_start(const char *updatetype, const char *mem)
{
    device_code code;
    const bool found = apcam_hisi_firmware::read_device_code(updatetype, &code);
    const struct iris_decision decision =
        iris_decide(environment_string("CAMERA_APP_DC_IRIS"), found ? &code : NULL);
    ca_log("DC iris %s: %s", decision.enable ? "enabled" : "skipped", decision.reason);
    if (decision.enable) (void)iris_write_registers(mem);
    return decision.enable;
}

int iris_write_status(const struct apcam_hisi_iris::status &status, const char *ready)
{
    if (ready == NULL) ready = environment_string("CAMERA_APP_READY_PATH");
    if (ready == NULL) ready = "/run/camera-app.ready";
    char path[4096], temporary[sizeof(path) + 4], contents[192];
    if (snprintf(path, sizeof(path), "%s%s", ready, apcam_hisi_iris::STATUS_SUFFIX) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    (void)apcam_hisi_iris::format_status(contents, sizeof(contents), status);
    FILE *file = fopen(temporary, "w");
    if (file == NULL) return -1;
    bool ok = fputs(contents, file) >= 0;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temporary, path) < 0) {
        const int saved = errno != 0 ? errno : EIO;
        (void)unlink(temporary);
        errno = saved;
        return -1;
    }
    return 0;
}
