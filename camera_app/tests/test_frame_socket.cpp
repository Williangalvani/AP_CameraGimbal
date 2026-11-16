#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/frame_socket.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

static std::atomic<unsigned> keyframes[CA_FRAME_SOCKET_STREAMS];

static void on_keyframe(void *context, unsigned stream)
{
    (void)context;
    if (stream < CA_FRAME_SOCKET_STREAMS) keyframes[stream]++;
}

static int failures;

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(unsigned ms)
{
    const struct timespec delay = {(time_t)(ms / 1000U), (long)(ms % 1000U) * 1000000L};
    (void)nanosleep(&delay, NULL);
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int connect_to(const char *name, unsigned stream, uint8_t info[CA_FRAME_SOCKET_INFO_SIZE])
{
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path + 1, name + 1, strlen(name) - 1U);
    const socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(name));
    const uint8_t hello[CA_FRAME_SOCKET_HELLO_SIZE] = {'A', 'P', 'V', 'R', CA_FRAME_SOCKET_VERSION,
                                                       (uint8_t)stream};
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (const struct sockaddr *)&address, length) < 0 ||
        send(fd, hello, sizeof(hello), 0) != (ssize_t)sizeof(hello) ||
        recv(fd, info, CA_FRAME_SOCKET_INFO_SIZE, MSG_WAITALL) != (ssize_t)CA_FRAME_SOCKET_INFO_SIZE) {
        close(fd);
        return -1;
    }
    return fd;
}

struct frame {
    uint32_t length;
    uint32_t flags;
    uint64_t pts;
    uint8_t first;
};

static bool read_frame(int fd, struct frame *frame)
{
    uint8_t header[CA_FRAME_SOCKET_HEADER_SIZE];
    if (recv(fd, header, sizeof(header), MSG_WAITALL) != (ssize_t)sizeof(header)) return false;
    frame->length = u32(header);
    frame->flags = u32(header + 4);
    frame->pts = u32(header + 8) | (uint64_t)u32(header + 12) << 32;
    uint8_t *data = (uint8_t *)malloc(frame->length);
    const bool ok = data != NULL &&
                    recv(fd, data, frame->length, MSG_WAITALL) == (ssize_t)frame->length;
    frame->first = ok ? data[4] : 0U;
    free(data);
    return ok;
}

static void publish(struct ca_frame_socket *server, uint8_t tag, size_t length, uint64_t pts,
                    bool key)
{
    uint8_t *data = (uint8_t *)calloc(1, length);
    data[3] = 1U;
    data[4] = tag;
    ca_frame_socket_publish(server, 1U, data, length, pts, key);
    free(data);
}

int main(void)
{
    char name[64];
    snprintf(name, sizeof(name), "@apcam-test-%d", (int)getpid());
    struct ca_frame_socket *server = NULL;
    uint8_t info[CA_FRAME_SOCKET_INFO_SIZE];
    struct frame frame;

    if (ca_frame_socket_open(&server, name, on_keyframe, NULL) < 0 ||
        ca_frame_socket_configure(server, 1U, CA_VIDEO_H265, 1280U, 720U, 30U) < 0) {
        perror("open");
        return 1;
    }

    int fd = connect_to(name, 0U, info);
    check(fd >= 0 && memcmp(info, "APVS", 4) == 0 && info[5] == CA_FRAME_SOCKET_NO_STREAM,
          "an unconfigured stream is refused");
    if (fd >= 0) close(fd);

    fd = connect_to(name, 1U, info);
    check(fd >= 0 && info[5] == CA_FRAME_SOCKET_OK && info[6] == 2U && u16(info + 8) == 1280U &&
              u16(info + 10) == 720U && u16(info + 12) == 30U,
          "the stream's codec, size and rate are announced");
    sleep_ms(50);
    check(keyframes[1] == 1U, "a new client asks for a key frame");

    publish(server, 0x02, 100, 1000, false);
    publish(server, 0x26, 200, 2000, true);
    publish(server, 0x02, 300, 3000, false);
    check(read_frame(fd, &frame) && frame.first == 0x26 && frame.flags == CA_FRAME_KEY &&
              frame.pts == 2000U && frame.length == 200U,
          "the first frame sent is a key frame");
    check(read_frame(fd, &frame) && frame.first == 0x02 && frame.flags == 0U &&
              frame.pts == 3000U && frame.length == 300U,
          "later frames follow in order");

    sleep_ms(300);
    check(send(fd, "K", 1, 0) == 1, "the client can ask for a key frame");
    sleep_ms(250);
    check(keyframes[1] == 2U, "a 'K' from the client reaches the encoder");

    for (unsigned i = 0; i < 40U; i++) publish(server, 0x02, 100000, 4000U + i, false);
    publish(server, 0x26, 1000, 9000, true);
    bool discont_key = false, ordered = true;
    uint64_t last = 3000U;
    while (read_frame(fd, &frame)) {
        ordered = ordered && frame.pts > last;
        last = frame.pts;
        if (frame.flags & CA_FRAME_DISCONT) {
            discont_key = frame.flags == (CA_FRAME_KEY | CA_FRAME_DISCONT) && frame.pts == 9000U;
        }
        if (frame.pts == 9000U) break;
    }
    check(discont_key && ordered,
          "a client that falls behind resumes at a key frame marked as a discontinuity");

    ca_frame_socket_close(server);
    uint8_t byte;
    check(recv(fd, &byte, 1, 0) == 0, "closing the server ends the connection");
    close(fd);
    return failures == 0 ? 0 : 1;
}
