#include "iris.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using apcam_hisi_firmware::device_code;

static const uint32_t clock_address = 0x11014598U;
static const uint32_t pinmux_address = 0x10260058U;

static bool contains(const char *text, const char *part)
{
    return strstr(text, part) != nullptr;
}

static void registers(void)
{
    assert(strcmp(iris_registers[0].name, "PWM clock") == 0);
    assert(iris_registers[0].address == clock_address);
    assert(strcmp(iris_registers[1].name, "PWM0 pin mux") == 0);
    assert(iris_registers[1].address == pinmux_address);

    const iris_register *clock = &iris_registers[0];
    assert(iris_register_value(clock, 0U) == 0x10U);
    assert(iris_register_value(clock, 0xabcd1234U) == 0xabcd1210U);
    assert(iris_register_value(clock, 0xffffffffU) == 0xffffff10U);
    assert(iris_register_value(clock, 0x10U) == 0x10U);

    const iris_register *pinmux = &iris_registers[1];
    assert(iris_register_value(pinmux, 0x1200U) == 0x1105U);
    assert(iris_register_value(pinmux, 0xffffffffU) == 0x1105U);
    assert(iris_register_value(pinmux, 0U) == 0x1105U);
}

static void decisions(void)
{
    const device_code u35_5 = {19, 2, 51}, main_v9 = {19, 2, 35}, other_dev = {18, 2, 51};

    iris_decision d = iris_decide(nullptr, &u35_5);
    assert(d.enable && contains(d.reason, "19/2/51"));
    d = iris_decide(nullptr, &main_v9);
    assert(!d.enable && contains(d.reason, "19/2/35"));
    d = iris_decide(nullptr, &other_dev);
    assert(!d.enable && contains(d.reason, "18/2/51"));
    d = iris_decide(nullptr, nullptr);
    assert(!d.enable && contains(d.reason, "updatetype.ini"));

    d = iris_decide("1", &main_v9);
    assert(d.enable && contains(d.reason, "CAMERA_APP_DC_IRIS=1"));
    d = iris_decide("1", nullptr);
    assert(d.enable);
    d = iris_decide("0", &u35_5);
    assert(!d.enable && contains(d.reason, "CAMERA_APP_DC_IRIS=0"));

    // anything else is ignored and the board decides
    d = iris_decide("yes", &u35_5);
    assert(d.enable && contains(d.reason, "ignored") && contains(d.reason, "19/2/51"));
    d = iris_decide("10", &main_v9);
    assert(!d.enable && contains(d.reason, "ignored") && contains(d.reason, "19/2/35"));

    // the web's guess before camera-app reports is camera-app's own decision
    const char *overrides[] = {nullptr, "1", "0", "yes", "10", ""};
    const device_code *codes[] = {&u35_5, &main_v9, &other_dev, nullptr};
    for (const char *override : overrides) {
        for (const device_code *code : codes)
            assert(iris_decide(override, code).enable == apcam_hisi_iris::dc_iris_expected(override, code));
    }
}

static void status_text(void)
{
    using apcam_hisi_iris::status;
    char text[192];
    const status clamped = {1234, true, true, 83333U, 33333U};
    status parsed = {};
    assert(apcam_hisi_iris::format_status(text, sizeof(text), clamped) < (int)sizeof(text));
    assert(!strcmp(text, "pid=1234\ndc_iris=1\npriority_active=1\nshutter_requested_us=83333\n"
                         "shutter_applied_us=33333\n"));
    assert(apcam_hisi_iris::parse_status(text, &parsed));
    assert(parsed.pid == 1234 && parsed.dc_iris && parsed.priority_active);
    assert(parsed.shutter_requested_us == 83333U && parsed.shutter_applied_us == 33333U);

    // CRLF, unknown keys and a missing last newline are fine; absent shutters are 0
    assert(apcam_hisi_iris::parse_status("pid=7\r\nfuture=x\r\ndc_iris=0", &parsed));
    assert(parsed.pid == 7 && !parsed.dc_iris && !parsed.priority_active && parsed.shutter_applied_us == 0U);
    // pid and dc_iris are required; a malformed value rejects the file and leaves *s alone
    parsed.pid = 99;
    const char *bad[] = {"", "dc_iris=1\n", "pid=7\n", "pid=0\ndc_iris=1\n", "pid=-7\ndc_iris=1\n",
                         "pid=7\ndc_iris=2\n", "pid=7\ndc_iris=\n", "pid=7x\ndc_iris=1\n",
                         "pid=7\ndc_iris=1\nshutter_applied_us=+5\n", "pid=7\ndc_iris=1\npriority_active=yes\n",
                         "pid=7\ndc_iris=1\nshutter_requested_us=99999999999999999999999\n"};
    for (const char *text_case : bad) assert(!apcam_hisi_iris::parse_status(text_case, &parsed) && parsed.pid == 99);

    assert(apcam_hisi_iris::shutter_denominator(0U) == 0U);
    assert(apcam_hisi_iris::shutter_denominator(83333U) == 12U && apcam_hisi_iris::shutter_denominator(33333U) == 30U);
    assert(apcam_hisi_iris::shutter_denominator(66666U) == 15U && apcam_hisi_iris::shutter_denominator(16666U) == 60U);
    assert(apcam_hisi_iris::shutter_denominator(40000U) == 25U && apcam_hisi_iris::shutter_denominator(10U) == 100000U);
}

static std::string temp_path(const char *name)
{
    static std::string root;
    if (root.empty()) {
        char pattern[] = "/tmp/test_hisi_iris.XXXXXX";
        assert(mkdtemp(pattern) != nullptr);
        root = pattern;
    }
    return root + "/" + name;
}

static void write_file(const std::string &path, const char *text)
{
    FILE *file = fopen(path.c_str(), "w");
    assert(file != nullptr && fputs(text, file) >= 0 && fclose(file) == 0);
}

static uint32_t peek(const std::string &mem, uint32_t address)
{
    uint32_t value = 0;
    const int fd = open(mem.c_str(), O_RDONLY);
    assert(fd >= 0 && pread(fd, &value, sizeof(value), address) == (ssize_t)sizeof(value));
    close(fd);
    return value;
}

static void poke(const std::string &mem, uint32_t address, uint32_t value)
{
    const int fd = open(mem.c_str(), O_WRONLY);
    assert(fd >= 0 && pwrite(fd, &value, sizeof(value), address) == (ssize_t)sizeof(value));
    close(fd);
}

// a sparse file standing in for /dev/mem, holding the registers' reset values
static std::string fake_mem(void)
{
    const std::string mem = temp_path("mem");
    const int fd = open(mem.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && ftruncate(fd, (clock_address | 0xffffU) + 1U) == 0);
    close(fd);
    poke(mem, clock_address, 0xabcd1200U);
    poke(mem, pinmux_address, 0x1200U);
    return mem;
}

static bool untouched(const std::string &mem)
{
    return peek(mem, clock_address) == 0xabcd1200U && peek(mem, pinmux_address) == 0x1200U;
}

static bool written(const std::string &mem)
{
    return peek(mem, clock_address) == 0xabcd1210U && peek(mem, pinmux_address) == 0x1105U;
}

// iris_start with stderr captured; returns the log
static std::string start(const std::string &ini, const std::string &mem, bool *enable)
{
    const std::string log = temp_path("log");
    fflush(stderr);
    const int saved = dup(2);
    const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(saved >= 0 && fd >= 0 && dup2(fd, 2) == 2);
    close(fd);
    *enable = iris_start(ini.c_str(), mem.c_str());
    fflush(stderr);
    assert(dup2(saved, 2) == 2);
    close(saved);

    std::string text;
    char buffer[512];
    FILE *file = fopen(log.c_str(), "r");
    assert(file != nullptr);
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, length);
    fclose(file);
    unlink(log.c_str());
    return text;
}

static unsigned lines(const std::string &text)
{
    unsigned count = 0;
    for (char c : text) count += c == '\n';
    return count;
}

static void board(void)
{
    const std::string u35_5 = temp_path("u35_5.ini"), main_v9 = temp_path("main_v9.ini");
    const std::string missing = temp_path("missing.ini");
    write_file(u35_5, "[UPDATE_SET]\r\nVER = 18.010.U35.5_0054_V16\r\nDEV_TYPE = 19\r\n"
                      "SDK_TYPE = 2\r\nPLATFORM_TYPE = 51\r\n");
    write_file(main_v9, "[UPDATE_SET]\nVER = 18.010.25.3_MAIN_V9(240601)\nDEV_TYPE = 19\n"
                        "SDK_TYPE = 2\nPLATFORM_TYPE = 35\n");
    bool enable;
    std::string log;

    assert(unsetenv("CAMERA_APP_DC_IRIS") == 0);
    std::string mem = fake_mem();
    log = start(u35_5, mem, &enable);
    assert(enable && written(mem));
    assert(lines(log) == 1U && contains(log.c_str(), "DC iris enabled: device code 19/2/51"));

    mem = fake_mem();
    log = start(main_v9, mem, &enable);
    assert(!enable && untouched(mem));
    assert(lines(log) == 1U && contains(log.c_str(), "DC iris skipped: device code 19/2/35"));

    mem = fake_mem();
    log = start(missing, mem, &enable);
    assert(!enable && untouched(mem));
    assert(lines(log) == 1U && contains(log.c_str(), "DC iris skipped: "));

    // an empty variable is unset, as for the other CAMERA_APP_* variables
    assert(setenv("CAMERA_APP_DC_IRIS", "", 1) == 0);
    mem = fake_mem();
    log = start(main_v9, mem, &enable);
    assert(!enable && untouched(mem));

    assert(setenv("CAMERA_APP_DC_IRIS", "1", 1) == 0);
    mem = fake_mem();
    log = start(missing, mem, &enable);
    assert(enable && written(mem));
    assert(lines(log) == 1U && contains(log.c_str(), "DC iris enabled: CAMERA_APP_DC_IRIS=1"));

    assert(setenv("CAMERA_APP_DC_IRIS", "0", 1) == 0);
    mem = fake_mem();
    log = start(u35_5, mem, &enable);
    assert(!enable && untouched(mem));
    assert(lines(log) == 1U && contains(log.c_str(), "DC iris skipped: CAMERA_APP_DC_IRIS=0"));

    // no /dev/mem: logged, and the ISP may still drive the iris
    assert(unsetenv("CAMERA_APP_DC_IRIS") == 0);
    log = start(u35_5, temp_path("no-mem"), &enable);
    assert(enable && lines(log) == 2U && contains(log.c_str(), "No such file or directory"));
    errno = 0;
    assert(iris_write_registers(temp_path("no-mem").c_str()) == -1 && errno == ENOENT);

    mem = fake_mem();
    assert(iris_write_registers(mem.c_str()) == 0 && written(mem));
    // read-modify-write: a second pass changes nothing
    assert(iris_write_registers(mem.c_str()) == 0 && written(mem));

    for (const std::string &path : {u35_5, main_v9, mem}) assert(unlink(path.c_str()) == 0);
}

static std::string read_text(const std::string &path)
{
    std::string text;
    char buffer[512];
    FILE *file = fopen(path.c_str(), "r");
    assert(file != nullptr);
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, length);
    fclose(file);
    return text;
}

static void status_file(void)
{
    const std::string ready = temp_path("camera-app.ready"), path = ready + ".iris";
    apcam_hisi_iris::status written = {(long)getpid(), true, false, 0U, 0U}, parsed = {};
    assert(iris_write_status(written, ready.c_str()) == 0);
    assert(apcam_hisi_iris::parse_status(read_text(path).c_str(), &parsed));
    assert(parsed.pid == getpid() && parsed.dc_iris && !parsed.priority_active);
    // replaced whole, through a temporary file that doesn't stay
    written = {(long)getpid(), true, true, 40000U, 33333U};
    assert(iris_write_status(written, ready.c_str()) == 0);
    assert(apcam_hisi_iris::parse_status(read_text(path).c_str(), &parsed));
    assert(parsed.priority_active && parsed.shutter_requested_us == 40000U && parsed.shutter_applied_us == 33333U);
    assert(access((path + ".tmp").c_str(), F_OK) != 0);
    assert(unlink(path.c_str()) == 0);

    // by default beside CAMERA_APP_READY_PATH
    assert(setenv("CAMERA_APP_READY_PATH", ready.c_str(), 1) == 0);
    assert(iris_write_status(written) == 0 && access(path.c_str(), F_OK) == 0);
    assert(unlink(path.c_str()) == 0 && unsetenv("CAMERA_APP_READY_PATH") == 0);
    // a directory that doesn't exist: an error, and nothing left behind
    errno = 0;
    assert(iris_write_status(written, temp_path("missing/ready").c_str()) == -1 && errno == ENOENT);
    assert(rmdir(temp_path("").c_str()) == 0);
}

int main()
{
    registers();
    decisions();
    status_text();
    board();
    status_file();
    puts("test_hisi_iris: ok");
    return 0;
}
