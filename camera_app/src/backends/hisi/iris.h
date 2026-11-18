#ifndef CAMERA_APP_HISI_IRIS_H
#define CAMERA_APP_HISI_IRIS_H

#include "apcam/hisi_firmware.h"
#include "apcam/hisi_iris.h"

#include <stdint.h>

/* DC iris: the U35.5 board's lens (device code 19/2/51) has an iris that stays
 * shut, and the picture black, until PWM0 holds it open. The PWM needs its
 * clock and its pad set through /dev/mem before the ISP drives it through
 * /dev/pwm. Other boards are left alone. The web makes the same decision
 * (apcam/hisi_iris.h) until camera-app publishes its own. */
constexpr apcam_hisi_firmware::device_code iris_device_code = apcam_hisi_iris::DEVICE_CODE;

struct iris_register {
    const char *name;
    uint32_t address;   // physical
    uint32_t keep;      // bits kept from the value read back
    uint32_t value;
};

// the clock first: without it the PWM ignores writes
extern const struct iris_register iris_registers[2];

uint32_t iris_register_value(const struct iris_register *reg, uint32_t current);

struct iris_decision {
    bool enable;
    char reason[96];
};

// override: CAMERA_APP_DC_IRIS, NULL when unset; code: NULL when
// updatetype.ini is missing or has no device code
struct iris_decision iris_decide(const char *override, const apcam_hisi_firmware::device_code *code);

// 0, or -1 with errno after logging; mem is /dev/mem but for tests
int iris_write_registers(const char *mem = "/dev/mem");

/* Logs whether the DC iris is enabled and why, and if so sets up PWM0. A
 * register failure is only logged. True when the ISP should drive the iris. */
bool iris_start(const char *updatetype = "/appfs/fconfigs/param/updatetype.ini",
                const char *mem = "/dev/mem");

/* Writes "<ready>.iris" through a temporary file and a rename; ready NULL is
 * CAMERA_APP_READY_PATH or /run/camera-app.ready. 0, or -1 with errno. */
int iris_write_status(const struct apcam_hisi_iris::status &status, const char *ready = NULL);

#endif
