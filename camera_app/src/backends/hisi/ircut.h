#ifndef CAMERA_APP_HISI_IRCUT_H
#define CAMERA_APP_HISI_IRCUT_H

/* IR-cut filter: a two-wire motor moved by a pulse on one of two sysfs GPIOs.
 * Both pins are held low for pulse_ms, then one is high for pulse_ms. Neither
 * may be left high: the coil overheats. */
struct ircut_gpio {
    const char *root;
    unsigned day_pin;     // filter in
    unsigned night_pin;   // filter out
    unsigned pulse_ms;
    // NULL selects the sysfs file write and nanosleep
    int (*write)(const char *path, const char *value);
    void (*sleep_ms)(unsigned ms);
};

extern const struct ircut_gpio ircut_camera_gpio;

// 0, or -1 with errno after driving both pins low
int ircut_pulse_day(const struct ircut_gpio *gpio = &ircut_camera_gpio);
int ircut_pulse_night(const struct ircut_gpio *gpio = &ircut_camera_gpio);
// pulses day once per process from a detached thread; failures are only logged
void ircut_start_day(void);

#endif
