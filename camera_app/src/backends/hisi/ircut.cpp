#include "ircut.h"

#include "camera_app/log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const struct ircut_gpio ircut_camera_gpio = {"/sys/class/gpio", 72U, 73U, 200U, NULL, NULL};

// pulses must not overlap, and exit waits for one in progress to end low
static pthread_mutex_t pulse_lock = PTHREAD_MUTEX_INITIALIZER;

static int sysfs_write(const char *path, const char *value)
{
    const size_t length = strlen(value);
    const int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    const ssize_t written = write(fd, value, length);
    const int saved = errno;
    (void)close(fd);
    if (written == (ssize_t)length) return 0;
    errno = written < 0 ? saved : EIO;
    return -1;
}

static void sleep_ms(unsigned ms)
{
    struct timespec delay = {.tv_sec = ms / 1000U, .tv_nsec = (long)(ms % 1000U) * 1000000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

// a failure with expected_errno is returned but not logged
static int write_attribute(const struct ircut_gpio *gpio, const char *path, const char *value,
                           int expected_errno = 0)
{
    if ((gpio->write != NULL ? gpio->write : sysfs_write)(path, value) == 0) return 0;
    const int saved = errno;
    if (saved != expected_errno) {
        ca_log("IR-cut: writing %s to %s failed: %s", value, path, strerror(saved));
    }
    errno = saved;
    return -1;
}

static int write_pin(const struct ircut_gpio *gpio, unsigned pin, const char *attribute,
                     const char *value)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/gpio%u/%s", gpio->root, pin, attribute) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return write_attribute(gpio, path, value);
}

static int export_pin(const struct ircut_gpio *gpio, unsigned pin)
{
    char path[PATH_MAX];
    char value[16];
    if (snprintf(path, sizeof(path), "%s/export", gpio->root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)snprintf(value, sizeof(value), "%u", pin);
    // EBUSY: already exported
    if (write_attribute(gpio, path, value, EBUSY) == 0 || errno == EBUSY) return 0;
    return -1;
}

static int pulse(const struct ircut_gpio *gpio, unsigned pin)
{
    void (*const sleep)(unsigned) = gpio->sleep_ms != NULL ? gpio->sleep_ms : sleep_ms;
    const unsigned pins[2] = {gpio->day_pin, gpio->night_pin};
    bool exported[2] = {false, false};
    int result = 0;

    pthread_mutex_lock(&pulse_lock);
    for (unsigned i = 0; i < 2U && result == 0; i++) {
        result = export_pin(gpio, pins[i]);
        exported[i] = result == 0;
        if (result == 0) result = write_pin(gpio, pins[i], "direction", "out");
        if (result == 0) result = write_pin(gpio, pins[i], "value", "0");
    }
    if (result == 0) {
        sleep(gpio->pulse_ms);
        result = write_pin(gpio, pin, "value", "1");
        if (result == 0) {
            sleep(gpio->pulse_ms);
            result = write_pin(gpio, pin, "value", "0");
        }
    }
    if (result < 0) {
        const int saved = errno;
        // a write that failed may still have taken effect
        for (unsigned i = 0; i < 2U; i++) {
            if (exported[i]) (void)write_pin(gpio, pins[i], "value", "0");
        }
        errno = saved;
    }
    pthread_mutex_unlock(&pulse_lock);
    return result;
}

int ircut_pulse_day(const struct ircut_gpio *gpio)
{
    return pulse(gpio, gpio->day_pin);
}

int ircut_pulse_night(const struct ircut_gpio *gpio)
{
    return pulse(gpio, gpio->night_pin);
}

static void *day_thread(void *)
{
    if (ircut_pulse_day() == 0) ca_log("IR-cut filter set to day");
    else ca_log("IR-cut day pulse failed; filter position unknown");
    return NULL;
}

// a pin stays where sysfs left it after the process exits
static void wait_for_pulse(void)
{
    pthread_mutex_lock(&pulse_lock);
}

static void start_day(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    if (atexit(wait_for_pulse) != 0 || pthread_attr_init(&attr) != 0) {
        ca_log("IR-cut day pulse not started");
        return;
    }
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int error = pthread_create(&thread, &attr, day_thread, NULL);
    (void)pthread_attr_destroy(&attr);
    if (error != 0) ca_log("IR-cut day pulse not started: %s", strerror(error));
}

void ircut_start_day(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)pthread_once(&once, start_day);
}
