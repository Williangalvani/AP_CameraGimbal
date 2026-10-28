#ifndef CAMERA_APP_STORAGE_H
#define CAMERA_APP_STORAGE_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* A removable card that packaging mounts and formats with a helper
 * (packaging/hisi/sdcard.sh). CAMERA_APP_STORAGE_STATE names the file holding
 * its state as the first word, CAMERA_APP_STORAGE_TOOL the helper, run as
 * "TOOL format". Without CAMERA_APP_STORAGE_STATE the output directories are
 * used as they are. */
enum ca_storage_state {
    CA_STORAGE_UNMANAGED,
    CA_STORAGE_ABSENT,
    CA_STORAGE_UNFORMATTED,
    CA_STORAGE_FORMATTING,
    CA_STORAGE_READY,
};

#ifndef ENOMEDIUM
#define ENOMEDIUM ENODEV
#endif
#ifndef EMEDIUMTYPE
#define EMEDIUMTYPE ENODEV
#endif

static inline enum ca_storage_state ca_storage_state(void)
{
    const char *path = getenv("CAMERA_APP_STORAGE_STATE");
    if (path == NULL || path[0] == '\0') return CA_STORAGE_UNMANAGED;
    char text[64];
    ssize_t length = -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        length = read(fd, text, sizeof(text) - 1U);
        (void)close(fd);
    }
    /* Not written yet: the helper has not looked at the card. */
    if (length <= 0) return CA_STORAGE_ABSENT;
    text[length] = '\0';
    text[strcspn(text, " \n")] = '\0';
    if (strcmp(text, "ready") == 0) return CA_STORAGE_READY;
    if (strcmp(text, "formatting") == 0) return CA_STORAGE_FORMATTING;
    if (strcmp(text, "unformatted") == 0) return CA_STORAGE_UNFORMATTED;
    return CA_STORAGE_ABSENT;
}

/* 0 if recordings and stills can be written now; otherwise -1 with errno
 * ENOMEDIUM (no card), EMEDIUMTYPE (unformatted) or EBUSY (formatting). */
static inline int ca_storage_writable(void)
{
    switch (ca_storage_state()) {
    case CA_STORAGE_UNMANAGED:
    case CA_STORAGE_READY:
        return 0;
    case CA_STORAGE_ABSENT:
        errno = ENOMEDIUM;
        return -1;
    case CA_STORAGE_UNFORMATTED:
        errno = EMEDIUMTYPE;
        return -1;
    case CA_STORAGE_FORMATTING:
        errno = EBUSY;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

/* Starts "TOOL format" in the background: the child's PID, or -1 with errno
 * set (ENOTSUP when no helper is configured). */
static inline pid_t ca_storage_format_start(void)
{
    const char *tool = getenv("CAMERA_APP_STORAGE_TOOL");
    if (tool == NULL || tool[0] == '\0') {
        errno = ENOTSUP;
        return -1;
    }
    struct rlimit files;
    int max_fd = getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur != RLIM_INFINITY &&
                 files.rlim_cur < 65536U ? (int)files.rlim_cur : 65536;
    pid_t child = fork();
    if (child != 0) return child;
    /* Only async-signal-safe calls until exec. Sockets and device handles stay
     * with camera-app: the helper may outlive it across a restart. */
    sigset_t none;
    sigemptyset(&none);
    (void)sigprocmask(SIG_SETMASK, &none, NULL);
    (void)signal(SIGPIPE, SIG_DFL);
    for (int fd = 3; fd < max_fd; fd++) (void)close(fd);
    execl(tool, tool, "format", (char *)NULL);
    _exit(127);
}

/* 0 while the format runs, 1 once it succeeded, -1 once it failed. */
static inline int ca_storage_format_poll(pid_t child)
{
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == 0) return 0;
    if (waited < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : -1;
}

#endif
