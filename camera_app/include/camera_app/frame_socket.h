#ifndef CAMERA_APP_FRAME_SOCKET_H
#define CAMERA_APP_FRAME_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apcam/config.h"

/* Encoded video for a local consumer (GStreamer's apcamsrc), one access unit
 * at a time and without RTP. A Unix stream socket; a name starting with '@'
 * is in the abstract namespace. All integers are little-endian.
 *
 * The client sends one hello:
 *   "APVR", u8 version (1), u8 stream (0 main, 1 sub), u16 zero
 * and the server answers:
 *   "APVS", u8 version (1), u8 status (0 ok), u8 codec (1 H.264, 2 H.265),
 *   u8 zero, u16 width, u16 height, u16 frame rate, u16 zero
 * then sends access units in Annex B, each after
 *   u32 length, u32 flags, u64 pts (encoder clock, microseconds)
 * The first is a key frame. A client that falls behind loses the queued
 * frames and resumes at the next key frame, flagged as a discontinuity.
 * The client may send 'K' at any time to ask for a key frame.
 * The connection closes when camera-app stops or the stream changes. */

#define CA_FRAME_SOCKET_NAME "@apcam/video"
#define CA_FRAME_SOCKET_VERSION 1U
#define CA_FRAME_SOCKET_HELLO_SIZE 8U
#define CA_FRAME_SOCKET_INFO_SIZE 16U
#define CA_FRAME_SOCKET_HEADER_SIZE 16U
#define CA_FRAME_KEY 1U
#define CA_FRAME_DISCONT 2U
#define CA_FRAME_SOCKET_STREAMS 2U

enum {
    CA_FRAME_SOCKET_OK = 0,
    CA_FRAME_SOCKET_NO_STREAM = 1,
    CA_FRAME_SOCKET_BUSY = 2,
};

struct ca_frame_socket;

/* At most every 250 ms per stream, from a client thread or the publisher. */
typedef void (*ca_frame_socket_keyframe_fn)(void *context, unsigned stream);

int ca_frame_socket_open(struct ca_frame_socket **server, const char *name,
                         ca_frame_socket_keyframe_fn keyframe, void *context);
int ca_frame_socket_configure(struct ca_frame_socket *server, unsigned stream,
                              enum ca_video_codec codec, unsigned width,
                              unsigned height, unsigned frame_rate);
/* Cheap when nobody is connected to the stream. */
void ca_frame_socket_publish(struct ca_frame_socket *server, unsigned stream,
                             const uint8_t *annex_b, size_t length, uint64_t pts,
                             bool key_frame);
void ca_frame_socket_close(struct ca_frame_socket *server);

#endif
