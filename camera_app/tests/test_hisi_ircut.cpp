#include "ircut.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static std::vector<std::string> events;
// last level written to each value file
static std::map<std::string, std::string> levels;
static unsigned writes;
static unsigned fail_at;
static bool fail_applies;
static const char *busy_suffix;
// when set, each sleep also records both pins' value files under it
static const char *watch_root;

static bool ends_with(const std::string &text, const char *suffix)
{
    const size_t length = strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

static int record_write(const char *path, const char *value)
{
    events.push_back(std::string("write ") + path + " " + value);
    const bool fail = writes++ == fail_at;
    if (busy_suffix != nullptr && ends_with(path, busy_suffix)) {
        errno = EBUSY;
        return -1;
    }
    if ((!fail || fail_applies) && ends_with(path, "/value")) levels[path] = value;
    if (fail) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static std::string read_file(const std::string &path)
{
    char buffer[32] = {};
    FILE *file = fopen(path.c_str(), "r");
    assert(file != nullptr);
    const size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    return std::string(buffer, length);
}

static void record_sleep(unsigned ms)
{
    std::string event = "sleep " + std::to_string(ms);
    if (watch_root != nullptr) {
        event += " " + read_file(std::string(watch_root) + "/gpio5/value") +
                 read_file(std::string(watch_root) + "/gpio6/value");
    }
    events.push_back(event);
}

static ircut_gpio recorded(unsigned fail, bool applies)
{
    events.clear();
    levels.clear();
    writes = 0;
    fail_at = fail;
    fail_applies = applies;
    busy_suffix = nullptr;
    ircut_gpio gpio = ircut_camera_gpio;
    gpio.write = record_write;
    gpio.sleep_ms = record_sleep;
    return gpio;
}

static std::vector<std::string> camera_pulse(unsigned high)
{
    const std::string value = "write /sys/class/gpio/gpio" + std::to_string(high) + "/value ";
    return {
        "write /sys/class/gpio/export 72",
        "write /sys/class/gpio/gpio72/direction out",
        "write /sys/class/gpio/gpio72/value 0",
        "write /sys/class/gpio/export 73",
        "write /sys/class/gpio/gpio73/direction out",
        "write /sys/class/gpio/gpio73/value 0",
        "sleep 200",
        value + "1",
        "sleep 200",
        value + "0",
    };
}

static void assert_low(void)
{
    for (const auto &level : levels) assert(level.second == "0");
}

static bool raised(void)
{
    for (const std::string &event : events) {
        if (ends_with(event, "/value 1")) return true;
    }
    return false;
}

static void write_file(const std::string &path, const char *value)
{
    FILE *file = fopen(path.c_str(), "w");
    assert(file != nullptr && fputs(value, file) >= 0 && fclose(file) == 0);
}

static void sysfs_files(void)
{
    char root[] = "/tmp/test_hisi_ircut.XXXXXX";
    assert(mkdtemp(root) != nullptr);
    const std::string base = root;
    write_file(base + "/export", "");
    for (const char *pin : {"/gpio5", "/gpio6"}) {
        assert(mkdir((base + pin).c_str(), 0700) == 0);
        write_file(base + pin + "/direction", "in");
        write_file(base + pin + "/value", "1");
    }

    ircut_gpio gpio = recorded(~0U, false);
    gpio = {root, 5, 6, 200, nullptr, record_sleep};
    watch_root = root;
    assert(ircut_pulse_night(&gpio) == 0);
    watch_root = nullptr;
    assert((events == std::vector<std::string>{"sleep 200 00", "sleep 200 01"}));
    assert(read_file(base + "/export") == "6");
    for (const char *pin : {"/gpio5", "/gpio6"}) {
        assert(read_file(base + pin + "/direction") == "out");
        assert(read_file(base + pin + "/value") == "0");
    }

    for (const char *pin : {"/gpio5", "/gpio6"}) {
        assert(unlink((base + pin + "/direction").c_str()) == 0);
        assert(unlink((base + pin + "/value").c_str()) == 0);
        assert(rmdir((base + pin).c_str()) == 0);
    }
    assert(unlink((base + "/export").c_str()) == 0);
    assert(rmdir(root) == 0);

    // no sysfs GPIO at all
    gpio = recorded(~0U, false);
    gpio.root = "/nonexistent/test_hisi_ircut";
    gpio.write = nullptr;
    errno = 0;
    assert(ircut_pulse_day(&gpio) == -1 && errno == ENOENT && events.empty());
}

int main()
{
    ircut_gpio gpio = recorded(~0U, false);
    assert(ircut_pulse_day(&gpio) == 0 && events == camera_pulse(72));
    assert(levels.size() == 2);
    assert_low();

    gpio = recorded(~0U, false);
    assert(ircut_pulse_night(&gpio) == 0 && events == camera_pulse(73));
    assert(levels.size() == 2);
    assert_low();

    // already exported
    gpio = recorded(~0U, false);
    busy_suffix = "/export";
    assert(ircut_pulse_day(&gpio) == 0 && events == camera_pulse(72));
    assert_low();

    // only export may report EBUSY
    gpio = recorded(~0U, false);
    busy_suffix = "/gpio73/direction";
    errno = 0;
    assert(ircut_pulse_day(&gpio) == -1 && errno == EBUSY && !raised());
    assert_low();

    // a failed first export touches nothing else
    gpio = recorded(0, false);
    assert(ircut_pulse_day(&gpio) == -1 && events.size() == 1U);

    // every write failing, with and without taking effect, ends with both pins
    // low; a setup failure never raises a pin
    const unsigned pulse_writes = 8;
    for (unsigned fail = 0; fail < pulse_writes; fail++) {
        for (bool applies : {false, true}) {
            for (int night = 0; night < 2; night++) {
                gpio = recorded(fail, applies);
                errno = 0;
                const int result = night ? ircut_pulse_night(&gpio) : ircut_pulse_day(&gpio);
                assert(result == -1 && errno == EIO);
                assert_low();
                if (fail < 6U) assert(!raised());
            }
        }
    }

    sysfs_files();
    puts("test_hisi_ircut: ok");
    return 0;
}
