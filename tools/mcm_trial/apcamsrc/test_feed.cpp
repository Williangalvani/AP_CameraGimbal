/* Host stand-in for camera-app: publishes an Annex B file with access unit
 * delimiters on camera-app's frame socket, looping, in real time.
 * Usage: test_feed SOCKET FILE WIDTH HEIGHT [FPS] [h264|h265] */
#include "camera_app/frame_socket.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

static volatile sig_atomic_t stopping;

static void on_signal(int) { stopping = 1; }

static void on_keyframe(void *, unsigned stream)
{
    printf("keyframe request stream %u\n", stream);
    fflush(stdout);
}

static size_t start_code(const std::vector<uint8_t> &data, size_t i)
{
    if (i + 3 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return 3;
    if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
        data[i + 3] == 1) return 4;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s SOCKET FILE WIDTH HEIGHT [FPS] [h264|h265]\n", argv[0]);
        return 2;
    }
    const unsigned fps = argc > 5 ? (unsigned)atoi(argv[5]) : 30U;
    const bool h265 = argc > 6 && strcmp(argv[6], "h265") == 0;
    FILE *file = fopen(argv[2], "rb");
    if (file == NULL) return 1;
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) data.insert(data.end(), chunk, chunk + got);
    fclose(file);

    // split before each access unit delimiter
    std::vector<size_t> units;
    std::vector<bool> keys;
    for (size_t i = 0; i < data.size(); i++) {
        const size_t code = start_code(data, i);
        if (code == 0) continue;
        const uint8_t header = data[i + code];
        const unsigned type = h265 ? (header >> 1) & 0x3f : header & 0x1f;
        if (type == (h265 ? 35U : 9U)) {
            units.push_back(i);
            keys.push_back(false);
        } else if (!keys.empty() && (h265 ? type >= 16U && type <= 21U : type == 5U)) {
            keys.back() = true;
        }
        i += code;
    }
    if (units.empty()) {
        fprintf(stderr, "no access unit delimiters in %s\n", argv[2]);
        return 1;
    }
    units.push_back(data.size());

    struct ca_frame_socket *server = NULL;
    if (ca_frame_socket_open(&server, argv[1], on_keyframe, NULL) < 0 ||
        ca_frame_socket_configure(server, 0U, h265 ? CA_VIDEO_H265 : CA_VIDEO_H264,
                                  (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]), fps) < 0) {
        perror("frame socket");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (size_t n = 0; !stopping; n++) {
        const size_t i = n % (units.size() - 1);
        const uint64_t pts = (uint64_t)next.tv_sec * 1000000U + (uint64_t)next.tv_nsec / 1000U;
        ca_frame_socket_publish(server, 0U, data.data() + units[i], units[i + 1] - units[i], pts,
                                keys[i]);
        next.tv_nsec += 1000000000L / fps;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_sec++;
            next.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    ca_frame_socket_close(server);
    return 0;
}
