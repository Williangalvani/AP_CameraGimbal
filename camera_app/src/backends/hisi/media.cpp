/* HiSilicon Hi3516DV500 media backend for AP_CameraGimbal.
 *
 * Image pipeline only (gimbal is a separate, optional backend). Drives the
 * HiSilicon "ot" MPP pipeline: MIPI-RX -> VI -> ISP -> VPSS -> VENC, the same
 * ss_mpi SDK as src/backends/mt11 (ss928v100 = Hi3519DV500). The real
 * capture/encode is ported from mt11's pipeline.cpp/isp.cpp (single sensor,
 * no thermal) against the Hi3516DV500 SDK placed at HISI_MPP_ROOT; see README.md.
 *
 * SCAFFOLD STATE: this is the backend entry point. It implements the full
 * APC_Media_Backend surface so the app links and the target is selectable, but
 * the MPP pipeline is not wired yet (ready() == false). Each hardware hook is
 * marked TODO(hisi). Bringing it up: init sensor (IMX678) + MIPI + VI + ISP
 * (AE/AWB thread) + VPSS (main/sub) + VENC (H.265 main / H.264 sub / JPEG snap),
 * then a consume() worker feeds EncodedFrames to the RTSP/live/mp4 sinks exactly
 * as rpi_libcam and mt11 do.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apcam/target.h"
#include "camera_app/APC_Media_Backend.h"
#include "camera_app/log.h"
#include <errno.h>
#include <math.h>
#include <memory>
#include <new>
#include <string.h>

namespace {

class APC_Media_HiSi final : public APC_Media_Backend {
public:
    ~APC_Media_HiSi() override = default;

    static std::unique_ptr<APC_Media_Backend> create(const ca_media_config &config)
    {
        auto self = std::unique_ptr<APC_Media_HiSi>(new (std::nothrow) APC_Media_HiSi());
        if (!self) { errno = ENOMEM; return nullptr; }
        self->config = config;
        // TODO(hisi): bring up the MPP pipeline here (see README). Until then the
        // backend links and advertises itself but reports not-ready (no video).
        ca_log("hisi: media backend created (MPP pipeline not yet implemented)");
        return self;
    }

    bool ready() const override { return active; }

    int set_recording(bool active_) override { (void)active_; return unsupported(); }
    bool recording() const override { return false; }
    const char *recording_path() const override { return nullptr; }

    int set_zoom(float) override { return unsupported(); }   // fixed lens
    float zoom() const override { return 1.0f; }

    float hfov(bool thermal) const override { return thermal ? NAN : (float)APCAM_LENS1_FOV_H; }
    unsigned frame_rate(bool thermal) const override { return thermal ? 0u : (unsigned)APCAM_FRAME_RATE; }

    int set_lens(enum ca_media_lens lens_) override { return lens_ == CA_MEDIA_LENS_WIDE ? 0 : unsupported(); }
    enum ca_media_lens lens() const override { return CA_MEDIA_LENS_WIDE; }

    int set_thermal_main(bool) override { return unsupported(); }
    bool thermal_main() const override { return false; }

    int autofocus(uint16_t, uint16_t) override { return unsupported(); }   // fixed focus
    int manual_focus(int) override { return unsupported(); }
    int set_focus_percent(float) override { return unsupported(); }
    bool thermal_range(struct ca_thermal_range *) override { return false; }

    // TODO(hisi): JPEG from the dedicated VENC snapshot channel.
    int capture_photo(enum ca_photo_scope) override { return unsupported(); }

    int get_thermal_gain(uint8_t *) override { return unsupported(); }
    int set_thermal_gain(uint8_t) override { return unsupported(); }
    int get_thermal_palette(uint8_t *) override { return unsupported(); }
    int set_thermal_palette(uint8_t) override { return unsupported(); }
    int set_inverted(bool) override { return unsupported(); }

    // TODO(hisi): read live AE state via ot_mpi_isp_query_exposure_info.
    int exposure(unsigned, struct ca_exposure *) override { return unsupported(); }

    // TODO(hisi): OSD region (ot_mpi_rgn) and ISP image controls.
    int apply_overlay(const struct ca_config *) override { return 0; }
    int apply_image(const struct ca_config *) override { return 0; }

private:
    APC_Media_HiSi() = default;
    static int unsupported() { errno = ENOTSUP; return -1; }

    struct ca_media_config config{};
    bool active = false;   // set true once the MPP pipeline is streaming
};

} // namespace

std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config)
{
    return APC_Media_HiSi::create(config);
}
