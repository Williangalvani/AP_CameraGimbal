#ifndef CAMERA_APP_MP4_QUEUE_H
#define CAMERA_APP_MP4_QUEUE_H

/* The recorder's RAM buffer between the encoder threads and its writer
 * thread. Internal to the MP4 writer; declared for its tests.
 * One producer and one consumer: push, drop and close must not run
 * concurrently with each other (each recording has one frame source, which
 * also closes it), and only the writer thread pops and releases. */

#include "camera_app/metadata.h"
#include "camera_app/mp4.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* 48 MiB holds ~7.7 s at 50 Mbit/s while the card stalls. */
#ifndef CA_MP4_QUEUE_BYTES
#define CA_MP4_QUEUE_BYTES (48U * 1024U * 1024U)
#endif
#define CA_MP4_QUEUE_MIN_BYTES (64U * 1024U)
#define CA_MP4_QUEUE_MAX_BYTES (256U * 1024U * 1024U)
/* The writer collects muxer output and writes it in pieces of this size. */
#define CA_MP4_WRITE_BYTES (2U * 1024U * 1024U)

/* What an access unit carries, as far as dropping is concerned. */
enum ca_mp4_unit {
    CA_MP4_UNIT_HEADERS, /* parameter sets or SEI, no picture */
    CA_MP4_UNIT_PICTURE, /* a picture that may reference earlier ones */
    CA_MP4_UNIT_IDR,     /* an H.264 IDR or H.265 IRAP picture */
};

/* One access unit and the telemetry captured when it was queued. */
struct ca_mp4_frame {
    struct ca_mp4_frame *next;
    uint8_t *data;
    size_t length;
    bool idr;
    struct ca_metadata metadata;
    struct timespec utc;
};

struct ca_mp4_queue {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct ca_mp4_frame *head, *tail;
    size_t capacity, bytes, high_water, file_high_water;
    uint64_t frames_written, frames_dropped, drop_events;
    bool dropping;
    bool closed;
};

int ca_mp4_queue_init(struct ca_mp4_queue *queue, size_t capacity);
/* Frees any frames still queued. */
void ca_mp4_queue_destroy(struct ca_mp4_queue *queue);
/* Bytes a queued access unit of this length counts against the capacity. */
size_t ca_mp4_queue_cost(size_t length);
/* Copies the access unit and never waits for the writer. Returns 1 when
 * queued; 0 when dropped: a picture that does not fit, then every picture
 * until an IDR that fits while at least a quarter of the capacity is free.
 * Units without a picture are queued whenever they fit and neither start
 * nor end a run of drops. -1 after close. */
int ca_mp4_queue_push(struct ca_mp4_queue *queue, const uint8_t *data, size_t length,
                      enum ca_mp4_unit unit, const struct ca_metadata *metadata,
                      const struct timespec *utc);
/* Counts a frame the producer could not queue; pictures until the next IDR
 * that may resume are dropped too. */
void ca_mp4_queue_drop(struct ca_mp4_queue *queue);
/* Returns 1 with the oldest frame, 0 after timeout_ms, or -1 once closed and
 * empty. The frame counts against the capacity until released. */
int ca_mp4_queue_pop(struct ca_mp4_queue *queue, unsigned timeout_ms,
                     struct ca_mp4_frame **frame);
void ca_mp4_queue_release(struct ca_mp4_queue *queue, struct ca_mp4_frame *frame,
                          bool written);
void ca_mp4_queue_close(struct ca_mp4_queue *queue);
/* With file_high_water, also the peak since the previous such call. */
void ca_mp4_queue_stats(struct ca_mp4_queue *queue, struct ca_mp4_stats *stats,
                        size_t *file_high_water);

#endif
