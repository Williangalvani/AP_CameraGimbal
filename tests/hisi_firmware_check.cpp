// Host driver for include/apcam/hisi_firmware.h, run by test_hisi_firmware.py:
// hisi_firmware_check IMAGE [updatetype.ini] prints "ok" or the rejection.
#include "apcam/hisi_firmware.h"

#include <vector>

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    apcam_hisi_firmware::device_code expected = apcam_hisi_firmware::CAMERA_DEVICE_CODE;
    if (argc == 3 && !apcam_hisi_firmware::read_device_code(argv[2], &expected)) {
        std::puts("bad updatetype.ini");
        return 1;
    }
    FILE *file = std::fopen(argv[1], "rb");
    if (!file) return 2;
    std::vector<uint8_t> image;
    uint8_t buffer[65536];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) image.insert(image.end(), buffer, buffer + got);
    std::fclose(file);
    char error[256];
    const char *problem = apcam_hisi_firmware::check_appfs_image(image.data(), image.size(), expected,
                                                                  error, sizeof(error));
    std::puts(problem ? problem : "ok");
    return problem ? 1 : 0;
}
