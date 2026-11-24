#ifndef CAMERA_APP_VIDEO_METADATA_H
#define CAMERA_APP_VIDEO_METADATA_H

#include "camera_app/config.h"
#include "camera_app/metadata.h"

#define CA_VIDEO_METADATA_JSON_MAX 1024U
#define CA_VIDEO_METADATA_SEI_MAX 1600U

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t ca_video_metadata_uuid[16];

/* Bounded UTF-8 JSON, excluding the terminating NUL. pts90k identifies the
 * video sample (or RTP timestamp); UTC is the telemetry snapshot time. */
size_t ca_video_metadata_json(const struct ca_metadata *metadata,
                               const struct timespec *utc, uint64_t pts90k,
                               char *output, size_t capacity);
/* Annex-B user_data_unregistered SEI, including start code and escaping:
 *   00 00 00 01, then the NAL header:
 *     H.264: 06 (SEI, nal_ref_idc 0)
 *     H.265: 4E 0t (prefix SEI type 39, nuh_layer_id 0, nuh_temporal_id_plus1
 *            t: temporal_id_plus1 if 1 to 7, else 1). A prefix SEI's
 *            TemporalId must be its access unit's, so pass the first VCL
 *            NAL's; H.264 ignores it.
 *   then the same RBSP for both:
 *     05                        payloadType user_data_unregistered
 *     FF.. nn                   payloadSize: 16 + JSON length, in 255 steps
 *     ca_video_metadata_uuid    16 bytes
 *     JSON                      apcg.telemetry.v1, no terminating NUL
 *     80                        rbsp_trailing_bits
 *   with an emulation prevention 03 inserted after any 00 00 that precedes
 *   a byte up to 03. A reader skips one header byte (H.264) or two (H.265),
 *   removes the 03s and parses the payload identically. */
size_t ca_video_metadata_sei(enum ca_video_codec codec, unsigned temporal_id_plus1,
                              const char *json, size_t length,
                              uint8_t *output, size_t capacity);
/* Locate the next Annex-B NAL, excluding start code/trailing_zero_8bits. */
bool ca_annexb_next(const uint8_t *data, size_t length, size_t *offset,
                    size_t *nal, size_t *nal_length);
bool ca_video_nal_is_vcl(enum ca_video_codec codec, uint8_t header);
/* Insert immediately before the first VCL NAL of a single access unit.
 * Returns 1 and malloc-owned output, 0 for no VCL, or -1 on failure. */
int ca_video_metadata_insert(enum ca_video_codec codec,
                              const uint8_t *data, size_t length, uint64_t pts90k, float hfov_deg,
                              uint8_t **output, size_t *output_length);

#ifdef __cplusplus
}
#endif
#endif
