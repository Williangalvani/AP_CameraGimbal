#ifndef CAMERA_APP_MP4_H
#define CAMERA_APP_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_app/config.h"

struct ca_mp4;
struct ca_fmp4;

typedef int (*ca_fmp4_write_fn)(void *opaque, const void *data,
                                size_t length);

/* Fragmented MP4 with per-frame telemetry SEI. Complete fragments are playable
 * before close; background data syncs limit buffered loss on a crash.
 * FAT/vfat recordings roll on a key frame after 4 GiB minus 64 MiB, with a hard
 * ceiling of 4 GiB minus one byte. Other filesystems (including exFAT) do not
 * roll for size. On any filesystem, a parameter set that differs from the
 * part's sample entry (a new ID, or new content for an ID) rolls at the next
 * key frame, waiting for the previous part's close if need be, since samples
 * do not carry parameter sets; the change is logged. Continuations use
 * <stem>_part0002.<extension>, carry the latest parameter set of each ID, and
 * restart the sample/telemetry timeline at zero.
 * A writer thread does all file I/O after open, from a bounded RAM buffer.
 * Its errors fail a later write and close; the file is complete by then.
 * Close drains the buffer, waits for the last sync of every part and reports
 * asynchronous I/O errors. Calls on one writer must not overlap: one thread
 * at a time writes, then closes.
 * H.264 tracks are avc1/avcC; H.265 tracks are hvc1/hvcC with the VPS, SPS
 * and PPS. In both, samples carry only VCL and SEI NAL units, and a "key
 * frame" below is an H.264 IDR or an H.265 BLA or IDR (NAL types 16 to 20).
 * An H.265 CRA is not one: after a gap or at a part's start, the RASL
 * pictures that follow it would reference pictures the file lacks. */
int ca_mp4_open_codec(struct ca_mp4 **writer, const char *path,
                      enum ca_video_codec codec, unsigned width,
                      unsigned height, unsigned frame_rate);
/* H.264 */
int ca_mp4_open(struct ca_mp4 **writer, const char *path,
                unsigned width, unsigned height, unsigned frame_rate);
/* One complete Annex-B access unit per call, including all slices/SEI, in
 * the codec the writer was opened with.
 * Playback uses the configured constant frame rate (pts is encoder-specific).
 * Copies the frame and its telemetry and never waits for the card. A frame
 * that does not fit in the buffer is dropped with every following picture
 * until a key frame that fits while a quarter of the buffer is free. Drops,
 * and frames that are empty or oversized, are counted and logged and return
 * 0; only a NULL writer or frame (EINVAL) or a writer error returns -1.
 * Dropped frames take no time in the file: playback runs straight through a
 * gap, so the MP4 duration is shorter than the wall-clock recording time.
 * Each frame's SEI keeps the UTC time it arrived. */
int ca_mp4_write_video(struct ca_mp4 *writer, const uint8_t *annex_b,
                       size_t length, uint64_t pts, bool key_frame, float hfov_deg);
/* The same, for the H.264 writers ca_mp4_open returns. */
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
