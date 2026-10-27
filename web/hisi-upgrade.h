#pragma once
// Hi3516DV500: appfs images are validated here, then written by the rootfs
// recovery updater (/usr/hiface/update, HTTP 127.0.0.1:9091), which reboots.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include "../include/apcam/hisi_firmware.h"

#ifndef HISI_UPDATER_PORT
#define HISI_UPDATER_PORT 9091
#endif
#ifndef HISI_UPDATE_TYPE_PATH
#define HISI_UPDATE_TYPE_PATH "/appfs/fconfigs/param/updatetype.ini"
#endif
#define HISI_FLASH_ARG "--hisi-flash"
#define HISI_FLASH_LOG RUNTIME_DIR "/hisi-flash.log"
#define HISI_FLASH_EXE RUNTIME_DIR "/hisi-flash"
#define HISI_ACCEPT_TIMEOUT_MS 30000
#define HISI_UPDATER_SILENCE_MS 300000
static_assert(apcam_hisi_firmware::MAX_IMAGE_SIZE == 37748980U, "upgrade.js maxSize");

static bool hisi_write_all(int fd, const void *data, size_t length)
{
    const char *cursor = static_cast<const char *>(data);
    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        cursor += written;
        length -= (size_t)written;
    }
    return true;
}

static void hisi_flash_note(int log, const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (log >= 0 && length > 0) (void)hisi_write_all(log, line, std::min((size_t)length, sizeof(line) - 1));
}

// Runs from a tmpfs copy of this static binary, so rewriting appfs cannot
// fault its pages. The updater streams {"percent":N} on the request socket
// while it writes and does not ignore SIGPIPE: hanging up early can kill it
// mid-write, so the socket stays open until the camera reboots.
static int hisi_flash_forward(const char *image_path)
{
    signal(SIGPIPE, SIG_IGN);
    (void)unlink(HISI_FLASH_EXE);
    const int log = open(HISI_FLASH_LOG, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    const int image = open(image_path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (image < 0 || fstat(image, &st) < 0) {
        hisi_flash_note(log, "error: %s: %s\n", image_path, strerror(errno));
        return 1;
    }
    const int updater = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(HISI_UPDATER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (updater < 0 || connect(updater, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
        hisi_flash_note(log, "error: updater on port %d: %s\n", HISI_UPDATER_PORT, strerror(errno));
        (void)unlink(image_path);
        return 1;
    }
    char buffer[64 * 1024];
    int length = snprintf(buffer, sizeof(buffer),
                          "POST /fwUpgrade HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                          "Content-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
                          "Connection: close\r\n\r\n",
                          HISI_UPDATER_PORT, (long long)st.st_size);
    bool sent = hisi_write_all(updater, buffer, (size_t)length);
    off_t remaining = st.st_size;
    while (sent && remaining > 0) {
        ssize_t got = read(image, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        sent = hisi_write_all(updater, buffer, (size_t)got);
        remaining -= got;
    }
    const int saved = errno;
    close(image);
    (void)unlink(image_path);
    if (!sent || remaining != 0) {
        hisi_flash_note(log, "error: sending the image: %s\n", sent ? "short read" : strerror(saved));
        return 1;
    }
    hisi_flash_note(log, "sent %lld bytes\n", (long long)st.st_size);
    for (;;) {
        struct pollfd ready = {updater, POLLIN, 0};
        int polled = poll(&ready, 1, HISI_UPDATER_SILENCE_MS);
        if (polled < 0 && errno == EINTR) continue;
        if (polled == 0) {
            hisi_flash_note(log, "\nerror: updater silent for %d s\n", HISI_UPDATER_SILENCE_MS / 1000);
            return 1;
        }
        ssize_t got = polled > 0 ? recv(updater, buffer, sizeof(buffer), 0) : -1;
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        (void)hisi_write_all(log, buffer, (size_t)got);
    }
    hisi_flash_note(log, "\nupdater closed the connection\n");
    return 0;
}

static bool hisi_copy_file(const char *from, const char *to, char *error, size_t error_size)
{
    char temporary[PATH_MAX];
    snprintf(temporary, sizeof(temporary), "%s.tmp", to);
    const int input = open(from, O_RDONLY | O_CLOEXEC);
    const int output = input < 0 ? -1 : open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    bool ok = output >= 0;
    char buffer[64 * 1024];
    while (ok) {
        ssize_t got = read(input, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) ok = false;
        if (got <= 0) break;
        ok = hisi_write_all(output, buffer, (size_t)got);
    }
    if (!ok) snprintf(error, error_size, "copying %s to %s: %s", from, temporary, strerror(errno));
    if (input >= 0) close(input);
    if (output >= 0 && close(output) < 0 && ok) {
        snprintf(error, error_size, "%s: %s", temporary, strerror(errno));
        ok = false;
    }
    if (ok && rename(temporary, to) < 0) {
        snprintf(error, error_size, "%s: %s", to, strerror(errno));
        ok = false;
    }
    if (!ok) (void)unlink(temporary);
    return ok;
}

// The forwarder inherits the upgrade lock as fd 3, so later uploads see the
// flash in progress until the camera reboots.
static bool hisi_start_forward(const char *image, int upgrade_lock, char *error, size_t error_size)
{
    if (!hisi_copy_file("/proc/self/exe", HISI_FLASH_EXE, error, error_size)) return false;
    const int log = open(HISI_FLASH_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log < 0) {
        snprintf(error, error_size, "%s: %s", HISI_FLASH_LOG, strerror(errno));
        return false;
    }
    close(log);
    pid_t child = fork();
    if (child == 0) {
        (void)setsid();
        if (upgrade_lock == 3) (void)fcntl(3, F_SETFD, 0);
        else if (dup2(upgrade_lock, 3) < 0) _exit(126);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        for (int fd = 4; fd < 1024; fd++) close(fd);
        execl(HISI_FLASH_EXE, HISI_FLASH_EXE, HISI_FLASH_ARG, image, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        snprintf(error, error_size, "fork: %s", strerror(errno));
        (void)unlink(HISI_FLASH_EXE);
        return false;
    }
    return true;
}

// 1 when the updater acknowledged the image, 0 when the forwarder failed and
// -1 on timeout; the log text is copied to reply either way.
static int hisi_wait_for_updater(char *reply, size_t reply_size)
{
    for (int waited = 0; waited < HISI_ACCEPT_TIMEOUT_MS; waited += 100) {
        reply[0] = '\0';
        const int log = open(HISI_FLASH_LOG, O_RDONLY | O_CLOEXEC);
        if (log >= 0) {
            ssize_t got = read(log, reply, reply_size - 1);
            reply[got > 0 ? got : 0] = '\0';
            close(log);
        }
        if (strstr(reply, "\"percent\"")) return 1;
        if (strstr(reply, "error:") || strstr(reply, "updater closed")) return 0;
        usleep(100000);
    }
    return -1;
}

static void handle_hisi_firmware_install(int fd, const APC_HTTPRequest *request, const char *peer)
{
    char filename[256];
    char content_type[128];
    char package[PATH_MAX] = "";
    char error[512];
    char reply[512];
    struct statvfs space;
    int upgrade_lock = -1;
    int output = -1;
    size_t received = 0;
    const char *problem = nullptr;
    apcam_hisi_firmware::device_code expected = apcam_hisi_firmware::CAMERA_DEVICE_CODE;
    auto discard = [&]() {
        if (output >= 0) close(output);
        output = -1;
        if (package[0]) (void)unlink(package);
        package[0] = '\0';
    };

    if (!valid_csrf_header(request)) {
        log_message("CSRF check failed from %s for firmware upload", peer);
        send_text_errorf(fd, 403, "Forbidden", S_CSRF_RELOAD);
        return;
    }
    if (request->content_length == 0 || request->content_length > apcam_hisi_firmware::MAX_IMAGE_SIZE) {
        send_text_errorf(fd, 413, "Payload Too Large", S_E_FW_SIZE_HISI);
        return;
    }
    if (find_header(request, "Content-Type", content_type, sizeof(content_type)) == NULL ||
        strncasecmp(content_type, "application/octet-stream", 24) != 0 ||
        (content_type[24] != '\0' && content_type[24] != ';')) {
        send_text_errorf(fd, 415, "Unsupported Media Type", S_E_FW_CONTENT_TYPE);
        return;
    }
    if (find_header(request, "X-Firmware-Name", filename, sizeof(filename)) == NULL ||
        !valid_firmware_name(filename)) {
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_NAME, FIRMWARE_NAME_PATTERNS);
        return;
    }
    upgrade_lock = open(UPGRADE_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (upgrade_lock < 0) {
        send_text_errorf(fd, 503, "Service Unavailable", S_E_FW_LOCK, strerror(errno));
        return;
    }
    if (flock(upgrade_lock, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) send_text_errorf(fd, 409, "Conflict", S_E_FW_FLASHING);
        else send_text_errorf(fd, 503, "Service Unavailable", S_E_FW_LOCK, strerror(errno));
        close(upgrade_lock);
        return;
    }
    snprintf(package, sizeof(package), "%s/firmware-upload.XXXXXX", RUNTIME_DIR);
    output = mkostemp(package, O_CLOEXEC);
    if (output < 0) {
        package[0] = '\0';
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_TMP, RUNTIME_DIR, strerror(errno));
        goto done;
    }
    if (fstatvfs(output, &space) == 0 &&
        (uint64_t)space.f_bavail * space.f_frsize < request->content_length + 4U * 1024U * 1024U) {
        discard();
        send_text_errorf(fd, 507, "Insufficient Storage", S_E_FW_TMP_SPACE, RUNTIME_DIR);
        goto done;
    }
    if (!receive_upload_body(fd, request, output, &received)) {
        log_message("firmware upload from %s failed after %zu bytes: %s", peer, received, strerror(errno));
        discard();
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_FAILED, received, strerror(errno));
        goto done;
    }
    (void)apcam_hisi_firmware::read_device_code(HISI_UPDATE_TYPE_PATH, &expected);
    {
        void *image = mmap(NULL, received, PROT_READ, MAP_PRIVATE, output, 0);
        if (image == MAP_FAILED) {
            snprintf(error, sizeof(error), "mmap: %s", strerror(errno));
            problem = error;
        } else {
            problem = apcam_hisi_firmware::check_appfs_image(static_cast<const uint8_t *>(image), received,
                                                             expected, error, sizeof(error));
            munmap(image, received);
        }
    }
    if (problem != nullptr) {
        log_message("firmware %s from %s rejected: %s", filename, peer, problem);
        discard();
        send_text_errorf(fd, 400, "Bad Request", S_E_FW_PACKAGE, problem);
        goto done;
    }
    close(output);
    output = -1;
    if (!hisi_start_forward(package, upgrade_lock, error, sizeof(error))) {
        log_message("firmware %s from %s not forwarded: %s", filename, peer, error);
        discard();
        send_text_errorf(fd, 500, "Internal Server Error", S_E_FW_INSTALL, error);
        goto done;
    }
    package[0] = '\0';
    switch (hisi_wait_for_updater(reply, sizeof(reply))) {
    case 1:
        log_message("firmware %s (%zu bytes) from %s accepted by the updater; it reboots when written",
                    filename, received, peer);
        send_text_errorf(fd, 201, "Created", S_FW_FLASHING_HISI, filename);
        break;
    case 0:
        log_message("firmware %s from %s: updater failed: %s", filename, peer, reply);
        send_text_errorf(fd, 502, "Bad Gateway", S_E_FW_UPDATER, reply);
        break;
    default:
        log_message("firmware %s from %s: no answer from the updater yet", filename, peer);
        send_text_errorf(fd, 504, "Gateway Timeout", S_E_FW_UPDATER_TIMEOUT);
        break;
    }
done:
    discard();
    close(upgrade_lock);
}
