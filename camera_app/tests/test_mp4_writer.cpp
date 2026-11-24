#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "camera_app/mp4.h"
#include "camera_app/mp4_queue.h"
#include "camera_app/video_metadata.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

/* The recorder's buffer and writer thread: queue order, accounting and drops;
 * callers that never wait for a stalled card; large writes; splits decided
 * on queued data; complete, decodable files; and errors reported to the
 * caller. Storage calls are wrapped to stall, fail and check their thread. */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t frame_thread;
static bool hold_writes;
static unsigned stall_ms;
static int fail_writes;
/* Writes within the file succeed; one that extends it takes half its data,
 * then nothing more fits. */
static bool card_full, card_filled;
static int fail_syncs;
static unsigned pwrite_calls, writes_entered, large_writes;
static uint64_t pwrite_bytes;
static bool fat;

static void no_storage_on_frame_thread(void)
{
    assert(!pthread_equal(pthread_self(), frame_thread));
}

extern "C" ssize_t __real_pwrite64(int fd, const void *data, size_t size, off64_t offset);
extern "C" ssize_t __wrap_pwrite64(int fd, const void *data, size_t size, off64_t offset)
{
    no_storage_on_frame_thread();
    pthread_mutex_lock(&lock);
    writes_entered++;
    pthread_cond_broadcast(&changed);
    while (hold_writes) pthread_cond_wait(&changed, &lock);
    int fail = fail_writes;
    unsigned stall = stall_ms;
    if (card_full) {
        struct stat st;
        assert(fstat(fd, &st) == 0);
        if ((uint64_t)offset + size > (uint64_t)st.st_size) {
            if (card_filled || size < 2) fail = ENOSPC;
            else size /= 2;
            card_filled = true;
        }
    }
    if (!fail) {
        pwrite_calls++;
        pwrite_bytes += size;
        if (size >= CA_MP4_WRITE_BYTES) large_writes++;
    }
    pthread_mutex_unlock(&lock);
    if (fail) { errno = fail; return -1; }
    if (stall) usleep(stall * 1000U);
    return __real_pwrite64(fd, data, size, offset);
}

extern "C" int __real_fdatasync(int fd);
extern "C" int __wrap_fdatasync(int fd)
{
    no_storage_on_frame_thread();
    pthread_mutex_lock(&lock);
    int fail = fail_syncs;
    pthread_mutex_unlock(&lock);
    if (fail) { errno = fail; return -1; }
    return __real_fdatasync(fd);
}

extern "C" int __real_ftruncate64(int fd, off64_t length);
extern "C" int __wrap_ftruncate64(int fd, off64_t length)
{
    no_storage_on_frame_thread();
    return __real_ftruncate64(fd, length);
}

extern "C" int __real_fallocate64(int fd, int mode, off64_t offset, off64_t length);
extern "C" int __wrap_fallocate64(int fd, int mode, off64_t offset, off64_t length)
{
    no_storage_on_frame_thread();
    pthread_mutex_lock(&lock);
    bool full = card_full;
    pthread_mutex_unlock(&lock);
    if (full) { errno = ENOSPC; return -1; }
    return __real_fallocate64(fd, mode, offset, length);
}

extern "C" int __real_fstatfs64(int fd, struct statfs64 *fs);
extern "C" int __wrap_fstatfs64(int fd, struct statfs64 *fs)
{
    int result = __real_fstatfs64(fd, fs);
    if (result == 0 && fat) fs->f_type = 0x4d44;
    return result;
}

struct access_unit { const uint8_t *data; size_t length; bool idr; };
static std::vector<access_unit> units;
static char directory[] = "/tmp/mp4-writer-test-XXXXXX";

static void set_hold(bool hold)
{
    pthread_mutex_lock(&lock);
    hold_writes = hold;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void reset_counts(void)
{
    pthread_mutex_lock(&lock);
    pwrite_calls = writes_entered = large_writes = 0;
    pwrite_bytes = 0;
    pthread_mutex_unlock(&lock);
}

static void wait_for_write_entry(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&lock);
    while (writes_entered == 0)
        assert(pthread_cond_timedwait(&changed, &lock, &deadline) == 0);
    pthread_mutex_unlock(&lock);
}

static uint64_t now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Frame n of the fixture, repeated; it starts with an IDR every 10 frames. */
static uint64_t write_frame(struct ca_mp4 *writer, unsigned n, int *result)
{
    const access_unit &unit = units[n % units.size()];
    ca_metadata_set_position(-353632610 + (int32_t)n, 1491652300, 620, 30, 0);
    uint64_t started = now_us();
    *result = ca_mp4_write_h264(writer, unit.data, unit.length, n * 9000U, unit.idr, 54.7f);
    return now_us() - started;
}

/* Fails rather than hangs if the writer stops. */
static void drain(struct ca_mp4 *writer)
{
    struct ca_mp4_stats stats;
    uint64_t deadline = now_us() + 10000000U;
    for (;;) {
        ca_mp4_get_stats(writer, &stats);
        if (stats.buffer_bytes == 0) return;
        assert(now_us() < deadline);
        usleep(1000);
    }
}

static std::vector<uint8_t> read_file(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    std::vector<uint8_t> data((size_t)st.st_size);
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, data.data(), data.size()) == (ssize_t)data.size());
    close(fd);
    return data;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

struct layout {
    bool tiled;  /* top-level boxes cover the file exactly */
    bool moov;
    unsigned sidx, moofs;
    uint32_t timescale, duration; /* from the mvhd */
};

static layout parse_file(const char *path)
{
    std::vector<uint8_t> data = read_file(path);
    layout file = {};
    size_t at = 0;
    while (at < data.size()) {
        if (data.size() - at < 8) return file;
        uint32_t size = get32(&data[at]);
        if (size < 8 || size > data.size() - at) return file;
        if (at == 0 && memcmp(&data[4], "ftyp", 4)) return file;
        if (!memcmp(&data[at + 4], "moov", 4)) {
            file.moov = true;
            for (size_t child = at + 8; child + 32 <= at + size;) {
                uint32_t length = get32(&data[child]);
                if (length < 8 || length > at + size - child) break;
                if (!memcmp(&data[child + 4], "mvhd", 4) && data[child + 8] == 0) {
                    file.timescale = get32(&data[child + 20]);
                    file.duration = get32(&data[child + 24]);
                }
                child += length;
            }
        }
        if (!memcmp(&data[at + 4], "sidx", 4)) file.sidx++;
        if (!memcmp(&data[at + 4], "moof", 4)) file.moofs++;
        at += size;
    }
    file.tiled = true;
    return file;
}

/* Top-level boxes must tile the file exactly; one moof per frame. */
static unsigned count_fragments(const char *path)
{
    layout file = parse_file(path);
    assert(file.tiled && file.moov && file.sidx == 1);
    return file.moofs;
}

/* The fixture frame number in each sample's telemetry, and whether the
 * sample is an IDR. */
struct sample { long frame; bool idr; };
static const long LAT_BASE = -353632610;

static std::vector<sample> samples(const char *path)
{
    std::vector<uint8_t> data = read_file(path);
    std::vector<sample> result;
    static const char key[] = "\"lat_e7\":";
    for (size_t at = 0; at + 8 <= data.size(); at += get32(&data[at])) {
        uint32_t size = get32(&data[at]);
        assert(size >= 8 && size <= data.size() - at);
        if (memcmp(&data[at + 4], "mdat", 4)) continue;
        sample s = {LONG_MIN, false};
        for (size_t nal = at + 8; nal + 4 < at + size;) {
            uint32_t length = get32(&data[nal]);
            assert(length > 0 && length <= at + size - nal - 4);
            const uint8_t *p = &data[nal + 4];
            if ((p[0] & 31) == 5) s.idr = true;
            if ((p[0] & 31) == 6) {
                std::string text((const char*)p, length);
                size_t found = text.find(key);
                if (found != std::string::npos)
                    s.frame = strtol(text.c_str() + found + sizeof(key) - 1, NULL, 10) - LAT_BASE;
            }
            nal += 4 + length;
        }
        assert(s.frame != LONG_MIN);
        result.push_back(s);
    }
    return result;
}

static std::string run(const std::string &command)
{
    std::string output;
    FILE *pipe = popen(command.c_str(), "r");
    assert(pipe != NULL);
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    assert(pclose(pipe) == 0);
    return output;
}

/* Decoded frames, with no decoder complaint. */
static unsigned decoded_frames(const char *path)
{
    std::string quoted = std::string("'") + path + "'";
    assert(run("ffmpeg -v error -i " + quoted + " -f null - 2>&1").empty());
    std::string count = run("ffprobe -v error -count_frames -select_streams v:0 "
                            "-show_entries stream=nb_read_frames -of csv=p=0 " + quoted + " 2>&1");
    char *end;
    unsigned long frames = strtoul(count.c_str(), &end, 10);
    assert(end != count.c_str() && *end == '\n');
    return (unsigned)frames;
}

static bool first_frame_key(const char *path)
{
    std::string key = run(std::string("ffprobe -v error -select_streams v:0 -read_intervals %+#1 "
                          "-show_entries frame=key_frame -of csv=p=0 '") + path + "' 2>&1");
    return key == "1\n" || key == "1,\n";
}

static void check_file(const char *path, unsigned frames)
{
    assert(count_fragments(path) == frames);
    assert(decoded_frames(path) == frames);
}

static void test_queue(void)
{
    struct ca_mp4_queue queue;
    struct ca_mp4_frame *frame;
    struct ca_mp4_stats stats;
    struct ca_metadata metadata = {};
    struct timespec utc = {1700000000, 5};
    uint8_t data[4][1000];
    const size_t cost = ca_mp4_queue_cost(sizeof(data[0]));
    for (unsigned i = 0; i < 4; i++) memset(data[i], (int)i + 1, sizeof(data[i]));
    assert(ca_mp4_queue_init(&queue, 3 * cost) == 0);

    const enum ca_mp4_unit IDR = CA_MP4_UNIT_IDR, P = CA_MP4_UNIT_PICTURE,
                           HEADERS = CA_MP4_UNIT_HEADERS;
    metadata.lat_e7 = 11;
    assert(ca_mp4_queue_push(&queue, data[0], sizeof(data[0]), IDR, &metadata, &utc) == 1);
    metadata.lat_e7 = 12;
    assert(ca_mp4_queue_push(&queue, data[1], sizeof(data[1]), P, &metadata, &utc) == 1);
    ca_mp4_queue_stats(&queue, &stats, NULL);
    assert(stats.buffer_bytes == 2 * cost && stats.buffer_high_water == 2 * cost);
    assert(stats.buffer_capacity == 3 * cost);
    assert(ca_mp4_queue_push(&queue, data[2], sizeof(data[2]), P, &metadata, &utc) == 1);

    /* Full: the frame that does not fit, and every frame until an IDR that
     * does, is dropped; one event. */
    assert(ca_mp4_queue_push(&queue, data[3], 1, P, &metadata, &utc) == 0);
    assert(ca_mp4_queue_pop(&queue, 0, &frame) == 1);
    assert(frame->idr && frame->length == sizeof(data[0]) && frame->data[999] == 1);
    assert(frame->metadata.lat_e7 == 11 && frame->utc.tv_sec == utc.tv_sec &&
           frame->utc.tv_nsec == utc.tv_nsec);
    ca_mp4_queue_release(&queue, frame, true);
    assert(ca_mp4_queue_push(&queue, data[3], sizeof(data[3]), P, &metadata, &utc) == 0);
    uint8_t large[2000] = {};
    assert(ca_mp4_queue_push(&queue, large, sizeof(large), IDR, &metadata, &utc) == 0);
    ca_mp4_queue_stats(&queue, &stats, NULL);
    assert(stats.frames_dropped == 3 && stats.drop_events == 1 && stats.frames_written == 1);
    assert(stats.buffer_bytes == 2 * cost);
    assert(ca_mp4_queue_push(&queue, data[3], sizeof(data[3]), IDR, &metadata, &utc) == 1);
    assert(ca_mp4_queue_push(&queue, data[0], 1, P, &metadata, &utc) == 0);
    ca_mp4_queue_stats(&queue, &stats, NULL);
    assert(stats.frames_dropped == 4 && stats.drop_events == 2);
    assert(stats.buffer_bytes == 3 * cost && stats.buffer_high_water == 3 * cost);

    /* Order is preserved; released bytes return to the capacity. */
    const uint8_t expected[] = {2, 3, 4};
    for (uint8_t value : expected) {
        assert(ca_mp4_queue_pop(&queue, 0, &frame) == 1 && frame->data[0] == value);
        assert(frame->idr == (value == 4));
        ca_mp4_queue_release(&queue, frame, false);
    }
    size_t file_high_water;
    ca_mp4_queue_stats(&queue, &stats, &file_high_water);
    assert(stats.buffer_bytes == 0 && file_high_water == 3 * cost && stats.frames_written == 1);
    ca_mp4_queue_stats(&queue, &stats, &file_high_water);
    assert(file_high_water == 0 && stats.buffer_high_water == 3 * cost);

    /* A frame larger than the whole buffer never fits. */
    uint8_t huge[4000] = {};
    assert(ca_mp4_queue_push(&queue, huge, sizeof(huge), IDR, &metadata, &utc) == 0);
    assert(ca_mp4_queue_pop(&queue, 10, &frame) == 0 && frame == NULL);
    assert(ca_mp4_queue_push(&queue, data[0], sizeof(data[0]), IDR, &metadata, &utc) == 1);
    ca_mp4_queue_close(&queue);
    assert(ca_mp4_queue_push(&queue, data[0], 1, IDR, &metadata, &utc) < 0 && errno == EPIPE);
    assert(ca_mp4_queue_pop(&queue, 0, &frame) == 1);
    ca_mp4_queue_release(&queue, frame, true);
    assert(ca_mp4_queue_pop(&queue, 1000, &frame) == -1);
    ca_mp4_queue_destroy(&queue);

    /* While dropping, an IDR that fits resumes only with a quarter of the
     * buffer free. Units without a picture are kept whenever they fit and
     * neither end nor start a run. */
    const size_t small = ca_mp4_queue_cost(100);
    assert(ca_mp4_queue_init(&queue, 8 * small) == 0);
    for (unsigned i = 0; i < 8; i++)
        assert(ca_mp4_queue_push(&queue, data[0], 100, i ? P : IDR, &metadata, &utc) == 1);
    assert(ca_mp4_queue_push(&queue, data[0], 100, P, &metadata, &utc) == 0);
    assert(ca_mp4_queue_pop(&queue, 0, &frame) == 1);
    ca_mp4_queue_release(&queue, frame, true);
    assert(ca_mp4_queue_push(&queue, data[0], 100, IDR, &metadata, &utc) == 0);
    assert(ca_mp4_queue_push(&queue, data[1], 100, HEADERS, &metadata, &utc) == 1);
    for (unsigned i = 0; i < 2; i++) {
        assert(ca_mp4_queue_pop(&queue, 0, &frame) == 1);
        ca_mp4_queue_release(&queue, frame, true);
    }
    assert(ca_mp4_queue_push(&queue, data[0], 100, P, &metadata, &utc) == 0);
    assert(ca_mp4_queue_push(&queue, data[0], 100, IDR, &metadata, &utc) == 1);
    ca_mp4_queue_stats(&queue, &stats, NULL);
    assert(stats.frames_dropped == 3 && stats.drop_events == 1);
    assert(ca_mp4_queue_push(&queue, data[1], 200, HEADERS, &metadata, &utc) == 0);
    assert(ca_mp4_queue_push(&queue, data[0], 100, P, &metadata, &utc) == 1);
    ca_mp4_queue_stats(&queue, &stats, NULL);
    assert(stats.frames_dropped == 4 && stats.drop_events == 1 && stats.buffer_bytes == 8 * small);
    /* a frame the producer lost starts a run like one that did not fit */
    ca_mp4_queue_drop(&queue);
    for (unsigned i = 0; i < 4; i++) {
        assert(ca_mp4_queue_pop(&queue, 0, &frame) == 1);
        ca_mp4_queue_release(&queue, frame, true);
    }
    assert(ca_mp4_queue_push(&queue, data[0], 100, P, &metadata, &utc) == 0);
    assert(ca_mp4_queue_push(&queue, data[0], 100, IDR, &metadata, &utc) == 1);
    ca_mp4_queue_stats(&queue, &stats, NULL);
    assert(stats.frames_dropped == 6 && stats.drop_events == 2);
    ca_mp4_queue_destroy(&queue);
    puts("PASS recording buffer: order, accounting, drops until an IDR that fits with a quarter "
         "free, headers kept, high-water");
}

/* A stalled card never delays the caller; nothing is lost while it fits. */
static void test_stalled_card(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/stall.mp4", directory);
    struct ca_mp4 *writer = NULL;
    reset_counts();
    set_hold(true);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    int result;
    write_frame(writer, 0, &result);
    assert(result == 0);
    wait_for_write_entry();
    uint64_t slowest = 0;
    for (unsigned n = 1; n < 300; n++) {
        uint64_t elapsed = write_frame(writer, n, &result);
        assert(result == 0);
        if (elapsed > slowest) slowest = elapsed;
    }
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    pthread_mutex_lock(&lock);
    assert(pwrite_calls == 0);
    pthread_mutex_unlock(&lock);
    /* the first IDR's own write is the one held */
    assert(stats.frames_written == 0 && stats.frames_dropped == 0 && stats.buffer_bytes > 0);
    assert(slowest < 50000);
    stall_ms = 20;
    set_hold(false);
    assert(ca_mp4_close(writer) == 0);
    stall_ms = 0;
    check_file(path, 300);
    unlink(path);
    printf("PASS stalled card: slowest write call %llu us, 300 frames kept\n",
           (unsigned long long)slowest);
}

/* Frames reach the card in large writes, not one or more writes each. */
static void test_large_writes(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/large.mp4", directory);
    struct ca_mp4 *writer = NULL;
    reset_counts();
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    uint64_t queued = 0;
    unsigned n = 0;
    int result;
    while (queued < 5U * CA_MP4_WRITE_BYTES) {
        write_frame(writer, n, &result);
        assert(result == 0);
        queued += units[n % units.size()].length;
        n++;
    }
    drain(writer);
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    assert(stats.frames_written == n && stats.frames_dropped == 0);
    assert(ca_mp4_close(writer) == 0);
    check_file(path, n);
    pthread_mutex_lock(&lock);
    /* the large writes, the remainder at close and the index patches */
    assert(large_writes >= 4 && pwrite_calls <= large_writes + 8);
    printf("PASS large writes: %u frames in %u writes, %u of %u+ MiB\n",
           n, pwrite_calls, large_writes, CA_MP4_WRITE_BYTES >> 20);
    pthread_mutex_unlock(&lock);
    unlink(path);
}

/* A low FAT threshold splits on IDRs, counting data not yet on the card, and
 * every part is complete. */
static void test_split(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/split.mp4", directory);
    fat = true;
    assert(setenv("CAMERA_APP_RECORD_SEGMENT_BYTES", "65536", 1) == 0);
    struct ca_mp4 *writer = NULL;
    reset_counts();
    set_hold(true);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    const unsigned frames = 300;
    int result;
    for (unsigned n = 0; n < frames; n++) {
        write_frame(writer, n, &result);
        assert(result == 0);
    }
    set_hold(false);
    assert(ca_mp4_close(writer) == 0);
    fat = false;
    unsetenv("CAMERA_APP_RECORD_SEGMENT_BYTES");

    size_t gop = 0;
    for (size_t i = 0; i < units.size(); i += 10) {
        size_t bytes = 0;
        for (size_t j = i; j < i + 10 && j < units.size(); j++) bytes += units[j].length;
        if (bytes > gop) gop = bytes;
    }
    glob_t parts;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s/split*.mp4", directory);
    /* A part's final sync running late only lengthens the next part. */
    assert(glob(pattern, 0, NULL, &parts) == 0 && parts.gl_pathc >= 2);
    unsigned total = 0;
    for (size_t i = 0; i < parts.gl_pathc; i++) {
        const char *part = parts.gl_pathv[i];
        unsigned count = count_fragments(part);
        assert(decoded_frames(part) == count && first_frame_key(part));
        struct stat st;
        assert(stat(part, &st) == 0);
        if (i + 1 < parts.gl_pathc) assert(count % 10 == 0 && st.st_size >= 65536);
        /* decided while nothing had reached the card */
        if (i == 0) assert((size_t)st.st_size < 65536 + gop * 2 + 4096);
        total += count;
        unlink(part);
    }
    assert(total == frames);
    printf("PASS split on IDRs from queued data: %zu parts, %u frames\n", parts.gl_pathc, total);
    globfree(&parts);
}

/* An overflow drops whole runs until an IDR; the file stays decodable. */
static void test_overflow(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/overflow.mp4", directory);
    assert(setenv("CAMERA_APP_RECORD_BUFFER_BYTES", "65536", 1) == 0);
    struct ca_mp4 *writer = NULL;
    reset_counts();
    set_hold(true);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    unsetenv("CAMERA_APP_RECORD_BUFFER_BYTES");
    int result;
    unsigned n = 0;
    write_frame(writer, n++, &result);
    assert(result == 0);
    wait_for_write_entry();
    for (; n < 100; n++) {
        write_frame(writer, n, &result);
        assert(result == 0);
    }
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    assert(stats.buffer_capacity == 65536 && stats.frames_dropped > 0);
    assert(stats.buffer_high_water <= 65536);
    set_hold(false);
    for (; n < 300; n++) {
        write_frame(writer, n, &result);
        assert(result == 0);
        drain(writer);
    }
    ca_mp4_get_stats(writer, &stats);
    assert(stats.frames_written + stats.frames_dropped == n);
    assert(stats.drop_events >= 1 && stats.frames_written > 150);
    assert(ca_mp4_close(writer) == 0);
    check_file(path, (unsigned)stats.frames_written);
    /* Every run of frames, the first and each after a gap, starts on an IDR
     * (every tenth fixture frame). */
    std::vector<sample> written = samples(path);
    assert(written.size() == stats.frames_written);
    unsigned runs = 0;
    for (size_t i = 0; i < written.size(); i++) {
        assert(written[i].frame >= 0 && written[i].frame < (long)n);
        assert(written[i].idr == (written[i].frame % 10 == 0));
        if (i == 0 || written[i].frame != written[i - 1].frame + 1) {
            assert(i == 0 || written[i].frame > written[i - 1].frame);
            assert(written[i].idr);
            runs++;
        }
    }
    assert(runs == stats.drop_events + 1);
    printf("PASS overflow: %llu frames written, %llu dropped in %llu events, every run starts "
           "on an IDR, decodable\n",
           (unsigned long long)stats.frames_written, (unsigned long long)stats.frames_dropped,
           (unsigned long long)stats.drop_events);
    unlink(path);
}

/* The card fills during a stage write: the part it took is cut back to the
 * last whole fragment, and the file keeps its index and duration. */
static void test_card_full(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/full.mp4", directory);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    int result;
    unsigned n = 0;
    /* past the first write and a timed one */
    for (; n < 15; n++) {
        write_frame(writer, n, &result);
        assert(result == 0);
        drain(writer);
        usleep(100000);
    }
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size > 0);
    pthread_mutex_lock(&lock);
    card_full = true;
    card_filled = false;
    pthread_mutex_unlock(&lock);
    uint64_t deadline = now_us() + 10000000U;
    for (result = 0; result == 0; n++) {
        write_frame(writer, n, &result);
        assert(now_us() < deadline);
        usleep(20000);
    }
    assert(result < 0 && errno == ENOSPC);
    errno = 0;
    assert(ca_mp4_close(writer) < 0 && errno == ENOSPC);
    pthread_mutex_lock(&lock);
    assert(card_filled);
    card_full = false;
    pthread_mutex_unlock(&lock);

    layout file = parse_file(path);
    assert(file.tiled && file.moov && file.sidx == 1);
    assert(file.moofs >= 1 && file.moofs < n);
    assert(file.timescale != 0 && file.duration != 0);
    assert((uint64_t)file.duration * 10 == (uint64_t)file.moofs * file.timescale);
    assert(decoded_frames(path) == file.moofs);
    printf("PASS card full: %u of %u frames kept on whole fragments, with sidx and duration\n",
           file.moofs, n);
    unlink(path);
}

/* A failed background sync fails the recording; a later write and close
 * report it. */
static void test_sync_error(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/sync-error.mp4", directory);
    struct ca_mp4 *writer = NULL;
    pthread_mutex_lock(&lock);
    fail_syncs = EIO;
    pthread_mutex_unlock(&lock);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    int result = 0;
    unsigned n = 0;
    uint64_t deadline = now_us() + 10000000U;
    for (; result == 0; n++) {
        write_frame(writer, n, &result);
        assert(now_us() < deadline);
        usleep(5000);
    }
    assert(result < 0 && errno == EIO);
    write_frame(writer, n, &result);
    assert(result < 0 && errno == EIO);
    errno = 0;
    assert(ca_mp4_close(writer) < 0 && errno == EIO);
    pthread_mutex_lock(&lock);
    fail_syncs = 0;
    pthread_mutex_unlock(&lock);
    unlink(path);
    puts("PASS sync error: fails the recording, reported by a later write and close");
}

/* Bad input costs the frame, not the recording. */
static void test_bad_input(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/bad.mp4", directory);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    uint8_t byte = 0;
    assert(ca_mp4_write_h264(writer, &byte, 0, 0, true, 54.7f) == 0);
    errno = 0;
    assert(ca_mp4_write_h264(writer, NULL, 1, 0, true, 54.7f) < 0 && errno == EINVAL);
    errno = 0;
    assert(ca_mp4_write_h264(NULL, &byte, 1, 0, true, 54.7f) < 0 && errno == EINVAL);
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    assert(stats.frames_dropped == 1 && stats.drop_events == 1);
    int result;
    for (unsigned n = 0; n < 20; n++) {
        write_frame(writer, n, &result);
        assert(result == 0);
    }
    drain(writer);
    ca_mp4_get_stats(writer, &stats);
    assert(stats.frames_written == 20 && stats.frames_dropped == 1);
    assert(ca_mp4_close(writer) == 0);
    check_file(path, 20);
    unlink(path);
    puts("PASS bad input: dropped and counted, recording continues at the next IDR");
}

/* The asynchronous close returns at once while the card is stalled, and the
 * writer thread still completes the file. */
static void test_close_async(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/async.mp4", directory);
    struct ca_mp4 *writer = NULL;
    reset_counts();
    set_hold(true);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    int result;
    for (unsigned n = 0; n < 30; n++) {
        write_frame(writer, n, &result);
        assert(result == 0);
    }
    wait_for_write_entry();
    uint64_t started = now_us();
    ca_mp4_close_async(writer);
    assert(now_us() - started < 50000);
    set_hold(false);
    uint64_t deadline = now_us() + 10000000U;
    for (;;) {
        layout file = parse_file(path);
        if (file.tiled && file.sidx == 1 && file.moofs == 30) break;
        assert(now_us() < deadline);
        usleep(10000);
    }
    check_file(path, 30);
    unlink(path);
    puts("PASS asynchronous close: returns at once, file completed in the background");
}

/* A card error fails a later write and close, neither of which waits. */
static void test_error(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/error.mp4", directory);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    pthread_mutex_lock(&lock);
    fail_writes = ENOSPC;
    pthread_mutex_unlock(&lock);
    int result = 0;
    unsigned n = 0;
    uint64_t slowest = 0;
    for (; n < 1000 && result == 0; n++) {
        uint64_t elapsed = write_frame(writer, n, &result);
        if (elapsed > slowest) slowest = elapsed;
        usleep(5000);
    }
    assert(result < 0 && errno == ENOSPC && slowest < 50000);
    write_frame(writer, n, &result);
    assert(result < 0 && errno == ENOSPC);
    uint64_t started = now_us();
    errno = 0;
    assert(ca_mp4_close(writer) < 0 && errno == ENOSPC);
    assert(now_us() - started < 50000);
    pthread_mutex_lock(&lock);
    fail_writes = 0;
    pthread_mutex_unlock(&lock);
    unlink(path);
    puts("PASS card error: reported by a later write and close without waiting");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    static std::vector<uint8_t> fixture = read_file(argv[1]);
    const uint8_t *data = fixture.data();
    size_t size = fixture.size(), start = 0, scan = 0, nal, n;
    while (start < size) {
        size_t end = size;
        bool idr = false;
        while (ca_annexb_next(data, size, &scan, &nal, &n)) {
            if (n && (data[nal] & 31) == 9 && nal > start + 4) {
                end = nal - (data[nal - 4] == 0 ? 4 : 3);
                break;
            }
            if (n && (data[nal] & 31) == 5) idr = true;
        }
        units.push_back({data + start, end - start, idr});
        start = end;
    }
    assert(units.size() % 10 == 0 && units[0].idr && units[10].idr && !units[1].idr);
    assert(mkdtemp(directory) != NULL);
    frame_thread = pthread_self();
    test_queue();
    test_stalled_card();
    test_large_writes();
    test_split();
    test_overflow();
    test_error();
    test_card_full();
    test_sync_error();
    test_bad_input();
    test_close_async();
    assert(rmdir(directory) == 0);
    return 0;
}
