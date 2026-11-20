#include "camera_app/storage.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static char state_path[64], lock_path[64];

static void write_state(const char *text)
{
    FILE *file = fopen(state_path, "w");
    assert(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}

static int writable_errno(void)
{
    errno = 0;
    return ca_storage_writable() == 0 ? 0 : errno;
}

int main(void)
{
    char directory[] = "/tmp/camera-app-storage-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    snprintf(state_path, sizeof(state_path), "%s/sdcard.state", directory);
    snprintf(lock_path, sizeof(lock_path), "%s/sdbench.lock", directory);

    // Unmanaged output directories are always writable, benchmark or not.
    unsetenv("CAMERA_APP_STORAGE_STATE");
    setenv("CAMERA_APP_STORAGE_BENCH_LOCK", lock_path, 1);
    assert(writable_errno() == 0);

    setenv("CAMERA_APP_STORAGE_STATE", state_path, 1);
    write_state("ready\n");
    // No benchmark has run since boot: no lock file.
    assert(writable_errno() == 0 && !ca_storage_benchmarking());

    // The lock file sdbench.sh or hisi-web leaves, and their shared probes.
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    assert(lock >= 0);
    assert(writable_errno() == 0);
    assert(flock(lock, LOCK_SH) == 0);
    assert(writable_errno() == 0 && !ca_storage_benchmarking());

    // A running benchmark: recordings, stills and BIN logs wait.
    assert(flock(lock, LOCK_EX) == 0);
    assert(writable_errno() == EBUSY && ca_storage_benchmarking());
    // The probe neither takes nor breaks the benchmark's lock.
    int other = open(lock_path, O_RDONLY | O_CLOEXEC);
    assert(other >= 0 && flock(other, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK);
    close(other);
    // Without the variable the lock is not looked at.
    unsetenv("CAMERA_APP_STORAGE_BENCH_LOCK");
    assert(writable_errno() == 0);
    setenv("CAMERA_APP_STORAGE_BENCH_LOCK", "", 1);
    assert(writable_errno() == 0);
    setenv("CAMERA_APP_STORAGE_BENCH_LOCK", lock_path, 1);
    // The card's own state comes first.
    write_state("absent\n");
    assert(writable_errno() == ENOMEDIUM);
    write_state("unformatted /dev/mmcblk0\n");
    assert(writable_errno() == EMEDIUMTYPE);
    write_state("formatting\n");
    assert(writable_errno() == EBUSY);
    write_state("ready\n");
    assert(writable_errno() == EBUSY);

    // Finished, or killed: the lock goes with its holder.
    assert(flock(lock, LOCK_UN) == 0);
    assert(writable_errno() == 0 && !ca_storage_benchmarking());
    assert(flock(lock, LOCK_EX) == 0);
    close(lock);
    assert(writable_errno() == 0);

    unlink(lock_path);
    unlink(state_path);
    rmdir(directory);
    puts("PASS storage state and benchmark lock");
    return 0;
}
