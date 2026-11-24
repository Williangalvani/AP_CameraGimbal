#include "apcam/compiler.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <new>
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include "camera_app/mp4.h"
#include "camera_app/mp4_queue.h"
#include "camera_app/video_metadata.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
#include <unistd.h>

/* Linux vfat/msdos share this magic; exFAT has its own filesystem type.
 * Keep headroom for an IDR rather than ever splitting a GOP between files. */
#define CA_MSDOS_SUPER_MAGIC 0x4d44
#define CA_FAT_MAX_BYTES UINT64_C(4294967295)
#define CA_FAT_ROLL_BYTES (CA_FAT_MAX_BYTES - UINT64_C(67108863))
#define CA_MP4_HEADERS_MAX 65536U
/* The writer's staging buffer; a full CA_MP4_WRITE_BYTES, or data older than
 * CA_MP4_WRITE_MS, is written at the next frame boundary. */
#define CA_MP4_STAGE_BYTES (2U * CA_MP4_WRITE_BYTES)
#define CA_MP4_WRITE_MS 1000U
#define CA_MP4_SYNC_BYTES (16U * 1024U * 1024U)
#define CA_MP4_SYNC_MS 2000U
/* After a power cut, vfat keeps the preallocated clusters until repaired. */
#define CA_MP4_PREALLOC_BYTES (16U * 1024U * 1024U)
#define CA_MP4_TICK_MS 250U
#define CA_MP4_DROP_LOG_MS 5000U

APC_STATIC_ASSERT(sizeof(off_t) >= 8, "Recording requires 64-bit file offsets");
APC_STATIC_ASSERT(CA_MP4_QUEUE_BYTES >= 4U * 1024U * 1024U &&
                  CA_MP4_QUEUE_BYTES <= CA_MP4_QUEUE_MAX_BYTES,
                  "CA_MP4_QUEUE_BYTES must be 4 MiB to 256 MiB");

struct ca_mp4_keyframe { uint64_t offset, time; };

struct ca_mp4_file {
    int fd;
    int directory_fd;
    char *path;
    pthread_t sync_thread;
    pthread_mutex_t sync_lock;
    pthread_cond_t sync_changed;
    bool sync_started;
    bool sync_dirty;
    bool sync_stopping;
    atomic_int sync_error;
    unsigned max_sync_ms; /* under sync_lock */
    /* Muxer output waits here for a large write. Only the writer thread
     * touches the fields below. */
    uint8_t *stage;
    size_t stage_length, stage_capacity;
    uint64_t stage_at;   /* file offset of stage[0], always a fragment boundary */
    uint64_t stage_pts;  /* sample time of the fragment at stage_at */
    uint64_t staged_ms;  /* when the oldest staged byte arrived */
    uint64_t end;        /* bytes written to the file itself */
    int write_error;     /* errno of the muxer's failed output */
    uint64_t unsynced;
    uint64_t sync_requested_ms;
    bool sync_requested;
    uint64_t prealloc_end;
    bool preallocated, prealloc_failed;
    bool finished;
    /* reported at close */
    uint64_t frames, dropped;
    size_t high_water, capacity;
    unsigned max_write_ms;
    MP4E_mux_t *mux;
    mp4_h26x_writer_t h264;
    unsigned frame_duration;
    uint64_t next_pts90k;
    uint64_t bytes;
    uint64_t hard_limit;
    /* Space reserved after the moov holds a sidx written at close. Without
     * an index covering the whole file, FFmpeg-based players (Chrome among
     * them) walk every fragment before playback starts. */
    uint64_t index_at;   /* muxer offset of the reserved box; 0 before the moov */
    uint64_t shift;      /* bytes inserted there; later muxer offsets move by this */
    uint64_t mvhd_at, tkhd_at, mdhd_at; /* zero-duration headers patched at close */
    uint32_t movie_timescale, media_timescale;
    struct ca_mp4_keyframe *keyframes;
    size_t keyframe_count, keyframe_capacity;
    bool pending_key;
    bool index_failed;
    /* The reserved box shrinks to expose the sidx only once the sidx is on
     * the card; until then it still covers it. Zero when nothing waits. */
    uint32_t index_padding;
};
#define CA_MP4_INDEX_RESERVE (64U * 1024U)
#define CA_MP4_SIDX_HEADER 40U
#define CA_MP4_SIDX_ENTRY 12U

struct ca_mp4 {
    struct ca_mp4_file *file;
    char *path;
    unsigned width, height, frame_rate, part;
    uint64_t roll_bytes;
    uint8_t *headers;
    size_t headers_length;
    /* Frames pass from the callers to the writer thread, which owns the
     * files and every other field here except error. */
    struct ca_mp4_queue queue;
    pthread_t thread;
    /* set by the writer thread once its files are closed */
    atomic_int error;
    int close_error;
    /* set before the queue closes: the writer thread frees everything */
    bool detached;
    uint64_t dropped_at_open, dropped_logged, drop_logged_ms;
    /* At most one completed file can await its final background flush. */
    struct ca_mp4_file *retired;
    pthread_t retire_thread;
    atomic_bool retire_done;
    int retire_error;
};

struct ca_fmp4 {
    MP4E_mux_t *mux;
    mp4_h26x_writer_t h264;
    unsigned frame_duration;
    uint64_t next_pts90k;
    int64_t next_offset;
    ca_fmp4_write_fn write;
    void *opaque;
    bool failed;
};

static uint32_t read32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static int pwrite_all(int fd, const void *buffer, size_t size, uint64_t at)
{
    const uint8_t *data = (const uint8_t*)(buffer);
    size_t done = 0;
    while (done < size) {
        ssize_t written = pwrite(fd, data + done, size - done, (off_t)(at + done));
        if (written > 0) done += (size_t)written;
        else if (written < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

int ca_mp4_queue_init(struct ca_mp4_queue *queue, size_t capacity)
{
    if (queue == NULL || capacity < ca_mp4_queue_cost(1U)) { errno = EINVAL; return -1; }
    *queue = ca_mp4_queue{};
    queue->capacity = capacity;
    pthread_condattr_t attr;
    int result = pthread_mutex_init(&queue->lock, NULL);
    if (result != 0) { errno = result; return -1; }
    result = pthread_condattr_init(&attr);
    if (result == 0) {
#ifndef __APPLE__
        result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
        if (result == 0) result = pthread_cond_init(&queue->changed, &attr);
        pthread_condattr_destroy(&attr);
    }
    if (result != 0) {
        pthread_mutex_destroy(&queue->lock);
        errno = result;
        return -1;
    }
    return 0;
}

void ca_mp4_queue_destroy(struct ca_mp4_queue *queue)
{
    while (queue->head != NULL) {
        struct ca_mp4_frame *frame = queue->head;
        queue->head = frame->next;
        free(frame);
    }
    pthread_cond_destroy(&queue->changed);
    pthread_mutex_destroy(&queue->lock);
}

size_t ca_mp4_queue_cost(size_t length)
{
    return sizeof(struct ca_mp4_frame) + length;
}

static void drop_locked(struct ca_mp4_queue *queue, enum ca_mp4_unit unit)
{
    queue->frames_dropped++;
    if (unit == CA_MP4_UNIT_HEADERS) return;
    if (!queue->dropping) queue->drop_events++;
    queue->dropping = true;
}

void ca_mp4_queue_drop(struct ca_mp4_queue *queue)
{
    pthread_mutex_lock(&queue->lock);
    drop_locked(queue, CA_MP4_UNIT_PICTURE);
    pthread_mutex_unlock(&queue->lock);
}

int ca_mp4_queue_push(struct ca_mp4_queue *queue, const uint8_t *data, size_t length,
                      enum ca_mp4_unit unit, const struct ca_metadata *metadata,
                      const struct timespec *utc)
{
    if (length > SIZE_MAX - sizeof(struct ca_mp4_frame)) { errno = EINVAL; return -1; }
    size_t cost = ca_mp4_queue_cost(length);
    pthread_mutex_lock(&queue->lock);
    if (queue->closed) {
        pthread_mutex_unlock(&queue->lock);
        errno = EPIPE;
        return -1;
    }
    /* Later pictures reference a dropped one until the next IDR. Resuming
     * on any IDR that just fits would lose the GOP after it again. */
    size_t space = queue->capacity - queue->bytes;
    bool keep = cost <= space;
    if (queue->dropping && unit != CA_MP4_UNIT_HEADERS)
        keep = keep && unit == CA_MP4_UNIT_IDR && space >= queue->capacity / 4U;
    if (!keep) {
        drop_locked(queue, unit);
        pthread_mutex_unlock(&queue->lock);
        return 0;
    }
    if (unit != CA_MP4_UNIT_HEADERS) queue->dropping = false;
    queue->bytes += cost;
    if (queue->bytes > queue->high_water) queue->high_water = queue->bytes;
    if (queue->bytes > queue->file_high_water) queue->file_high_water = queue->bytes;
    pthread_mutex_unlock(&queue->lock);

    /* Copy outside the lock; the reservation above keeps the bound. */
    struct ca_mp4_frame *frame = (struct ca_mp4_frame*)(malloc(cost));
    pthread_mutex_lock(&queue->lock);
    if (frame == NULL) {
        queue->bytes -= cost;
        drop_locked(queue, unit);
        pthread_mutex_unlock(&queue->lock);
        return 0;
    }
    pthread_mutex_unlock(&queue->lock);
    frame->next = NULL;
    frame->data = (uint8_t*)(frame + 1);
    frame->length = length;
    frame->idr = unit == CA_MP4_UNIT_IDR;
    frame->metadata = *metadata;
    frame->utc = *utc;
    memcpy(frame->data, data, length);
    pthread_mutex_lock(&queue->lock);
    if (queue->tail != NULL) queue->tail->next = frame;
    else queue->head = frame;
    queue->tail = frame;
    pthread_cond_signal(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return 1;
}

int ca_mp4_queue_pop(struct ca_mp4_queue *queue, unsigned timeout_ms,
                     struct ca_mp4_frame **frame)
{
#ifdef __APPLE__
    // Darwin condition variables have no selectable clock.
    struct timespec remaining = {(time_t)(timeout_ms / 1000U),
                                 (long)(timeout_ms % 1000U) * 1000000L};
#else
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
#endif
    int waited = 0;
    *frame = NULL;
    pthread_mutex_lock(&queue->lock);
    while (queue->head == NULL && !queue->closed && waited == 0) {
#ifdef __APPLE__
        waited = pthread_cond_timedwait_relative_np(&queue->changed, &queue->lock, &remaining);
#else
        waited = pthread_cond_timedwait(&queue->changed, &queue->lock, &deadline);
#endif
    }
    int result = queue->closed ? -1 : 0;
    if (queue->head != NULL) {
        *frame = queue->head;
        queue->head = queue->head->next;
        if (queue->head == NULL) queue->tail = NULL;
        result = 1;
    }
    pthread_mutex_unlock(&queue->lock);
    return result;
}

void ca_mp4_queue_release(struct ca_mp4_queue *queue, struct ca_mp4_frame *frame,
                          bool written)
{
    pthread_mutex_lock(&queue->lock);
    queue->bytes -= ca_mp4_queue_cost(frame->length);
    if (written) queue->frames_written++;
    pthread_mutex_unlock(&queue->lock);
    free(frame);
}

void ca_mp4_queue_close(struct ca_mp4_queue *queue)
{
    pthread_mutex_lock(&queue->lock);
    queue->closed = true;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
}

void ca_mp4_queue_stats(struct ca_mp4_queue *queue, struct ca_mp4_stats *stats,
                        size_t *file_high_water)
{
    pthread_mutex_lock(&queue->lock);
    stats->frames_written = queue->frames_written;
    stats->frames_dropped = queue->frames_dropped;
    stats->drop_events = queue->drop_events;
    stats->buffer_bytes = queue->bytes;
    stats->buffer_high_water = queue->high_water;
    stats->buffer_capacity = queue->capacity;
    if (file_high_water != NULL) {
        *file_high_water = queue->file_high_water;
        queue->file_high_water = queue->bytes;
    }
    pthread_mutex_unlock(&queue->lock);
}

/* Classified by the first picture NAL. Stops there rather than scan the
 * slice data of every frame on the caller's thread. */
static enum ca_mp4_unit access_unit_kind(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i + 3U < length; i++) {
        if (data[i] != 0U || data[i + 1U] != 0U || data[i + 2U] != 1U) continue;
        unsigned type = data[i + 3U] & 31U;
        if (type >= 1U && type <= 5U)
            return type == 5U ? CA_MP4_UNIT_IDR : CA_MP4_UNIT_PICTURE;
        i += 2U;
    }
    return CA_MP4_UNIT_HEADERS;
}

/* The Files endpoint shares this lock only while taking its size snapshot,
 * so downloads end at a complete fragment without delaying recording. */
static int write_file(struct ca_mp4_file *writer, const void *data, size_t size, uint64_t at)
{
    if (flock(writer->fd, LOCK_EX) < 0) return -1;
    uint64_t started = monotonic_ms();
    int result = pwrite_all(writer->fd, data, size, at);
    int saved_errno = errno;
    unsigned elapsed = (unsigned)(monotonic_ms() - started);
    if (elapsed > writer->max_write_ms) writer->max_write_ms = elapsed;
    if (flock(writer->fd, LOCK_UN) < 0 && result == 0) return -1;
    if (result < 0) { errno = saved_errno; return -1; }
    if (at + size > writer->end) writer->end = at + size;
    writer->unsynced += size;
    return 0;
}

/* Called only between frames, so every write ends on a whole fragment. */
static int flush_stage(struct ca_mp4_file *writer)
{
    if (writer->stage_length == 0U) return 0;
    if (write_file(writer, writer->stage, writer->stage_length, writer->stage_at) < 0) return -1;
    writer->stage_at += writer->stage_length;
    writer->stage_length = 0U;
    writer->stage_pts = writer->next_pts90k;
    return 0;
}

/* Forget the output from file offset at, where the fragment for sample time
 * pts starts: a frame the muxer could not finish, or a stage the card
 * refused, perhaps after taking part of it. The file then ends on a whole
 * box, and the index and durations cover only what it holds. */
static void discard_from(struct ca_mp4_file *writer, uint64_t at, uint64_t pts)
{
    if (at <= writer->stage_at) {
        writer->stage_length = 0U;
        writer->stage_at = at;
        if (flock(writer->fd, LOCK_EX) == 0) {
            (void)ftruncate(writer->fd, (off_t)at);
            (void)flock(writer->fd, LOCK_UN);
        }
        writer->end = at;
    } else if (at - writer->stage_at < writer->stage_length) {
        writer->stage_length = (size_t)(at - writer->stage_at);
    }
    writer->bytes = at;
    writer->next_pts90k = pts;
    while (writer->keyframe_count != 0U &&
           writer->keyframes[writer->keyframe_count - 1U].offset >= at)
        writer->keyframe_count--;
}

/* The muxer only appends; the reserved index leaves a gap, which is zero as
 * a hole would read. Output is never split across writes mid-fragment, so a
 * stage that cannot grow fails the recording. */
static int stage_write(struct ca_mp4_file *writer, const uint8_t *data, size_t size, uint64_t at)
{
    uint64_t end = writer->stage_at + writer->stage_length;
    if (at < writer->stage_at || (at > end && at - end > CA_MP4_INDEX_RESERVE)) {
        errno = EIO;
        return -1;
    }
    size_t offset = (size_t)(at - writer->stage_at);
    if (offset + size > writer->stage_capacity) {
        size_t capacity = writer->stage_capacity ? writer->stage_capacity : CA_MP4_STAGE_BYTES;
        while (capacity < offset + size && capacity <= SIZE_MAX / 2U) capacity *= 2U;
        uint8_t *grown = capacity >= offset + size
            ? (uint8_t*)(realloc(writer->stage, capacity)) : NULL;
        if (grown == NULL) { errno = ENOMEM; return -1; }
        writer->stage = grown;
        writer->stage_capacity = capacity;
    }
    if (writer->stage_length == 0U) writer->staged_ms = monotonic_ms();
    if (offset > writer->stage_length)
        memset(writer->stage + writer->stage_length, 0, offset - writer->stage_length);
    memcpy(writer->stage + offset, data, size);
    if (offset + size > writer->stage_length) writer->stage_length = offset + size;
    return 0;
}

/* Remember where the version 0 duration fields of the moov live. */
static void locate_headers(struct ca_mp4_file *writer, const uint8_t *moov, size_t size, uint64_t at)
{
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t box = read32(moov + pos);
        if (box < 8 || box > size - pos) return;
        if (!memcmp(moov + pos + 4, "mvhd", 4) && box >= 28 && moov[pos + 8] == 0) {
            writer->mvhd_at = at + pos;
            writer->movie_timescale = read32(moov + pos + 20);
        } else if (!memcmp(moov + pos + 4, "trak", 4)) {
            size_t p2 = pos + 8;
            while (p2 + 8 <= pos + box) {
                uint32_t b2 = read32(moov + p2);
                if (b2 < 8 || b2 > pos + box - p2) return;
                if (!memcmp(moov + p2 + 4, "tkhd", 4) && b2 >= 32 && moov[p2 + 8] == 0 && !writer->tkhd_at)
                    writer->tkhd_at = at + p2;
                if (!memcmp(moov + p2 + 4, "mdia", 4)) {
                    size_t p3 = p2 + 8;
                    while (p3 + 8 <= p2 + b2) {
                        uint32_t b3 = read32(moov + p3);
                        if (b3 < 8 || b3 > p2 + b2 - p3) return;
                        if (!memcmp(moov + p3 + 4, "mdhd", 4) && b3 >= 28 && moov[p3 + 8] == 0 && !writer->mdhd_at) {
                            writer->mdhd_at = at + p3;
                            writer->media_timescale = read32(moov + p3 + 20);
                        }
                        p3 += b3;
                    }
                }
                p2 += b2;
            }
        }
        pos += box;
    }
}

static void note_keyframe(struct ca_mp4_file *writer, uint64_t offset)
{
    if (writer->index_failed) return;
    if (writer->keyframe_count == writer->keyframe_capacity) {
        size_t capacity = writer->keyframe_capacity ? writer->keyframe_capacity * 2 : 256;
        struct ca_mp4_keyframe *grown = (struct ca_mp4_keyframe*)(realloc(writer->keyframes, capacity * sizeof(*grown)));
        if (grown == NULL) { writer->index_failed = true; return; }
        writer->keyframes = grown;
        writer->keyframe_capacity = capacity;
    }
    writer->keyframes[writer->keyframe_count++] =
        (struct ca_mp4_keyframe){offset, writer->next_pts90k};
}

static int write_at(int64_t offset, const void *buffer, size_t size, void *opaque)
{
    struct ca_mp4_file *writer = (struct ca_mp4_file*)(opaque);
    const uint8_t *data = (const uint8_t*)(buffer);

    if (offset < 0) { writer->write_error = EINVAL; return 1; }
    uint64_t at = (uint64_t)offset;
    if (writer->shift && at >= writer->index_at) at += writer->shift;
    if (at > writer->hard_limit || size > writer->hard_limit - at) {
        writer->write_error = EFBIG;
        return 1;
    }
    if (stage_write(writer, data, size, at) < 0) { writer->write_error = errno; return 1; }
    uint64_t end = at + size;
    if (end > writer->bytes) writer->bytes = end;
    if (size >= 8 && !memcmp(data + 4, "moof", 4) && writer->pending_key) {
        note_keyframe(writer, at);
    } else if (size >= 8 && !memcmp(data + 4, "moov", 4) && !writer->shift) {
        /* reserve the index space as a free box right after the moov */
        uint8_t box[8];
        put32(box, CA_MP4_INDEX_RESERVE);
        memcpy(box + 4, "free", 4);
        if (end + CA_MP4_INDEX_RESERVE > writer->hard_limit) {
            writer->write_error = EFBIG;
            return 1;
        }
        if (stage_write(writer, box, sizeof(box), end) < 0) { writer->write_error = errno; return 1; }
        locate_headers(writer, data, size, at);
        writer->index_at = end;
        writer->shift = CA_MP4_INDEX_RESERVE;
        writer->bytes = end + CA_MP4_INDEX_RESERVE;
    }
    return 0;
}

/* End the reserved box with a sidx of keyframe-aligned references covering
 * the file, so the first fragment directly follows the index (Chrome ignores
 * an index whose first_offset skips padding), and give the headers the final
 * duration. Shrinking the box to expose the sidx waits for expose_index. */
static int write_index(struct ca_mp4_file *writer)
{
    if (!writer->shift || writer->index_failed || writer->keyframe_count == 0) return 0;
    size_t max_refs = (CA_MP4_INDEX_RESERVE - 8U - CA_MP4_SIDX_HEADER) / CA_MP4_SIDX_ENTRY;
    size_t stride = (writer->keyframe_count + max_refs - 1) / max_refs;
    size_t count = (writer->keyframe_count + stride - 1) / stride;
    size_t sidx_size = CA_MP4_SIDX_HEADER + count * CA_MP4_SIDX_ENTRY;
    uint8_t *box = (uint8_t*)(calloc(1, sidx_size));
    if (box == NULL) return -1;
    put32(box, (uint32_t)sidx_size);
    memcpy(box + 4, "sidx", 4);
    box[8] = 1; /* version 1: 64-bit time and offset */
    put32(box + 12, 1); /* reference_ID: the video track */
    put32(box + 16, writer->media_timescale ? writer->media_timescale : 90000U);
    put64(box + 20, writer->keyframes[0].time);
    put64(box + 28, 0); /* first_offset: the first moof follows directly */
    box[38] = (uint8_t)(count >> 8);
    box[39] = (uint8_t)count;
    uint8_t *entry = box + CA_MP4_SIDX_HEADER;
    for (size_t i = 0; i < count; i++, entry += CA_MP4_SIDX_ENTRY) {
        const struct ca_mp4_keyframe *start = &writer->keyframes[i * stride];
        struct ca_mp4_keyframe next = {writer->bytes, writer->next_pts90k};
        if ((i + 1) * stride < writer->keyframe_count) next = writer->keyframes[(i + 1) * stride];
        uint64_t bytes = next.offset - start->offset, duration = next.time - start->time;
        if (bytes >= 0x80000000ULL || duration > UINT32_MAX) { free(box); return 0; }
        put32(entry, (uint32_t)bytes);
        put32(entry + 4, (uint32_t)duration);
        put32(entry + 8, 0x90000000U); /* starts with a type 1 SAP at delta 0 */
    }
    int result = write_file(writer, box, sidx_size,
                            writer->index_at + CA_MP4_INDEX_RESERVE - sidx_size);
    free(box);
    if (result < 0) return -1;
    writer->index_padding = (uint32_t)(CA_MP4_INDEX_RESERVE - sidx_size);
    uint64_t media = writer->next_pts90k;
    uint32_t ts = writer->media_timescale ? writer->media_timescale : 90000U;
    uint64_t movie = media * writer->movie_timescale / ts;
    uint8_t value[4];
    if (writer->mdhd_at && media <= UINT32_MAX) {
        put32(value, (uint32_t)media);
        if (write_file(writer, value, 4, writer->mdhd_at + 24) < 0) return -1;
    }
    if (movie <= UINT32_MAX) {
        put32(value, (uint32_t)movie);
        if (writer->mvhd_at && write_file(writer, value, 4, writer->mvhd_at + 24) < 0) return -1;
        if (writer->tkhd_at && write_file(writer, value, 4, writer->tkhd_at + 28) < 0) return -1;
    }
    return 0;
}

/* Only once the sidx is synced: a shrunk header on the card without the sidx
 * after it would leave zeros where a power cut expects a box. */
static int expose_index(struct ca_mp4_file *writer)
{
    if (!writer->index_padding) return 0;
    uint8_t padding[8];
    put32(padding, writer->index_padding);
    memcpy(padding + 4, "free", 4);
    writer->index_padding = 0U;
    if (write_file(writer, padding, sizeof(padding), writer->index_at) < 0) return -1;
    return fdatasync(writer->fd);
}

static int recording_directory(const char *path)
{
    char *parent = strdup(path);
    if (parent == NULL) return -1;
    char *slash = strrchr(parent, '/');
    if (slash != NULL) {
        if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
    }
    int fd = open(slash != NULL ? parent : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    return fd;
}

/* The worker never holds the frame-write flock or sync mutex during I/O.
 * The writer thread requests flushes; requests coalesce while one runs, so a
 * slow card cannot build a queue of flushes. Only close waits for this worker
 * and its final flush. */
static void *sync_recording(void *opaque)
{
    struct ca_mp4_file *writer = (struct ca_mp4_file*)(opaque);
    pthread_mutex_lock(&writer->sync_lock);
    for (;;) {
        while (!writer->sync_stopping && !writer->sync_dirty)
            pthread_cond_wait(&writer->sync_changed, &writer->sync_lock);
        bool stopping = writer->sync_stopping;
        writer->sync_dirty = false;
        pthread_mutex_unlock(&writer->sync_lock);
        uint64_t started = monotonic_ms();
        int error = 0;
        if (fdatasync(writer->fd) < 0) error = errno;
        if (error == 0 && writer->directory_fd >= 0) {
            if (fsync(writer->directory_fd) < 0) error = errno;
            else {
                close(writer->directory_fd);
                writer->directory_fd = -1;
            }
        }
        unsigned elapsed = (unsigned)(monotonic_ms() - started);
        pthread_mutex_lock(&writer->sync_lock);
        if (elapsed > writer->max_sync_ms) writer->max_sync_ms = elapsed;
        if (error != 0) atomic_store(&writer->sync_error, error);
        if (error != 0 || stopping) break;
    }
    pthread_mutex_unlock(&writer->sync_lock);
    return NULL;
}

static int start_recording_sync(struct ca_mp4_file *writer)
{
    int result = pthread_mutex_init(&writer->sync_lock, NULL);
    if (result != 0) { errno = result; return -1; }
    result = pthread_cond_init(&writer->sync_changed, NULL);
    if (result != 0) goto fail_mutex;
    result = pthread_create(&writer->sync_thread, NULL, sync_recording, writer);
    if (result != 0) {
        pthread_cond_destroy(&writer->sync_changed);
        goto fail_mutex;
    }
    writer->sync_started = true;
    return 0;
fail_mutex:
    pthread_mutex_destroy(&writer->sync_lock);
    errno = result;
    return -1;
}

static int file_open(struct ca_mp4_file **result, const char *path,
                     unsigned width, unsigned height, unsigned frame_rate)
{
    struct ca_mp4_file *writer;

    if (result == NULL || path == NULL || width == 0U || height == 0U ||
        frame_rate == 0U) {
        errno = EINVAL;
        return -1;
    }
    writer = new (std::nothrow) ca_mp4_file{};
    if (writer == NULL) return -1;
    atomic_init(&writer->sync_error, 0);
    writer->path = strdup(path);
    writer->directory_fd = writer->path != NULL ? recording_directory(path) : -1;
    if (writer->directory_fd < 0) {
        free(writer->path);
        delete writer;
        return -1;
    }
    writer->fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (writer->fd < 0) {
        int saved_errno = errno;
        close(writer->directory_fd);
        free(writer->path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    struct statfs fs;
    if (fstatfs(writer->fd, &fs) < 0) {
        int saved_errno = errno;
        close(writer->fd);
        close(writer->directory_fd);
        unlink(path);
        free(writer->path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
#ifdef __APPLE__
    writer->hard_limit = !strcmp(fs.f_fstypename, "msdos") ? CA_FAT_MAX_BYTES : INT64_MAX;
#else
    writer->hard_limit = fs.f_type == CA_MSDOS_SUPER_MAGIC ? CA_FAT_MAX_BYTES : INT64_MAX;
#endif
    writer->frame_duration = 90000U / frame_rate;
    /* Append-only fragments keep completed frames playable before close and
     * after a crash; the movie header is emitted with the first video frame. */
    writer->mux = MP4E_open(1, 1, writer, write_at);
    if (writer->mux == NULL ||
        mp4_h26x_write_init(&writer->h264, writer->mux,
                            (int)width, (int)height, 0) != MP4E_STATUS_OK ||
        start_recording_sync(writer) < 0) {
        int saved_errno = errno != 0 ? errno : EIO;
        if (writer->mux != NULL) (void)MP4E_close(writer->mux);
        close(writer->fd);
        close(writer->directory_fd);
        unlink(path);
        free(writer->path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    *result = writer;
    return 0;
}

/* Each call is one complete encoder access unit, including all slices and
 * SEI. Use MP4E_put_sample once: treating each NAL as a picture breaks SEI
 * association and fragmented MP4 with multi-slice encoders. */
static int write_h264(mp4_h26x_writer_t *h264, unsigned frame_duration,
                      const uint8_t *annex_b, size_t length, uint64_t pts90k,
                      char *json, size_t *json_length,
                      const struct ca_metadata *metadata, const struct timespec *utc)
{
    size_t offset = 0U, nal, nal_length, used = 0U;
    bool picture = false, key = false;
    uint8_t sei[CA_VIDEO_METADATA_SEI_MAX];
    if (annex_b == NULL || length == 0U || length > INT_MAX - sizeof(sei)) {
        errno = EINVAL;
        return -1;
    }
    while (ca_annexb_next(annex_b, length, &offset, &nal, &nal_length)) {
        if (nal_length == 0U) continue;
        unsigned type = annex_b[nal] & 31U;
        int result = MP4E_STATUS_OK;
        if (type == 7U) {
            result = MP4E_set_sps(h264->mux, h264->mux_track_id, annex_b + nal, (int)nal_length);
            h264->need_sps = 0;
        } else if (type == 8U) {
            result = MP4E_set_pps(h264->mux, h264->mux_track_id, annex_b + nal, (int)nal_length);
            h264->need_pps = 0;
        } else if (ca_video_nal_is_vcl(CA_VIDEO_H264, annex_b[nal])) {
            picture = true;
            if (type == 5U) key = true;
        }
        if (result != MP4E_STATUS_OK) { errno = EIO; return -1; }
    }
    if (!picture) return 0;
    if (h264->need_sps || h264->need_pps || (h264->need_idr && !key)) return 0;
    *json_length = ca_video_metadata_json(metadata, utc, pts90k,
                                         json, CA_VIDEO_METADATA_JSON_MAX);
    size_t sei_length = ca_video_metadata_sei(CA_VIDEO_H264, json, *json_length,
                                              sei, sizeof(sei));
    if (sei_length == 0U) { errno = EOVERFLOW; return -1; }
    /* Three-byte Annex-B start codes expand by one byte in MP4. */
    if (length > (INT_MAX - sizeof(sei)) / 2U) { errno = EOVERFLOW; return -1; }
    uint8_t *sample = (uint8_t*)(malloc(length * 2U + sizeof(sei)));
    if (sample == NULL) return -1;
#define APPEND_NAL(data, size) do { \
    size_t n = (size); \
    sample[used++] = (uint8_t)(n >> 24); sample[used++] = (uint8_t)(n >> 16); \
    sample[used++] = (uint8_t)(n >> 8); sample[used++] = (uint8_t)n; \
    memcpy(sample + used, (data), n); used += n; \
} while (0)
    bool inserted = false;
    offset = 0U;
    while (ca_annexb_next(annex_b, length, &offset, &nal, &nal_length)) {
        if (nal_length == 0U) continue;
        unsigned type = annex_b[nal] & 31U;
        bool vcl = ca_video_nal_is_vcl(CA_VIDEO_H264, annex_b[nal]);
        if (vcl && !inserted) {
            APPEND_NAL(sei + 4U, sei_length - 4U);
            inserted = true;
        }
        if (vcl || type == 6U) APPEND_NAL(annex_b + nal, nal_length);
    }
#undef APPEND_NAL
    int result = MP4E_put_sample(h264->mux, h264->mux_track_id,
                                 sample, (int)used, (int)frame_duration,
                                 key ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT);
    free(sample);
    if (result != MP4E_STATUS_OK) { errno = EIO; return -1; }
    h264->need_idr = 0;
    return 1;
}

static int write_recording_frame(struct ca_mp4_file *writer, const uint8_t *annex_b,
                                 size_t length, const struct ca_metadata *metadata,
                                 const struct timespec *utc)
{
    /* The encoders provide platform-specific PTS. MP4 has always used the
     * configured constant frame rate; telemetry uses that same sample clock. */
    char json[CA_VIDEO_METADATA_JSON_MAX];
    size_t json_length = 0U;
    if (writer == NULL) { errno = EINVAL; return -1; }
    /* index the fragments that start with an IDR, as the muxer marks them */
    size_t offset = 0U, nal, nal_length;
    writer->pending_key = false;
    while (ca_annexb_next(annex_b, length, &offset, &nal, &nal_length)) {
        if (nal_length != 0U && (annex_b[nal] & 31U) == 5U) writer->pending_key = true;
    }
    uint64_t frame_at = writer->bytes;
    writer->write_error = 0;
    int result = write_h264(&writer->h264, writer->frame_duration, annex_b, length,
                            writer->next_pts90k, json, &json_length, metadata, utc);
    if (result < 0) {
        int error = writer->write_error ? writer->write_error : errno;
        discard_from(writer, frame_at, writer->next_pts90k);
        errno = error;
        return -1;
    }
    if (result == 0) return 0;
    writer->next_pts90k += writer->frame_duration;
    return 1;
}

/* Returns 1 for a video sample, 0 for headers alone or a frame the muxer
 * skips before the first IDR. */
static int file_write_h264(struct ca_mp4_file *writer, const uint8_t *annex_b,
                           size_t length, const struct ca_metadata *metadata,
                           const struct timespec *utc)
{
    if (writer == NULL) { errno = EINVAL; return -1; }
    int sync_error = atomic_load(&writer->sync_error);
    if (sync_error != 0) { errno = sync_error; return -1; }
    int result = write_recording_frame(writer, annex_b, length, metadata, utc);
    if (result > 0) writer->frames++;
    return result;
}

/* Allocate ahead of the data in large steps without changing the file size,
 * so the size, downloads and crash recovery see only data. FALLOC_FL_KEEP_SIZE
 * rather than mode 0, which on vfat zero-fills the range through the page
 * cache and moves EOF past the data. Not every filesystem supports it. */
static void preallocate(struct ca_mp4_file *writer)
{
#if defined(__linux__) && !defined(__UCLIBC__) && defined(FALLOC_FL_KEEP_SIZE)
    if (writer->prealloc_failed ||
        writer->end + CA_MP4_PREALLOC_BYTES / 2U <= writer->prealloc_end) return;
    uint64_t end = writer->end + CA_MP4_PREALLOC_BYTES;
    if (end > writer->hard_limit) end = writer->hard_limit;
    if (end <= writer->end) return;
    if (fallocate(writer->fd, FALLOC_FL_KEEP_SIZE, (off_t)writer->end,
                  (off_t)(end - writer->end)) == 0) {
        writer->prealloc_end = end;
        writer->preallocated = true;
        return;
    }
    if (errno != EOPNOTSUPP && errno != ENOSYS)
        fprintf(stderr, "recording preallocation stopped for %s: %s\n",
                writer->path, strerror(errno));
    writer->prealloc_failed = true;
#else
    (void)writer;
#endif
}

/* Large writes at frame boundaries, and a data sync after the first write,
 * then every CA_MP4_SYNC_BYTES or CA_MP4_SYNC_MS. A new file's headers and
 * first IDR are written and synced, with its directory entry, at once. */
static int file_tick(struct ca_mp4_file *writer)
{
    int sync_error = atomic_load(&writer->sync_error);
    if (sync_error != 0) { errno = sync_error; return -1; }
    uint64_t now = monotonic_ms();
    if ((writer->frames != 0U && !writer->sync_requested) ||
        writer->stage_length >= CA_MP4_WRITE_BYTES ||
        (writer->stage_length != 0U && now - writer->staged_ms >= CA_MP4_WRITE_MS)) {
        if (flush_stage(writer) < 0) {
            int error = errno;
            discard_from(writer, writer->stage_at, writer->stage_pts);
            errno = error;
            return -1;
        }
        preallocate(writer);
    }
    if (writer->unsynced != 0U &&
        (!writer->sync_requested || writer->unsynced >= CA_MP4_SYNC_BYTES ||
         now - writer->sync_requested_ms >= CA_MP4_SYNC_MS)) {
        writer->unsynced = 0U;
        writer->sync_requested = true;
        writer->sync_requested_ms = now;
        pthread_mutex_lock(&writer->sync_lock);
        if (!writer->sync_dirty) {
            writer->sync_dirty = true;
            pthread_cond_signal(&writer->sync_changed);
        }
        pthread_mutex_unlock(&writer->sync_lock);
    }
    return 0;
}

/* Write what remains, then patch the index and durations at their offsets.
 * Those patches land in space the file already has, so a full card still
 * gets them for the data it kept. */
static int file_finish(struct ca_mp4_file *writer)
{
    if (writer->finished) return 0;
    writer->finished = true;
    int error = 0;
    mp4_h26x_write_close(&writer->h264);
    if (MP4E_close(writer->mux) != MP4E_STATUS_OK) error = EIO;
    if (flush_stage(writer) < 0) {
        if (!error) error = errno ? errno : EIO;
        discard_from(writer, writer->stage_at, writer->stage_pts);
    }
    if (write_index(writer) < 0 && !error) error = errno ? errno : EIO;
    /* return the space allocated past the data */
    if (writer->preallocated && ftruncate(writer->fd, (off_t)writer->end) < 0 && !error)
        error = errno ? errno : EIO;
    free(writer->keyframes);
    writer->keyframes = NULL;
    free(writer->stage);
    writer->stage = NULL;
    writer->stage_length = 0U;
    if (error) { errno = error; return -1; }
    return 0;
}

static int file_close(struct ca_mp4_file *writer)
{
    if (writer == NULL) return 0;
    int error = file_finish(writer) < 0 ? (errno ? errno : EIO) : 0;
    unsigned max_sync_ms = 0U;
    if (writer->sync_started) {
        pthread_mutex_lock(&writer->sync_lock);
        writer->sync_stopping = true;
        pthread_cond_signal(&writer->sync_changed);
        pthread_mutex_unlock(&writer->sync_lock);
        pthread_join(writer->sync_thread, NULL);
        max_sync_ms = writer->max_sync_ms;
        pthread_cond_destroy(&writer->sync_changed);
        pthread_mutex_destroy(&writer->sync_lock);
    }
    int sync_error = atomic_load(&writer->sync_error);
    if (sync_error != 0) error = sync_error;
    else if (expose_index(writer) < 0 && error == 0) error = errno ? errno : EIO;
    if (close(writer->fd) < 0 && error == 0) error = errno ? errno : EIO;
    if (writer->directory_fd >= 0) close(writer->directory_fd);
    unsigned percent = writer->capacity
        ? (unsigned)((uint64_t)writer->high_water * 100U / writer->capacity) : 0U;
    /* fewer than were muxed if a failed write was cut back */
    uint64_t kept = writer->frame_duration
        ? writer->next_pts90k / writer->frame_duration : writer->frames;
    if (writer->frames != 0U)
        fprintf(stderr, "recording closed %s: %" PRIu64 " frames written, %" PRIu64
                " dropped, buffer high-water %zu bytes (%u%%), %" PRIu64
                " bytes, max write %u ms, max sync %u ms%s%s\n",
                writer->path, kept, writer->dropped, writer->high_water, percent,
                writer->end, writer->max_write_ms, max_sync_ms,
                error ? ", error: " : "", error ? strerror(error) : "");
    free(writer->path);
    delete writer;
    if (error != 0) { errno = error; return -1; }
    return 0;
}

/* Retain all distinct parameter sets, including headers sent separately from
 * IDRs. A continuation must decode even when the encoder never repeats them. */
static int cache_headers(struct ca_mp4 *writer, const uint8_t *data, size_t length,
                         bool *idr)
{
    size_t offset = 0, nal, n;
    *idr = false;
    while (ca_annexb_next(data, length, &offset, &nal, &n)) {
        if (!n) continue;
        unsigned type = data[nal] & 31U;
        if (type == 5U) *idr = true;
        if (type != 7U && type != 8U) continue;
        size_t scan = 0, old, old_length;
        bool found = false;
        while (ca_annexb_next(writer->headers, writer->headers_length,
                             &scan, &old, &old_length)) {
            if (old_length == n && !memcmp(writer->headers + old, data + nal, n)) {
                found = true;
                break;
            }
        }
        if (found) continue;
        if (writer->headers_length > CA_MP4_HEADERS_MAX - 4U ||
            n > CA_MP4_HEADERS_MAX - 4U - writer->headers_length) {
            errno = EOVERFLOW;
            return -1;
        }
        uint8_t *headers = (uint8_t*)(realloc(writer->headers, writer->headers_length + 4U + n));
        if (!headers) return -1;
        writer->headers = headers;
        memcpy(headers + writer->headers_length, "\0\0\0\1", 4);
        memcpy(headers + writer->headers_length + 4, data + nal, n);
        writer->headers_length += 4U + n;
    }
    return 0;
}

static void *retire_file(void *opaque)
{
    struct ca_mp4 *writer = (struct ca_mp4*)(opaque);
    writer->retire_error = file_close(writer->retired) < 0 ? (errno ? errno : EIO) : 0;
    atomic_store(&writer->retire_done, true);
    return NULL;
}

static int reap_file(struct ca_mp4 *writer, bool wait)
{
    if (!writer->retired) return 0;
    if (!wait && !atomic_load(&writer->retire_done)) return 1;
    pthread_join(writer->retire_thread, NULL);
    writer->retired = NULL;
    if (writer->retire_error) { errno = writer->retire_error; return -1; }
    return 0;
}

/* Drops and the buffer's peak while the current file was open. */
static void note_file_stats(struct ca_mp4 *writer)
{
    struct ca_mp4_stats stats;
    size_t high_water;
    ca_mp4_queue_stats(&writer->queue, &stats, &high_water);
    if (writer->file != NULL) {
        writer->file->dropped = stats.frames_dropped - writer->dropped_at_open;
        writer->file->high_water = high_water;
        writer->file->capacity = stats.buffer_capacity;
    }
    writer->dropped_at_open = stats.frames_dropped;
}

static int roll_file(struct ca_mp4 *writer, const struct ca_mp4_frame *frame)
{
    char *path = NULL;
    const char *slash = strrchr(writer->path, '/');
    const char *extension = strrchr(writer->path, '.');
    if (!extension || (slash && extension < slash)) extension = writer->path + strlen(writer->path);
    size_t stem = (size_t)(extension - writer->path);
    if (stem > INT_MAX || writer->part == UINT_MAX) { errno = EOVERFLOW; return -1; }
    if (asprintf(&path, "%.*s_part%04u%s", (int)stem, writer->path,
                 writer->part + 1U, extension) < 0) return -1;
    struct ca_mp4_file *next = NULL;
    if (file_open(&next, path, writer->width, writer->height, writer->frame_rate) < 0) {
        free(path);
        return -1;
    }
    {
    /* Seed the decoder configuration without adding a video sample or SEI. */
    if (writer->headers_length &&
        file_write_h264(next, writer->headers, writer->headers_length,
                        &frame->metadata, &frame->utc) < 0)
        goto fail;
    /* Complete the old part here; only its final sync is left to wait. */
    note_file_stats(writer);
    if (file_finish(writer->file) < 0) goto fail;
    writer->retired = writer->file;
    atomic_store(&writer->retire_done, false);
    int error = pthread_create(&writer->retire_thread, NULL, retire_file, writer);
    if (error) {
        writer->retired = NULL;
        errno = error;
        goto fail;
    }
    writer->file = next;
    writer->part++;
    fprintf(stderr, "recording continued in %s\n", path);
    free(path);
    return 0;
    }
fail:;
    int saved_errno = errno;
    (void)file_close(next);
    unlink(path);
    free(path);
    errno = saved_errno;
    return -1;
}

static int record_frame(struct ca_mp4 *writer, const struct ca_mp4_frame *frame)
{
    int retired = reap_file(writer, false);
    if (retired < 0) return -1;
    /* The writer has muxed every earlier frame, so the size includes all of
     * them, written or not. */
    if (writer->roll_bytes) {
        bool idr;
        if (cache_headers(writer, frame->data, frame->length, &idr) < 0) return -1;
        if (idr && writer->file->bytes >= writer->roll_bytes && retired == 0) {
            if (roll_file(writer, frame) < 0) return -1;
        }
    }
    /* Refuse an access unit before writing any of it if even the conservative
     * size bound reaches the filesystem limit. Broken encoders without IDRs
     * and indefinitely blocked final syncs must not grow a file past FAT32.
     * Include start-code expansion, telemetry, decoder headers and MP4 boxes. */
    uint64_t bound = (uint64_t)frame->length * 2U + CA_VIDEO_METADATA_SEI_MAX +
                     CA_MP4_HEADERS_MAX + 4096U;
    if (bound > writer->file->hard_limit - writer->file->bytes) {
        errno = EFBIG;
        return -1;
    }
    return file_write_h264(writer->file, frame->data, frame->length,
                           &frame->metadata, &frame->utc);
}

/* Close every file before publishing the error, so the caller's close after
 * a failed write never waits for the card. */
static void fail_recording(struct ca_mp4 *writer, int error)
{
    note_file_stats(writer);
    (void)file_close(writer->file);
    writer->file = NULL;
    (void)reap_file(writer, true);
    atomic_store(&writer->error, error);
}

static void log_drops(struct ca_mp4 *writer)
{
    struct ca_mp4_stats stats;
    ca_mp4_queue_stats(&writer->queue, &stats, NULL);
    if (stats.frames_dropped == writer->dropped_logged) return;
    uint64_t now = monotonic_ms();
    if (writer->drop_logged_ms != 0U && now - writer->drop_logged_ms < CA_MP4_DROP_LOG_MS) return;
    fprintf(stderr, "recording buffer full: %" PRIu64 " frames dropped in %" PRIu64
            " events (%zu of %zu bytes queued); recording resumes at an IDR\n",
            stats.frames_dropped, stats.drop_events, stats.buffer_bytes,
            stats.buffer_capacity);
    writer->dropped_logged = stats.frames_dropped;
    writer->drop_logged_ms = now;
}

static int writer_error(struct ca_mp4 *writer)
{
    int error = atomic_load(&writer->error);
    return error ? error : writer->close_error;
}

static void destroy_writer(struct ca_mp4 *writer)
{
    ca_mp4_queue_destroy(&writer->queue);
    free(writer->headers);
    free(writer->path);
    delete writer;
}

/* All file I/O after open: mux, write, roll, sync requests and close. After
 * an error, frames are discarded until close. */
static void *write_frames(void *opaque)
{
    struct ca_mp4 *writer = (struct ca_mp4*)(opaque);
    for (;;) {
        struct ca_mp4_frame *frame = NULL;
        int popped = ca_mp4_queue_pop(&writer->queue, CA_MP4_TICK_MS, &frame);
        if (popped < 0) break;
        bool written = false;
        if (atomic_load(&writer->error) == 0) {
            int result = frame != NULL ? record_frame(writer, frame) : 0;
            written = result > 0;
            if (result >= 0) result = file_tick(writer->file);
            if (result < 0) fail_recording(writer, errno ? errno : EIO);
        }
        if (frame != NULL) ca_mp4_queue_release(&writer->queue, frame, written);
        log_drops(writer);
    }
    if (atomic_load(&writer->error) == 0) {
        int error = 0;
        note_file_stats(writer);
        if (file_close(writer->file) < 0) error = errno ? errno : EIO;
        writer->file = NULL;
        if (reap_file(writer, true) < 0 && !error) error = errno ? errno : EIO;
        writer->close_error = error;
    }
    if (writer->detached) {
        int error = writer_error(writer);
        if (error)
            fprintf(stderr, "recording %s closed in the background, error: %s\n",
                    writer->path, strerror(error));
        destroy_writer(writer);
    }
    return NULL;
}

static size_t queue_capacity(void)
{
    size_t capacity = CA_MP4_QUEUE_BYTES;
    /* A smaller buffer permits bounded overflow tests; it cannot grow past
     * the build's. */
    const char *setting = getenv("CAMERA_APP_RECORD_BUFFER_BYTES");
    if (setting && *setting >= '0' && *setting <= '9') {
        char *end;
        errno = 0;
        unsigned long long bytes = strtoull(setting, &end, 10);
        if (!errno && !*end && bytes >= CA_MP4_QUEUE_MIN_BYTES && bytes < capacity)
            capacity = (size_t)bytes;
    }
    return capacity;
}

int ca_mp4_open(struct ca_mp4 **result, const char *path,
                unsigned width, unsigned height, unsigned frame_rate)
{
    if (!result || !path || !width || !height || !frame_rate) { errno = EINVAL; return -1; }
    struct ca_mp4 *writer = new (std::nothrow) ca_mp4{};
    if (!writer) return -1;
    writer->path = strdup(path);
    if (!writer->path) { delete writer; return -1; }
    if (file_open(&writer->file, path, width, height, frame_rate) < 0) {
        int saved_errno = errno;
        free(writer->path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    writer->width = width;
    writer->height = height;
    writer->frame_rate = frame_rate;
    writer->part = 1;
    atomic_init(&writer->retire_done, false);
    atomic_init(&writer->error, 0);
    if (writer->file->hard_limit == CA_FAT_MAX_BYTES) {
        writer->roll_bytes = CA_FAT_ROLL_BYTES;
        /* A lower FAT threshold permits bounded bench tests. It cannot enable
         * splitting on exFAT or raise the production safety threshold. */
        const char *setting = getenv("CAMERA_APP_RECORD_SEGMENT_BYTES");
        if (setting && *setting >= '0' && *setting <= '9') {
            char *end;
            errno = 0;
            unsigned long long bytes = strtoull(setting, &end, 10);
            if (!errno && !*end && bytes >= 65536 && bytes < writer->roll_bytes)
                writer->roll_bytes = bytes;
        }
    }
    int error = 0;
    if (ca_mp4_queue_init(&writer->queue, queue_capacity()) < 0) {
        error = errno;
    } else if ((error = pthread_create(&writer->thread, NULL, write_frames, writer)) != 0) {
        ca_mp4_queue_destroy(&writer->queue);
    }
    if (error) {
        (void)file_close(writer->file);
        unlink(path);
        free(writer->path);
        delete writer;
        errno = error;
        return -1;
    }
    *result = writer;
    return 0;
}

int ca_mp4_write_h264(struct ca_mp4 *writer, const uint8_t *annex_b,
                      size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    (void)pts;
    (void)key_frame;
    if (!writer || !annex_b) { errno = EINVAL; return -1; }
    int error = atomic_load(&writer->error);
    if (error) { errno = error; return -1; }
    /* Bad input loses this frame, not the recording: every backend closes
     * a failed recording on its frame thread. */
    if (!length || length > INT_MAX / 2U) {
        ca_mp4_queue_drop(&writer->queue);
        return 0;
    }
    /* Telemetry belongs to the frame's arrival, not to when it is written. */
    struct ca_metadata metadata;
    struct timespec utc;
    ca_metadata_snapshot(&metadata);
    metadata.hfov_deg = hfov_deg;
    if (clock_gettime(CLOCK_REALTIME, &utc) < 0) {
        ca_mp4_queue_drop(&writer->queue);
        return 0;
    }
    /* Real IDRs, not the caller's flag, end a run of dropped frames. */
    if (ca_mp4_queue_push(&writer->queue, annex_b, length, access_unit_kind(annex_b, length),
                          &metadata, &utc) < 0) return -1;
    return 0;
}

int ca_mp4_close(struct ca_mp4 *writer)
{
    if (!writer) return 0;
    ca_mp4_queue_close(&writer->queue);
    pthread_join(writer->thread, NULL);
    int error = writer_error(writer);
    destroy_writer(writer);
    if (error) { errno = error; return -1; }
    return 0;
}

void ca_mp4_close_async(struct ca_mp4 *writer)
{
    if (!writer) return;
    writer->detached = true;
    (void)pthread_detach(writer->thread);
    /* the writer thread may free the writer once the queue is closed */
    ca_mp4_queue_close(&writer->queue);
}

void ca_mp4_get_stats(struct ca_mp4 *writer, struct ca_mp4_stats *stats)
{
    ca_mp4_queue_stats(&writer->queue, stats, NULL);
}

static int stream_write_at(int64_t offset, const void *buffer, size_t size,
                           void *opaque)
{
    struct ca_fmp4 *writer = (struct ca_fmp4*)(opaque);

    if (writer->failed || offset != writer->next_offset ||
        size > (size_t)(INT64_MAX - writer->next_offset) ||
        writer->write(writer->opaque, buffer, size) != 0) {
        writer->failed = true;
        return 1;
    }
    writer->next_offset += (int64_t)size;
    return 0;
}

int ca_fmp4_open(struct ca_fmp4 **result, unsigned width, unsigned height,
                 unsigned frame_rate, ca_fmp4_write_fn write, void *opaque)
{
    struct ca_fmp4 *writer;

    if (result == NULL || width == 0U || height == 0U || frame_rate == 0U ||
        write == NULL) {
        errno = EINVAL;
        return -1;
    }
    writer = new (std::nothrow) ca_fmp4{};
    if (writer == NULL) return -1;
    writer->frame_duration = 90000U / frame_rate;
    writer->write = write;
    writer->opaque = opaque;
    writer->mux = MP4E_open(1, 1, writer, stream_write_at);
    if (writer->mux == NULL || writer->failed ||
        mp4_h26x_write_init(&writer->h264, writer->mux,
                            (int)width, (int)height, 0) != MP4E_STATUS_OK) {
        int saved_errno = errno != 0 ? errno : EIO;
        if (writer->mux != NULL) (void)MP4E_close(writer->mux);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    *result = writer;
    return 0;
}

int ca_fmp4_write_h264(struct ca_fmp4 *writer, const uint8_t *annex_b,
                       size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    (void)pts;
    (void)key_frame;
    if (writer == NULL || writer->failed) {
        errno = writer != NULL && writer->failed ? EPIPE : EINVAL;
        return -1;
    }
    char json[CA_VIDEO_METADATA_JSON_MAX];
    size_t json_length;
    struct ca_metadata metadata;
    struct timespec utc;
    ca_metadata_snapshot(&metadata);
    metadata.hfov_deg = hfov_deg;
    if (clock_gettime(CLOCK_REALTIME, &utc) < 0) return -1;
    int result = write_h264(&writer->h264, writer->frame_duration, annex_b, length,
                            writer->next_pts90k, json, &json_length, &metadata, &utc);
    if (result > 0) writer->next_pts90k += writer->frame_duration;
    return result < 0 ? -1 : 0;
}

int ca_fmp4_close(struct ca_fmp4 *writer)
{
    int result = 0;

    if (writer == NULL) return 0;
    mp4_h26x_write_close(&writer->h264);
    if (MP4E_close(writer->mux) != MP4E_STATUS_OK || writer->failed) {
        result = -1;
        errno = EIO;
    }
    delete writer;
    return result;
}
