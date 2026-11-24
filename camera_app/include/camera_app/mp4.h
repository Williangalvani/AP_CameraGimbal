#ifndef CAMERA_APP_MP4_H
#define CAMERA_APP_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ca_mp4;
struct ca_fmp4;

typedef int (*ca_fmp4_write_fn)(void *opaque, const void *data,
                                size_t length);

/* Fragmented MP4 with per-frame telemetry SEI. Complete fragments are playable
 * before close; background data syncs limit buffered loss on a crash.
 * FAT/vfat recordings roll on an IDR after 4 GiB minus 64 MiB, with a hard
 * ceiling of 4 GiB minus one byte. Other filesystems (including exFAT) do not
 * roll. Continuations use <stem>_part0002.<extension>, preserve decoder headers,
 * and restart the sample/telemetry timeline at zero.
 * A writer thread does all file I/O after open, from a bounded RAM buffer.
 * Its errors fail a later write and close; the file is complete by then.
 * Close drains the buffer, waits for the last sync of every part and reports
 * asynchronous I/O errors. Calls on one writer must not overlap: one thread
 * at a time writes, then closes. */
int ca_mp4_open(struct ca_mp4 **writer, const char *path,
                unsigned width, unsigned height, unsigned frame_rate);
/* One complete Annex-B access unit per call, including all slices/SEI.
 * Playback uses the configured constant frame rate (pts is encoder-specific).
 * Copies the frame and its telemetry and never waits for the card. A frame
 * that does not fit in the buffer is dropped with every following picture
 * until an IDR that fits while a quarter of the buffer is free. Drops, and
 * frames that are empty or oversized, are counted and logged and return 0;
 * only a NULL writer or frame (EINVAL) or a writer error returns -1.
 * Dropped frames take no time in the file: playback runs straight through a
 * gap, so the MP4 duration is shorter than the wall-clock recording time.
 * Each frame's SEI keeps the UTC time it arrived. */
int ca_mp4_write_h264(struct ca_mp4 *writer, const uint8_t *annex_b,
                      size_t length, uint64_t pts, bool key_frame, float hfov_deg);
int ca_mp4_close(struct ca_mp4 *writer);
/* Close without waiting, for a frame thread that must not wait for the card:
 * the writer thread finishes the file in the background, logs any error and
 * frees the writer. The writer must not be used afterwards. */
void ca_mp4_close_async(struct ca_mp4 *writer);

struct ca_mp4_stats {
    /* muxed by the writer thread; not necessarily on the card yet */
    uint64_t frames_written;
    uint64_t frames_dropped;
    /* runs of consecutive dropped frames */
    uint64_t drop_events;
    /* including the frame being written */
    size_t buffer_bytes;
    size_t buffer_high_water;
    size_t buffer_capacity;
};
/* Counters since open. */
void ca_mp4_get_stats(struct ca_mp4 *writer, struct ca_mp4_stats *stats);

/* Sequential fragmented MP4 for non-seekable transports.  The callback is
 * invoked synchronously and must consume the complete buffer before returning.
 */
int ca_fmp4_open(struct ca_fmp4 **writer, unsigned width, unsigned height,
                 unsigned frame_rate, ca_fmp4_write_fn write, void *opaque);
int ca_fmp4_write_h264(struct ca_fmp4 *writer, const uint8_t *annex_b,
                       size_t length, uint64_t pts, bool key_frame, float hfov_deg);
int ca_fmp4_close(struct ca_fmp4 *writer);

#endif
