#ifndef CAMERA_APP_RTSP_H
#define CAMERA_APP_RTSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera_app/config.h"

struct ca_rtsp;

#ifdef __cplusplus
extern "C" {
#endif

int ca_rtsp_open(struct ca_rtsp **server, unsigned port, const char *path,
                 enum ca_video_codec codec, unsigned frame_rate);
int ca_rtsp_add_video(struct ca_rtsp *server, const char *path,
                      enum ca_video_codec codec, unsigned frame_rate,
                      unsigned *stream_id);
/* Serve an existing stream under an additional RTSP path such as the
 * vendor's main.264. An empty alias is ignored. */
int ca_rtsp_add_alias(struct ca_rtsp *server, unsigned stream_id, const char *alias);
/* Add the configured [stream.main]/[stream.sub] aliases, logging failures. */
void ca_rtsp_add_config_aliases(struct ca_rtsp *server, const struct ca_config *settings);
int ca_rtsp_push_h264(struct ca_rtsp *server, const uint8_t *data,
                      size_t length, bool key_frame, float hfov_deg);
int ca_rtsp_push_h264_stream(struct ca_rtsp *server, unsigned stream_id,
                             const uint8_t *data, size_t length,
                             bool key_frame, float hfov_deg);
int ca_rtsp_push_video(struct ca_rtsp *server, unsigned stream_id,
                       const uint8_t *data, size_t length, bool key_frame, float hfov_deg);
/* Part of an access unit, such as the slices an encoder has finished, sent at
 * once. All parts of an access unit share its RTP timestamp and only the last
 * packet of the part with frame_end carries the RTP marker; each NAL unit is
 * its own RTP payload (single NAL or fragments). The first part must hold the
 * parameter sets and the first slice, and its key_frame marks the unit; a
 * client that starts playing mid-unit waits for the next key frame. The
 * metadata SEI goes before the first slice, SupportProxy gets the whole unit
 * at its end, and an empty part with frame_end only closes an open unit
 * (without a marker). ca_rtsp_push_video is a whole unit. */
int ca_rtsp_push_video_part(struct ca_rtsp *server, unsigned stream_id,
                            const uint8_t *data, size_t length, bool key_frame, bool frame_end,
                            float hfov_deg);
/* For sources with variable cadence (for example thermal throttling), preserve
 * their presentation clock instead of stepping the nominal frame rate. */
int ca_rtsp_push_video_timed(struct ca_rtsp *server, unsigned stream_id,
                             const uint8_t *data, size_t length, bool key_frame,
                             float hfov_deg, uint64_t pts_us);
int ca_rtsp_set_frame_rate(struct ca_rtsp *server, unsigned stream_id,
                           unsigned frame_rate);
int ca_rtsp_support_proxy(struct ca_rtsp *server, const struct ca_support_config *config);
/* Send each RTSP client of a stream (aliases included) its RTP at no more than
 * bits_per_second, with a burst of 16 packets after a gap and up to 30 ms at the
 * rate kept across late timer wakeups, so a key frame is spread over a few
 * frame intervals instead of leaving at line rate. 0, the default,
 * sends as fast as the socket accepts. Applies to UDP and TCP clients within a
 * packet of the change; RTCP and RTSP replies are never held back. */
int ca_rtsp_set_pacing(struct ca_rtsp *server, unsigned stream_id, uint64_t bits_per_second);
/* Called on the RTSP event thread when a client starts playing a stream,
 * sends an RTCP PLI or FIR for it, or fell behind and is waiting for a key
 * frame; at most every 500 ms per stream. */
typedef void (*ca_rtsp_keyframe_fn)(void *context, unsigned stream_id);
void ca_rtsp_set_keyframe_callback(struct ca_rtsp *server, ca_rtsp_keyframe_fn request,
                                   void *context);
/* A watchdog thread checks that the RTSP event loop keeps running. If it is
 * stuck for stall_ms (default 5000) the watchdog logs it and calls action
 * once, on the watchdog thread. The default action shuts camera-app down
 * with SIGTERM, so its supervisor restarts it, and exits if that hangs.
 * A null action restores the default. */
void ca_rtsp_set_watchdog(struct ca_rtsp *server, unsigned stall_ms,
                          void (*action)(void *context), void *context);
/* Tests only: block the event loop for ms milliseconds. */
void ca_rtsp_test_stall_loop(struct ca_rtsp *server, unsigned ms);
void ca_rtsp_close(struct ca_rtsp *server);

#ifdef __cplusplus
}
#endif

#endif
