/*
 * Minimal sampling profiler for one thread, for kernels without perf events.
 * Interrupts TID with ptrace, records pc, lr and a frame-pointer walk, and
 * resumes it. Prints one sample per line as hex addresses, then the maps.
 * Usage: tsample PID TID HZ SECONDS
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAMES 24

struct regs {
    uint64_t x[31];
    uint64_t sp, pc, pstate;
};

static int read_words(pid_t pid, uint64_t addr, uint64_t *out, size_t n)
{
    struct iovec local = {out, n * sizeof(*out)};
    struct iovec remote = {(void *)(uintptr_t)addr, n * sizeof(*out)};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)(n * sizeof(*out));
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s PID TID HZ SECONDS\n", argv[0]);
        return 2;
    }
    pid_t pid = atoi(argv[1]), tid = atoi(argv[2]);
    long hz = atol(argv[3]), seconds = atol(argv[4]);
    long samples = hz * seconds, taken = 0, missed = 0;
    struct timespec gap = {0, 1000000000L / hz};

    if (ptrace(PTRACE_SEIZE, tid, 0, 0) != 0) {
        perror("PTRACE_SEIZE");
        return 1;
    }
    for (long i = 0; i < samples; i++) {
        int status;
        struct regs r;
        struct iovec io = {&r, sizeof(r)};
        nanosleep(&gap, NULL);
        if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) != 0 || waitpid(tid, &status, __WALL) != tid) {
            missed++;
            continue;
        }
        if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &io) == 0) {
            printf("S %llx %llx", (unsigned long long)r.pc, (unsigned long long)r.x[30]);
            uint64_t fp = r.x[29];
            for (int f = 0; f < MAX_FRAMES && fp && fp >= r.sp && fp - r.sp < (8u << 20); f++) {
                uint64_t frame[2];
                if (!read_words(pid, fp, frame, 2) || !frame[1]) break;
                printf(" %llx", (unsigned long long)frame[1]);
                if (frame[0] <= fp) break;
                fp = frame[0];
            }
            putchar('\n');
            taken++;
        }
        ptrace(PTRACE_CONT, tid, 0, 0);
    }
    ptrace(PTRACE_DETACH, tid, 0, 0);
    fprintf(stderr, "samples %ld missed %ld\n", taken, missed);

    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *maps = fopen(path, "r");
    while (maps && fgets(line, sizeof(line), maps))
        if (strstr(line, " r-xp ") || strstr(line, " r--p ")) printf("M %s", line);
    if (maps) fclose(maps);
    return 0;
}
