#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/frame_socket.h"

#include "camera_app/log.h"

#include "apcam/atomic.h"

#include <errno.h>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <deque>

#define MAX_CLIENTS 4U
// About half a second of 30 fps video; more is latency, not smoothing.
#define MAX_QUEUED_FRAMES 15U
#define MAX_QUEUED_BYTES (16U * 1024U * 1024U)
#define KEYFRAME_INTERVAL_MS 250U

namespace {

struct queued_frame {
    uint8_t *data;
    size_t length;
    uint64_t pts;
    uint32_t flags;
};

struct client {
    struct ca_frame_socket *server;
    pthread_t thread;
    int fd;
    unsigned stream;
    bool used;
    bool thread_started;
    bool finished;
    bool ready;
    bool waiting_key;
    bool discont;
    size_t queued_bytes;
    std::deque<queued_frame> queue;
};

struct stream_info {
    bool configured;
    enum ca_video_codec codec;
    unsigned width;
    unsigned height;
    unsigned frame_rate;
    uint64_t keyframe_requested_ms;
};

}  // namespace

struct ca_frame_socket {
    int listen_fd;
    pthread_t thread;
    bool thread_started;
    atomic_bool stop;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    ca_frame_socket_keyframe_fn keyframe;
    void *context;
    struct stream_info streams[CA_FRAME_SOCKET_STREAMS];
    atomic_uint watchers[CA_FRAME_SOCKET_STREAMS];
    struct client clients[MAX_CLIENTS];
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void put_u16(uint8_t *out, unsigned value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4U; i++) out[i] = (uint8_t)(value >> (8U * i));
}

static void put_u64(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0; i < 8U; i++) out[i] = (uint8_t)(value >> (8U * i));
}

static void drop_queue(struct client *client)
{
    for (queued_frame &frame : client->queue) free(frame.data);
    client->queue.clear();
    client->queued_bytes = 0;
}

static void request_keyframe(struct ca_frame_socket *server, unsigned stream)
{
    const uint64_t now = monotonic_ms();
    pthread_mutex_lock(&server->lock);
    struct stream_info *info = &server->streams[stream];
    const bool due = info->keyframe_requested_ms == 0U ||
                     now - info->keyframe_requested_ms >= KEYFRAME_INTERVAL_MS;
    if (due) info->keyframe_requested_ms = now;
    pthread_mutex_unlock(&server->lock);
    if (due && server->keyframe != NULL) server->keyframe(server->context, stream);
}

static int send_all(int fd, struct iovec *parts, int count)
{
    while (count > 0) {
        struct msghdr message = {};
        message.msg_iov = parts;
        message.msg_iovlen = (size_t)count;
        ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        while (count > 0 && (size_t)sent >= parts->iov_len) {
            sent -= (ssize_t)parts->iov_len;
            parts++;
            count--;
        }
        if (count > 0) {
            parts->iov_base = (uint8_t *)parts->iov_base + sent;
            parts->iov_len -= (size_t)sent;
        }
    }
    return 0;
}

static int send_info(int fd, uint8_t status, const struct stream_info *info)
{
    uint8_t reply[CA_FRAME_SOCKET_INFO_SIZE] = {'A', 'P', 'V', 'S', CA_FRAME_SOCKET_VERSION, status};
    if (status == CA_FRAME_SOCKET_OK) {
        reply[6] = info->codec == CA_VIDEO_H265 ? 2U : 1U;
        put_u16(reply + 8, info->width);
        put_u16(reply + 10, info->height);
        put_u16(reply + 12, info->frame_rate);
    }
    struct iovec part = {reply, sizeof(reply)};
    return send_all(fd, &part, 1);
}

// Reads 'K' requests and notices a closed peer, without blocking.
static int read_requests(struct client *client)
{
    uint8_t requests[16];
    for (;;) {
        ssize_t bytes = recv(client->fd, requests, sizeof(requests), MSG_DONTWAIT);
        if (bytes == 0) return -1;
        if (bytes < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (memchr(requests, 'K', (size_t)bytes) != NULL) {
            request_keyframe(client->server, client->stream);
        }
    }
}

static int handshake(struct client *client)
{
    struct ca_frame_socket *server = client->server;
    uint8_t hello[CA_FRAME_SOCKET_HELLO_SIZE];
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    (void)setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (recv(client->fd, hello, sizeof(hello), MSG_WAITALL) != (ssize_t)sizeof(hello) ||
        memcmp(hello, "APVR", 4) != 0 || hello[4] != CA_FRAME_SOCKET_VERSION) return -1;
    const unsigned stream = hello[5];
    struct stream_info info = {};
    pthread_mutex_lock(&server->lock);
    if (stream < CA_FRAME_SOCKET_STREAMS) info = server->streams[stream];
    pthread_mutex_unlock(&server->lock);
    if (!info.configured) {
        (void)send_info(client->fd, CA_FRAME_SOCKET_NO_STREAM, &info);
        return -1;
    }
    if (send_info(client->fd, CA_FRAME_SOCKET_OK, &info) < 0) return -1;
    pthread_mutex_lock(&server->lock);
    client->stream = stream;
    client->waiting_key = true;
    client->ready = true;
    server->watchers[stream]++;
    pthread_mutex_unlock(&server->lock);
    ca_log("frame socket: client on stream %u (%ux%u/%u %s)", stream, info.width,
           info.height, info.frame_rate, info.codec == CA_VIDEO_H265 ? "H.265" : "H.264");
    request_keyframe(server, stream);
    return 0;
}

static void *client_thread(void *opaque)
{
    struct client *client = (struct client *)opaque;
    struct ca_frame_socket *server = client->server;
    std::deque<queued_frame> sending;

    if (handshake(client) == 0) {
        while (!atomic_load(&server->stop)) {
            pthread_mutex_lock(&server->lock);
            if (client->queue.empty() && !atomic_load(&server->stop)) {
                struct timespec until;
                (void)clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += 100000000L;
                if (until.tv_nsec >= 1000000000L) {
                    until.tv_sec++;
                    until.tv_nsec -= 1000000000L;
                }
                (void)pthread_cond_timedwait(&server->changed, &server->lock, &until);
            }
            sending.swap(client->queue);
            client->queued_bytes = 0;
            pthread_mutex_unlock(&server->lock);
            if (read_requests(client) < 0) break;
            bool failed = false;
            while (!sending.empty()) {
                queued_frame frame = sending.front();
                sending.pop_front();
                uint8_t header[CA_FRAME_SOCKET_HEADER_SIZE];
                put_u32(header, (uint32_t)frame.length);
                put_u32(header + 4, frame.flags);
                put_u64(header + 8, frame.pts);
                struct iovec parts[2] = {{header, sizeof(header)}, {frame.data, frame.length}};
                failed = failed || send_all(client->fd, parts, 2) < 0;
                free(frame.data);
            }
            if (failed) break;
        }
    }
    pthread_mutex_lock(&server->lock);
    if (client->ready) {
        server->watchers[client->stream]--;
        ca_log("frame socket: client on stream %u left", client->stream);
    }
    client->ready = false;
    drop_queue(client);
    close(client->fd);
    client->fd = -1;
    client->finished = true;
    pthread_mutex_unlock(&server->lock);
    return NULL;
}

// Joins clients whose threads ended; the lock must not be held.
static void reap_clients(struct ca_frame_socket *server, bool all)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        struct client *client = &server->clients[i];
        pthread_mutex_lock(&server->lock);
        const bool join = client->used && client->thread_started && (all || client->finished);
        pthread_mutex_unlock(&server->lock);
        if (!join) continue;
        pthread_join(client->thread, NULL);
        pthread_mutex_lock(&server->lock);
        client->used = false;
        client->thread_started = false;
        pthread_mutex_unlock(&server->lock);
    }
}

static void *accept_thread(void *opaque)
{
    struct ca_frame_socket *server = (struct ca_frame_socket *)opaque;
    while (!atomic_load(&server->stop)) {
        struct pollfd wait = {server->listen_fd, POLLIN, 0};
        if (poll(&wait, 1, 200) <= 0) continue;
        int fd = accept4(server->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) continue;
        reap_clients(server, false);
        struct client *client = NULL;
        pthread_mutex_lock(&server->lock);
        for (unsigned i = 0; i < MAX_CLIENTS && client == NULL; i++) {
            if (!server->clients[i].used) client = &server->clients[i];
        }
        if (client != NULL) {
            client->used = true;
            client->finished = false;
            client->ready = false;
            client->discont = false;
            client->fd = fd;
        }
        pthread_mutex_unlock(&server->lock);
        if (client == NULL) {
            const struct stream_info none = {};
            (void)send_info(fd, CA_FRAME_SOCKET_BUSY, &none);
            close(fd);
            continue;
        }
        if (pthread_create(&client->thread, NULL, client_thread, client) != 0) {
            pthread_mutex_lock(&server->lock);
            close(fd);
            client->fd = -1;
            client->used = false;
            pthread_mutex_unlock(&server->lock);
            continue;
        }
        pthread_mutex_lock(&server->lock);
        client->thread_started = true;
        pthread_mutex_unlock(&server->lock);
    }
    return NULL;
}

int ca_frame_socket_open(struct ca_frame_socket **result, const char *name,
                         ca_frame_socket_keyframe_fn keyframe, void *context)
{
    struct sockaddr_un address = {};
    socklen_t address_length;
    const size_t name_length = name != NULL ? strlen(name) : 0U;
    if (result == NULL || name_length < 2U || name_length >= sizeof(address.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    address.sun_family = AF_UNIX;
    if (name[0] == '@') {
        memcpy(address.sun_path + 1, name + 1, name_length - 1U);
    } else {
        memcpy(address.sun_path, name, name_length);
        (void)unlink(name);
    }
    address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length +
                                 (name[0] == '@' ? 0U : 1U));

    auto *server = new (std::nothrow) ca_frame_socket{};
    if (server == NULL) {
        errno = ENOMEM;
        return -1;
    }
    server->keyframe = keyframe;
    server->context = context;
    pthread_mutex_init(&server->lock, NULL);
    pthread_cond_init(&server->changed, NULL);
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        server->clients[i].server = server;
        server->clients[i].fd = -1;
    }
    server->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server->listen_fd < 0 ||
        bind(server->listen_fd, (const struct sockaddr *)&address, address_length) < 0 ||
        listen(server->listen_fd, (int)MAX_CLIENTS) < 0) {
        goto fail;
    }
    {
        const int code = pthread_create(&server->thread, NULL, accept_thread, server);
        if (code != 0) {
            errno = code;
            goto fail;
        }
    }
    server->thread_started = true;
    ca_log("frame socket ready on %s", name);
    *result = server;
    return 0;
fail:
    {
        const int saved = errno;
        ca_frame_socket_close(server);
        errno = saved;
    }
    return -1;
}

int ca_frame_socket_configure(struct ca_frame_socket *server, unsigned stream,
                              enum ca_video_codec codec, unsigned width,
                              unsigned height, unsigned frame_rate)
{
    if (server == NULL || stream >= CA_FRAME_SOCKET_STREAMS ||
        (codec != CA_VIDEO_H264 && codec != CA_VIDEO_H265) ||
        width == 0U || width > 65535U || height == 0U || height > 65535U ||
        frame_rate == 0U || frame_rate > 65535U) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&server->lock);
    server->streams[stream] = {true, codec, width, height, frame_rate, 0U};
    pthread_mutex_unlock(&server->lock);
    return 0;
}

void ca_frame_socket_publish(struct ca_frame_socket *server, unsigned stream,
                             const uint8_t *annex_b, size_t length, uint64_t pts,
                             bool key_frame)
{
    if (server == NULL || stream >= CA_FRAME_SOCKET_STREAMS || annex_b == NULL ||
        length == 0U || length > UINT32_MAX || atomic_load(&server->watchers[stream]) == 0U) {
        return;
    }
    bool want_key = false;
    pthread_mutex_lock(&server->lock);
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        struct client *client = &server->clients[i];
        if (!client->used || !client->ready || client->stream != stream) continue;
        if (client->queue.size() >= MAX_QUEUED_FRAMES ||
            client->queued_bytes + length > MAX_QUEUED_BYTES) {
            if (!client->waiting_key) {
                ca_log("frame socket: stream %u client fell behind; dropped %zu frames",
                       stream, client->queue.size());
            }
            drop_queue(client);
            client->waiting_key = true;
            client->discont = true;
        }
        if (client->waiting_key && !key_frame) {
            want_key = true;
            continue;
        }
        uint8_t *copy = (uint8_t *)malloc(length);
        if (copy == NULL) {
            drop_queue(client);
            client->waiting_key = true;
            client->discont = true;
            continue;
        }
        memcpy(copy, annex_b, length);
        const uint32_t flags = (key_frame ? CA_FRAME_KEY : 0U) |
                               (client->discont ? CA_FRAME_DISCONT : 0U);
        client->queue.push_back({copy, length, pts, flags});
        client->queued_bytes += length;
        client->waiting_key = false;
        client->discont = false;
    }
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
    if (want_key) request_keyframe(server, stream);
}

void ca_frame_socket_close(struct ca_frame_socket *server)
{
    if (server == NULL) return;
    atomic_store(&server->stop, true);
    if (server->thread_started) pthread_join(server->thread, NULL);
    pthread_mutex_lock(&server->lock);
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i].fd >= 0) (void)shutdown(server->clients[i].fd, SHUT_RDWR);
    }
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
    reap_clients(server, true);
    if (server->listen_fd >= 0) close(server->listen_fd);
    pthread_cond_destroy(&server->changed);
    pthread_mutex_destroy(&server->lock);
    delete server;
}
