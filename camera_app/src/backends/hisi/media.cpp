/* HiSilicon Hi3516DV500 media backend for AP_CameraGimbal.
 *
 * One IMX678 through MIPI-RX -> VI/ISP -> VPSS -> VENC on the Hi3519DV500 MPP
 * SDK (same "ot" ss_mpi family as src/backends/mt11, which this is ported
 * from). VPSS channel 0 feeds the main RTSP stream, channel 1 the sub stream.
 * Recording reuses the main encoder when it already produces H.264 at the
 * recording size; otherwise channel 2 feeds a dedicated H.264 encoder that
 * runs only while recording, so two 4K encodes never run unnecessarily.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "isp.h"
#include "pipeline.h"

#include "camera_app/APC_Media_Backend.h"
#include "camera_app/live_video_server.h"
#include "camera_app/log.h"
#include "camera_app/mp4.h"
#include "camera_app/overlay.h"
#include "camera_app/rtsp.h"
#include "camera_app/still.h"

#include "ss_mpi_sys.h"
#include "ss_mpi_vpss.h"

#include "apcam/atomic.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CA_HISI_MAIN_VENC 0U
#define CA_HISI_SUB_VENC 1U
#define CA_HISI_RECORD_VENC 2U
#define CA_HISI_VENC_COUNT 3U
#define CA_HISI_STILL_VENC 3
#define CA_HISI_LIVE_VIDEO_PORT 8555U

class APC_Media_HiSi;
struct APC_Media_HiSi_State {
    struct ca_media_config config;
    struct ca_hisi_output_sizes sizes;
    struct ca_overlay_hw *overlay;
    sample_vi_cfg vi_cfg;
    ot_venc_chn_attr venc_attr[CA_HISI_VENC_COUNT];
    // 2, or 3 when recording needs its own encoder
    unsigned venc_count;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_mutex_t venc_lock;
    pthread_mutex_t photo_lock;
    td_bool sys_started;
    td_bool vi_started;
    td_bool vi_vpss_bound;
    td_bool vpss_started;
    td_bool venc_created[CA_HISI_VENC_COUNT];
    td_bool venc_bound[CA_HISI_VENC_COUNT];
    td_bool venc_started[CA_HISI_VENC_COUNT];
    bool record_path_active;
    bool thread_started;
    atomic_bool stop;
    atomic_bool recording;
    atomic_bool inverted;
    bool recording_wait_keyframe;
    struct ca_rtsp *rtsp;
    unsigned sub_rtsp_stream;
    struct ca_live_video_server *live_video;
    struct ca_mp4 *mp4;
    char recording_path[PATH_MAX];
    atomic_bool encoded_frame_logged[CA_HISI_VENC_COUNT];
};

// Driver state is private to this translation unit. The owner joins the
// capture thread before releasing SDK resources.
class APC_Media_HiSi final : public APC_Media_Backend {
public:
    ~APC_Media_HiSi() override { shutdown(); }
    static std::unique_ptr<APC_Media_Backend> create(const ca_media_config &config)
    {
        auto *driver = new (std::nothrow) APC_Media_HiSi();
        if (!driver) { errno = ENOMEM; return nullptr; }
        if (driver->init(&config) < 0) {
            const int saved = errno;
            delete driver;
            errno = saved;
            return nullptr;
        }
        return std::unique_ptr<APC_Media_Backend>(driver);
    }
    bool ready() const override { return _state != nullptr; }
    int set_recording(bool active) override;
    bool recording() const override { return _state && atomic_load(&_state->recording); }
    const char *recording_path() const override { return _state ? _state->recording_path : ""; }
    int set_zoom(float zoom) override { return zoom == 1.0f ? 0 : unsupported(); }   // fixed lens
    float zoom() const override { return 1.0f; }
    float hfov(bool thermal) const override { return thermal ? NAN : (float)APCAM_LENS1_FOV_H; }
    unsigned frame_rate(bool thermal) const override {
        return thermal ? 0U : _state ? _state->config.frame_rate : (unsigned)APCAM_FRAME_RATE;
    }
    int set_lens(enum ca_media_lens lens) override { return lens == CA_MEDIA_LENS_WIDE ? 0 : unsupported(); }
    enum ca_media_lens lens() const override { return CA_MEDIA_LENS_WIDE; }
    int set_thermal_main(bool thermal_main) override { return thermal_main ? unsupported() : 0; }
    bool thermal_main() const override { return false; }
    int autofocus(uint16_t, uint16_t) override { return unsupported(); }   // fixed focus
    int manual_focus(int) override { return unsupported(); }
    int set_focus_percent(float) override { return unsupported(); }
    bool thermal_range(struct ca_thermal_range *) override { return false; }
    int capture_photo(enum ca_photo_scope scope) override;
    int get_thermal_gain(uint8_t *) override { return unsupported(); }
    int set_thermal_gain(uint8_t) override { return unsupported(); }
    int get_thermal_palette(uint8_t *) override { return unsupported(); }
    int set_thermal_palette(uint8_t) override { return unsupported(); }
    int set_inverted(bool inverted) override;
    int exposure(unsigned lens, struct ca_exposure *sample) override;
    int apply_overlay(const struct ca_config *settings) override;
    int apply_image(const struct ca_config *settings) override;
private:
    APC_Media_HiSi() = default;
    static int unsupported() { errno = ENOTSUP; return -1; }
    int init(const ca_media_config *config);
    void shutdown();
    APC_Media_HiSi_State *_state = nullptr;
};

static unsigned record_venc(const struct APC_Media_HiSi_State *media)
{
    return media->venc_count > CA_HISI_RECORD_VENC ? CA_HISI_RECORD_VENC
                                                   : CA_HISI_MAIN_VENC;
}

static bool video_key_frame(enum ca_video_codec codec, const uint8_t *data,
                            size_t length)
{
    for (size_t i = 0; i + 4U < length; i++) {
        size_t start = 0;
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) {
            start = i + 3U;
        } else if (data[i] == 0U && data[i + 1U] == 0U &&
                   data[i + 2U] == 0U && data[i + 3U] == 1U) {
            start = i + 4U;
        }
        if (start != 0U && start < length) {
            unsigned type = codec == CA_VIDEO_H265
                                ? (data[start] >> 1U) & 0x3fU
                                : data[start] & 0x1fU;
            if ((codec == CA_VIDEO_H264 && type == 5U) ||
                (codec == CA_VIDEO_H265 && type >= 19U && type <= 21U)) {
                return true;
            }
        }
    }
    return false;
}

static td_s32 set_record_path_locked(struct APC_Media_HiSi_State *media,
                                     bool active)
{
    const ot_venc_start_param start = {.recv_pic_num = -1};
    td_s32 result = TD_SUCCESS;

    if (record_venc(media) != CA_HISI_RECORD_VENC) return TD_SUCCESS;
    if (active == media->record_path_active) return TD_SUCCESS;
    pthread_mutex_lock(&media->venc_lock);
    if (active) {
        result = ss_mpi_vpss_enable_chn(CA_HISI_GROUP, CA_HISI_RECORD_CHN);
        if (result == TD_SUCCESS) result = ss_mpi_venc_start_chn(CA_HISI_RECORD_VENC, &start);
        if (result == TD_SUCCESS) media->venc_started[CA_HISI_RECORD_VENC] = TD_TRUE;
    }
    if (!active || result != TD_SUCCESS) {
        if (media->venc_started[CA_HISI_RECORD_VENC]) {
            (void)ss_mpi_venc_stop_chn(CA_HISI_RECORD_VENC);
            // drop queued streams so the next recording starts clean
            (void)ss_mpi_venc_reset_chn(CA_HISI_RECORD_VENC);
            media->venc_started[CA_HISI_RECORD_VENC] = TD_FALSE;
        }
        (void)ss_mpi_vpss_disable_chn(CA_HISI_GROUP, CA_HISI_RECORD_CHN);
    }
    media->record_path_active = active && result == TD_SUCCESS;
    pthread_mutex_unlock(&media->venc_lock);
    if (result != TD_SUCCESS) {
        ca_log("record path %s failed: 0x%x", active ? "start" : "stop",
               (unsigned)result);
    }
    return result;
}

static void write_recording(struct APC_Media_HiSi_State *media, const uint8_t *data,
                            size_t length, uint64_t pts, bool key_frame)
{
    pthread_mutex_lock(&media->lock);
    if (atomic_load(&media->recording) && media->recording_wait_keyframe && key_frame) {
        media->recording_wait_keyframe = false;
    }
    if (atomic_load(&media->recording) && !media->recording_wait_keyframe &&
        ca_mp4_write_h264(media->mp4, data, length, pts, key_frame,
                          (float)APCAM_LENS1_FOV_H) < 0) {
        ca_log("recording stopped after MP4 write failure: %s", strerror(errno));
        (void)ca_mp4_close(media->mp4);
        media->mp4 = NULL;
        atomic_store(&media->recording, false);
        (void)set_record_path_locked(media, false);
    }
    pthread_mutex_unlock(&media->lock);
}

static void consume_frame(struct APC_Media_HiSi_State *media, unsigned channel,
                          uint8_t *data, size_t length, uint64_t pts)
{
    const struct ca_config *settings = &media->config.settings;
    enum ca_video_codec codec = channel == CA_HISI_MAIN_VENC ? settings->main_codec
                              : channel == CA_HISI_SUB_VENC ? settings->sub_codec
                              : CA_VIDEO_H264;
    bool key_frame = video_key_frame(codec, data, length);
    const float hfov_deg = (float)APCAM_LENS1_FOV_H;
    if (!atomic_exchange(&media->encoded_frame_logged[channel], true)) {
        ca_log("VENC channel %u first encoded frame bytes=%zu key=%u", channel,
               length, key_frame ? 1U : 0U);
    }
    if (channel <= CA_HISI_SUB_VENC) {
        unsigned stream = channel == CA_HISI_MAIN_VENC ? 0U : media->sub_rtsp_stream;
        (void)ca_rtsp_push_video(media->rtsp, stream, data, length, key_frame, hfov_deg);
        (void)ca_live_video_server_publish(media->live_video, channel, data,
                                            length, pts, key_frame, hfov_deg);
    }
    if (channel == record_venc(media) && atomic_load(&media->recording)) {
        write_recording(media, data, length, pts, key_frame);
    }
}

static void *capture_thread(void *opaque)
{
    struct APC_Media_HiSi_State *media = (struct APC_Media_HiSi_State *)(opaque);
    struct pollfd waits[CA_HISI_VENC_COUNT];
    const unsigned count = media->venc_count;
    bool blocking = true;
    for (unsigned channel = 0; channel < count; channel++) {
        waits[channel] = {ss_mpi_venc_get_fd(channel), POLLIN, 0};
        blocking = blocking && waits[channel].fd >= 0;
    }
    if (!blocking) ca_log("VENC fds unavailable; polling encoders");
    while (!atomic_load(&media->stop)) {
        bool consumed = false;
        // sleep until an encoder has a stream rather than polling at 200 Hz
        if (blocking && poll(waits, count, 100) <= 0) continue;
        for (unsigned channel = 0; channel < count; channel++) {
            ot_venc_chn_status status = {};
            ot_venc_stream stream = {};
            uint8_t *frame = NULL;
            size_t frame_length = 0;
            uint64_t pts = 0;
            pthread_mutex_lock(&media->venc_lock);
            td_s32 ret = ss_mpi_venc_query_status(channel, &status);
            if (ret != TD_SUCCESS || status.cur_packs == 0U) {
                pthread_mutex_unlock(&media->venc_lock);
                continue;
            }
            if (status.cur_packs > 128U) {
                ca_log("VENC channel %u returned invalid pack count=%u",
                       channel, status.cur_packs);
                pthread_mutex_unlock(&media->venc_lock);
                continue;
            }
            stream.pack = (ot_venc_pack *)(calloc(status.cur_packs, sizeof(*stream.pack)));
            if (stream.pack == NULL) {
                pthread_mutex_unlock(&media->venc_lock);
                continue;
            }
            stream.pack_cnt = status.cur_packs;
            ret = ss_mpi_venc_get_stream(channel, &stream, 100);
            if (ret == TD_SUCCESS) {
                size_t total = 0;
                for (td_u32 i = 0; i < stream.pack_cnt; i++) {
                    if (stream.pack[i].offset <= stream.pack[i].len) {
                        total += stream.pack[i].len - stream.pack[i].offset;
                        pts = stream.pack[i].pts;
                    }
                }
                frame = (uint8_t *)(total != 0U ? malloc(total) : NULL);
                if (frame != NULL) {
                    size_t offset = 0;
                    for (td_u32 i = 0; i < stream.pack_cnt; i++) {
                        ot_venc_pack *pack = &stream.pack[i];
                        if (pack->offset > pack->len) continue;
                        size_t bytes = pack->len - pack->offset;
                        memcpy(frame + offset, pack->addr + pack->offset, bytes);
                        offset += bytes;
                    }
                    frame_length = offset;
                }
                (void)ss_mpi_venc_release_stream(channel, &stream);
                consumed = true;
            }
            free(stream.pack);
            pthread_mutex_unlock(&media->venc_lock);
            if (frame != NULL) {
                consume_frame(media, channel, frame, frame_length, pts);
                free(frame);
            }
        }
        if (!consumed && !blocking) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000};
            (void)nanosleep(&delay, NULL);
        }
    }
    return NULL;
}

static int make_recording_path(struct APC_Media_HiSi_State *media)
{
    time_t now = time(NULL);
    struct tm local;
    char directory[PATH_MAX];
    char day[16];

    localtime_r(&now, &local);
    strftime(day, sizeof(day), "%Y-%m-%d", &local);
    if (mkdir(media->config.record_root, 0700) < 0 && errno != EEXIST) return -1;
    if (snprintf(directory, sizeof(directory), "%s/%s",
                 media->config.record_root, day) >= (int)sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    for (unsigned suffix = 0; suffix < 1000U; suffix++) {
        int length = snprintf(media->recording_path, sizeof(media->recording_path),
                              "%s/%s_%lld_A%s.mp4", directory, day, (long long)now,
                              suffix == 0U ? "" : "_next");
        if (length < 0 || (size_t)length >= sizeof(media->recording_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (access(media->recording_path, F_OK) < 0 && errno == ENOENT) return 0;
        now++;
    }
    errno = EEXIST;
    return -1;
}

static void media_cleanup(struct APC_Media_HiSi_State *media, APC_Media_HiSi *owner)
{
    ca_overlay_hw_close(media->overlay);
    media->overlay = NULL;
    if (media->thread_started) {
        atomic_store(&media->stop, true);
        pthread_join(media->thread, NULL);
        media->thread_started = false;
    }
    ca_live_video_server_close(media->live_video);
    media->live_video = NULL;
    (void)owner->set_recording(false);
    ca_rtsp_close(media->rtsp);
    media->rtsp = NULL;
    for (int channel = (int)CA_HISI_VENC_COUNT - 1; channel >= 0; channel--) {
        if (media->venc_started[channel]) (void)ss_mpi_venc_stop_chn(channel);
    }
    static const ot_vpss_chn sources[CA_HISI_VENC_COUNT] = {
        CA_HISI_MAIN_CHN, CA_HISI_SUB_CHN, CA_HISI_RECORD_CHN
    };
    for (int channel = (int)CA_HISI_VENC_COUNT - 1; channel >= 0; channel--) {
        if (media->venc_bound[channel]) {
            (void)sample_comm_vpss_un_bind_venc(CA_HISI_GROUP, sources[channel], channel);
        }
    }
    for (int channel = (int)CA_HISI_VENC_COUNT - 1; channel >= 0; channel--) {
        if (media->venc_created[channel]) (void)ss_mpi_venc_destroy_chn(channel);
    }
    if (media->vi_vpss_bound) {
        (void)sample_comm_vi_un_bind_vpss(CA_HISI_PIPE, 0, CA_HISI_GROUP, 0);
    }
    if (media->vpss_started) ca_hisi_vpss_stop(&media->sizes);
    if (media->vi_started) sample_comm_vi_stop_vi(&media->vi_cfg);
    if (media->sys_started) sample_comm_sys_exit();
}

static unsigned stream_bitrate(unsigned width, unsigned height)
{
    if (width >= 3840U || height >= 2160U) return 12000U;
    if (width >= 1920U || height >= 1080U) return 4096U;
    return 2048U;
}

// 0 selects the automatic rate for the main stream's resolution
static unsigned main_bitrate(const struct APC_Media_HiSi_State *media,
                             const struct ca_config *settings)
{
    return settings->main_bitrate_kbps != 0U
               ? settings->main_bitrate_kbps
               : stream_bitrate(media->sizes.width[CA_HISI_MAIN_CHN],
                                media->sizes.height[CA_HISI_MAIN_CHN]);
}

int APC_Media_HiSi::init(const struct ca_media_config *config)
{
    struct APC_Media_HiSi_State *media;
    struct ca_hisi_output_sizes *sizes;
    const struct ca_config *settings;
    ot_venc_start_param start = {.recv_pic_num = -1};
    const char *stage = "argument validation";
    // frame_rate is the configured main stream rate; the sensor runs at APCAM_FRAME_RATE
    if (config == NULL || config->capture_root == NULL ||
        config->record_root == NULL || config->frame_rate < APCAM_MAIN_FPS_MIN ||
        config->frame_rate > APCAM_FRAME_RATE) {
        errno = EINVAL;
        return -1;
    }
    media = new (std::nothrow) APC_Media_HiSi_State{};
    if (media == NULL) { errno = ENOMEM; return -1; }
    _state = media;
    media->config = *config;
    settings = &media->config.settings;
    sizes = &media->sizes;
    ca_video_resolution_size(settings->main_resolution,
                             &sizes->width[CA_HISI_MAIN_CHN], &sizes->height[CA_HISI_MAIN_CHN]);
    ca_video_resolution_size(settings->sub_resolution,
                             &sizes->width[CA_HISI_SUB_CHN], &sizes->height[CA_HISI_SUB_CHN]);
    ca_video_resolution_size(settings->recording_resolution,
                             &sizes->width[CA_HISI_RECORD_CHN], &sizes->height[CA_HISI_RECORD_CHN]);
    // the MP4 writer only muxes H.264
    sizes->record_channel = settings->main_codec != CA_VIDEO_H264 ||
        sizes->width[CA_HISI_RECORD_CHN] != sizes->width[CA_HISI_MAIN_CHN] ||
        sizes->height[CA_HISI_RECORD_CHN] != sizes->height[CA_HISI_MAIN_CHN];
    media->venc_count = sizes->record_channel ? 3U : 2U;
    pthread_mutex_init(&media->lock, NULL);
    pthread_mutex_init(&media->venc_lock, NULL);
    pthread_mutex_init(&media->photo_lock, NULL);
    ca_hisi_configure_vi(&media->vi_cfg);
    stage = "SYS/VB initialization";
    if (ca_hisi_system_init(sizes) != TD_SUCCESS) goto fail;
    media->sys_started = TD_TRUE;
    stage = "MIPI/VI/ISP startup";
    if (sample_comm_vi_start_vi(&media->vi_cfg) != TD_SUCCESS) goto fail;
    media->vi_started = TD_TRUE;
    stage = "VPSS startup";
    if (ca_hisi_vpss_start(sizes) != TD_SUCCESS) goto fail;
    media->vpss_started = TD_TRUE;
    stage = "VI-to-VPSS binding";
    if (sample_comm_vi_bind_vpss(CA_HISI_PIPE, 0, CA_HISI_GROUP, 0) != TD_SUCCESS) goto fail;
    media->vi_vpss_bound = TD_TRUE;
    stage = "ISP configuration";
    if (ca_hisi_apply_isp_config(settings, false) != TD_SUCCESS) goto fail;
    ca_hisi_video_attributes(&media->venc_attr[CA_HISI_MAIN_VENC], settings->main_codec,
                             sizes->width[CA_HISI_MAIN_CHN], sizes->height[CA_HISI_MAIN_CHN],
                             config->frame_rate, main_bitrate(media, settings));
    ca_hisi_video_attributes(&media->venc_attr[CA_HISI_SUB_VENC], settings->sub_codec,
                             sizes->width[CA_HISI_SUB_CHN], sizes->height[CA_HISI_SUB_CHN],
                             APCAM_FRAME_RATE,
                             stream_bitrate(sizes->width[CA_HISI_SUB_CHN],
                                            sizes->height[CA_HISI_SUB_CHN]));
    ca_hisi_video_attributes(&media->venc_attr[CA_HISI_RECORD_VENC], CA_VIDEO_H264,
                             sizes->width[CA_HISI_RECORD_CHN], sizes->height[CA_HISI_RECORD_CHN],
                             APCAM_FRAME_RATE,
                             stream_bitrate(sizes->width[CA_HISI_RECORD_CHN],
                                            sizes->height[CA_HISI_RECORD_CHN]));
    stage = "VENC creation";
    for (unsigned channel = 0; channel < media->venc_count; channel++) {
        if (ss_mpi_venc_create_chn(channel, &media->venc_attr[channel]) != TD_SUCCESS) goto fail;
        media->venc_created[channel] = TD_TRUE;
    }
    stage = "VPSS-to-VENC binding";
    for (unsigned channel = 0; channel < media->venc_count; channel++) {
        // VENC channel n encodes VPSS channel n
        if (sample_comm_vpss_bind_venc(CA_HISI_GROUP, channel, channel) != TD_SUCCESS) goto fail;
        media->venc_bound[channel] = TD_TRUE;
    }
    stage = "VENC startup";
    for (unsigned channel = CA_HISI_MAIN_VENC; channel <= CA_HISI_SUB_VENC; channel++) {
        if (ss_mpi_venc_start_chn(channel, &start) != TD_SUCCESS) goto fail;
        media->venc_started[channel] = TD_TRUE;
    }
    // a dedicated record output is idle until recording starts
    if (sizes->record_channel &&
        ss_mpi_vpss_disable_chn(CA_HISI_GROUP, CA_HISI_RECORD_CHN) != TD_SUCCESS) goto fail;
    stage = "RTSP startup";
    if (ca_rtsp_open(&media->rtsp, config->rtsp_port, "video1",
                     settings->main_codec, config->frame_rate) < 0) goto fail;
    if (ca_rtsp_add_video(media->rtsp, "video2", settings->sub_codec,
                          APCAM_FRAME_RATE, &media->sub_rtsp_stream) < 0) goto fail;
    ca_rtsp_add_config_aliases(media->rtsp, settings);
    if (ca_rtsp_support_proxy(media->rtsp, &settings->support) < 0)
        ca_log("SupportProxy video startup failed: %s", strerror(errno));
    stage = "native live-video startup";
    if (ca_live_video_server_open(&media->live_video, CA_HISI_LIVE_VIDEO_PORT) < 0 ||
        ca_live_video_server_configure(
            media->live_video, 0U, sizes->width[CA_HISI_MAIN_CHN],
            sizes->height[CA_HISI_MAIN_CHN], config->frame_rate,
            settings->main_codec == CA_VIDEO_H264) < 0 ||
        ca_live_video_server_configure(
            media->live_video, 1U, sizes->width[CA_HISI_SUB_CHN],
            sizes->height[CA_HISI_SUB_CHN], APCAM_FRAME_RATE,
            settings->sub_codec == CA_VIDEO_H264) < 0) goto fail;
    if (settings->orientation == CA_MOUNT_INVERTED && set_inverted(true) < 0) {
        stage = "inverted output configuration";
        goto fail;
    }
    stage = "capture-thread startup";
    if (pthread_create(&media->thread, NULL, capture_thread, media) != 0) goto fail;
    media->thread_started = true;
    (void)ss_mpi_venc_request_idr(CA_HISI_MAIN_VENC, TD_TRUE);
    (void)ss_mpi_venc_request_idr(CA_HISI_SUB_VENC, TD_TRUE);
    ca_log("media ready: /video1 %ux%u/%u %s %u kbps; /video2 %ux%u/%u %s; "
           "recording %ux%u H.264 (%s encoder); RTSP port %u",
           sizes->width[CA_HISI_MAIN_CHN], sizes->height[CA_HISI_MAIN_CHN],
           config->frame_rate, ca_video_codec_name(settings->main_codec),
           main_bitrate(media, settings),
           sizes->width[CA_HISI_SUB_CHN], sizes->height[CA_HISI_SUB_CHN],
           APCAM_FRAME_RATE, ca_video_codec_name(settings->sub_codec),
           sizes->width[CA_HISI_RECORD_CHN], sizes->height[CA_HISI_RECORD_CHN],
           sizes->record_channel ? "dedicated" : "main", config->rtsp_port);
    return 0;
fail:
    ca_log("media startup failed at %s", stage);
    shutdown();
    errno = EIO;
    return -1;
}

void APC_Media_HiSi::shutdown()
{
    auto *media = _state;
    if (media == NULL) return;
    media_cleanup(media, this);
    pthread_mutex_destroy(&media->photo_lock);
    pthread_mutex_destroy(&media->venc_lock);
    pthread_mutex_destroy(&media->lock);
    _state = nullptr;
    delete media;
}

int APC_Media_HiSi::set_recording(bool active)
{
    auto *media = _state;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->lock);
    if (active == atomic_load(&media->recording)) {
        pthread_mutex_unlock(&media->lock);
        return 0;
    }
    if (active) {
        const struct ca_hisi_output_sizes *sizes = &media->sizes;
        const unsigned frame_rate = record_venc(media) == CA_HISI_MAIN_VENC
                                        ? media->config.frame_rate : APCAM_FRAME_RATE;
        if (make_recording_path(media) < 0 ||
            ca_mp4_open(&media->mp4, media->recording_path,
                        sizes->width[CA_HISI_RECORD_CHN],
                        sizes->height[CA_HISI_RECORD_CHN], frame_rate) < 0) {
            pthread_mutex_unlock(&media->lock);
            return -1;
        }
        if (set_record_path_locked(media, true) != TD_SUCCESS) {
            (void)ca_mp4_close(media->mp4);
            media->mp4 = NULL;
            (void)unlink(media->recording_path);
            pthread_mutex_unlock(&media->lock);
            errno = EIO;
            return -1;
        }
        atomic_store(&media->recording, true);
        media->recording_wait_keyframe = true;
        (void)ss_mpi_venc_request_idr(record_venc(media), TD_TRUE);
        ca_log("recording started %s", media->recording_path);
        pthread_mutex_unlock(&media->lock);
        return 0;
    }
    struct ca_mp4 *mp4 = media->mp4;
    char path[PATH_MAX];
    media->mp4 = NULL;
    atomic_store(&media->recording, false);
    (void)set_record_path_locked(media, false);
    media->recording_wait_keyframe = false;
    memcpy(path, media->recording_path, sizeof(path));
    pthread_mutex_unlock(&media->lock);
    // closing syncs the file; the capture thread must not wait on it for the lock
    int result = ca_mp4_close(mp4) < 0 ? -1 : 0;
    ca_log("recording stopped %s", path);
    return result;
}

static void jpeg_attributes(ot_venc_chn_attr *attr, td_u32 width, td_u32 height)
{
    memset(attr, 0, sizeof(*attr));
    attr->venc_attr.type = OT_PT_JPEG;
    attr->venc_attr.max_pic_width = width;
    attr->venc_attr.max_pic_height = height;
    attr->venc_attr.pic_width = width;
    attr->venc_attr.pic_height = height;
    attr->venc_attr.buf_size = OT_ALIGN_UP(width, 16U) * OT_ALIGN_UP(height, 16U) * 4U;
    attr->venc_attr.is_by_frame = TD_TRUE;
    attr->venc_attr.jpeg_attr.dcf_en = TD_FALSE;
    attr->venc_attr.jpeg_attr.mpf_cfg.large_thumbnail_num = 0;
    attr->venc_attr.jpeg_attr.recv_mode = OT_VENC_PIC_RECV_SINGLE;
    attr->gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
}

static int capture_jpeg(struct APC_Media_HiSi_State *media)
{
    const td_u32 width = media->sizes.width[CA_HISI_MAIN_CHN];
    const td_u32 height = media->sizes.height[CA_HISI_MAIN_CHN];
    ot_venc_chn_attr attr;
    ot_venc_start_param start = {.recv_pic_num = 1};
    ot_video_frame_info frame = {};
    ot_venc_chn_status status = {};
    ot_venc_stream stream = {};
    struct timespec captured_at;
    char path[PATH_MAX];
    unsigned char *jpeg = NULL;
    size_t jpeg_length = 0;
    bool created = false;
    bool started = false;
    bool frame_acquired = false;
    bool stream_acquired = false;
    const char *stage = "VENC creation";
    td_s32 code;
    int result = -1;

    jpeg_attributes(&attr, width, height);
    code = ss_mpi_venc_create_chn(CA_HISI_STILL_VENC, &attr);
    if (code != TD_SUCCESS) goto mpi_fail;
    created = true;
    stage = "VENC startup";
    code = ss_mpi_venc_start_chn(CA_HISI_STILL_VENC, &start);
    if (code != TD_SUCCESS) goto mpi_fail;
    started = true;
    stage = "VPSS frame acquisition";
    code = ss_mpi_vpss_get_chn_frame(CA_HISI_GROUP, CA_HISI_MAIN_CHN, &frame, 1000);
    if (code != TD_SUCCESS) goto mpi_fail;
    frame_acquired = true;
    if (clock_gettime(CLOCK_REALTIME, &captured_at) < 0) goto done;
    stage = "JPEG frame submission";
    code = ss_mpi_venc_send_frame(CA_HISI_STILL_VENC, &frame, 1000);
    if (code != TD_SUCCESS) goto mpi_fail;
    (void)ss_mpi_vpss_release_chn_frame(CA_HISI_GROUP, CA_HISI_MAIN_CHN, &frame);
    frame_acquired = false;

    stage = "JPEG completion";
    for (unsigned attempt = 0; attempt < 50U; attempt++) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        code = ss_mpi_venc_query_status(CA_HISI_STILL_VENC, &status);
        if (code != TD_SUCCESS) goto mpi_fail;
        if (status.cur_packs != 0U) break;
        (void)nanosleep(&delay, NULL);
    }
    if (status.cur_packs == 0U || status.cur_packs > 128U) {
        code = TD_FAILURE;
        goto mpi_fail;
    }
    stream.pack = (ot_venc_pack *)(calloc(status.cur_packs, sizeof(*stream.pack)));
    if (stream.pack == NULL) goto done;
    stream.pack_cnt = status.cur_packs;
    stage = "JPEG stream retrieval";
    code = ss_mpi_venc_get_stream(CA_HISI_STILL_VENC, &stream, 1000);
    if (code != TD_SUCCESS) goto mpi_fail;
    stream_acquired = true;
    for (td_u32 i = 0; i < stream.pack_cnt; i++) {
        const ot_venc_pack *pack = &stream.pack[i];
        if (pack->offset > pack->len ||
            jpeg_length > SIZE_MAX - (pack->len - pack->offset)) {
            errno = EOVERFLOW;
            goto done;
        }
        jpeg_length += pack->len - pack->offset;
    }
    jpeg = (unsigned char *)(malloc(jpeg_length));
    if (jpeg == NULL) goto done;
    for (td_u32 i = 0, offset = 0; i < stream.pack_cnt; i++) {
        const ot_venc_pack *pack = &stream.pack[i];
        td_u32 bytes = pack->len - pack->offset;
        memcpy(jpeg + offset, pack->addr + pack->offset, bytes);
        offset += bytes;
    }
    if (ca_still_write_jpeg(media->config.capture_root, 'C', jpeg, jpeg_length,
                            &captured_at, path, sizeof(path)) < 0) {
        ca_log("C JPEG write failed: %s", strerror(errno));
        goto done;
    }
    ca_log("C JPEG capture saved: %s (%zu bytes)", path, jpeg_length);
    result = 0;
    goto done;

mpi_fail:
    ca_log("C JPEG capture failed at %s: 0x%x", stage, (unsigned)code);
    errno = EIO;
done:
    {
        int saved_errno = errno;
        free(jpeg);
        if (stream_acquired) (void)ss_mpi_venc_release_stream(CA_HISI_STILL_VENC, &stream);
        free(stream.pack);
        if (frame_acquired) {
            (void)ss_mpi_vpss_release_chn_frame(CA_HISI_GROUP, CA_HISI_MAIN_CHN, &frame);
        }
        if (started) (void)ss_mpi_venc_stop_chn(CA_HISI_STILL_VENC);
        if (created) (void)ss_mpi_venc_destroy_chn(CA_HISI_STILL_VENC);
        errno = saved_errno;
    }
    return result;
}

int APC_Media_HiSi::capture_photo(enum ca_photo_scope scope)
{
    auto *media = _state;
    if (media == NULL || (scope != CA_PHOTO_SCOPE_THERMAL &&
                          scope != CA_PHOTO_SCOPE_ALL)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&media->photo_lock);
    int result = capture_jpeg(media);
    pthread_mutex_unlock(&media->photo_lock);
    return result;
}

int APC_Media_HiSi::set_inverted(bool inverted)
{
    auto *media = _state;
    td_s32 result;
    if (media == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (atomic_load(&media->inverted) == inverted) return 0;
    pthread_mutex_lock(&media->lock);
    result = ca_hisi_set_inverted(inverted ? TD_TRUE : TD_FALSE,
                                  media->record_path_active, &media->sizes);
    pthread_mutex_unlock(&media->lock);
    if (result != TD_SUCCESS) {
        ca_log("output orientation change failed: 0x%x", (unsigned)result);
        errno = EIO;
        return -1;
    }
    atomic_store(&media->inverted, inverted);
    ca_log("output orientation=%s", inverted ? "inverted" : "upright");
    return 0;
}

int APC_Media_HiSi::apply_image(const struct ca_config *settings)
{
    auto *media = _state;
    pthread_mutex_lock(&media->lock);
    int result = ca_hisi_apply_isp_config(settings, true);
    const unsigned bit_rate = main_bitrate(media, settings);
    if (result == 0 && bit_rate != main_bitrate(media, &media->config.settings)) {
        result = ca_hisi_set_bitrate(CA_HISI_MAIN_VENC, bit_rate);
        if (result == 0) ca_log("main stream bitrate %u kbps", bit_rate);
        else ca_log("main stream bitrate change failed: 0x%x", (unsigned)result);
    }
    if (result == 0) ca_config_copy_image(&media->config.settings, settings);
    pthread_mutex_unlock(&media->lock);
    if (result != 0) { errno = EIO; return -1; }
    return 0;
}

int APC_Media_HiSi::exposure(unsigned lens, struct ca_exposure *s)
{
    auto *media = _state;
    pthread_mutex_lock(&media->lock);
    int result = ca_hisi_exposure(lens, s);
    pthread_mutex_unlock(&media->lock);
    return result;
}

int APC_Media_HiSi::apply_overlay(const struct ca_config *settings)
{
    auto *media = _state;
    struct ca_overlay_channel channels[CA_HISI_VENC_COUNT] = {};
    pthread_mutex_lock(&media->lock);
    media->config.settings.osd_cross = settings->osd_cross;
    media->config.settings.osd_recording = settings->osd_recording;
    pthread_mutex_lock(&media->venc_lock);
    for (unsigned c = 0; c < media->venc_count; c++) {
        channels[c] = (struct ca_overlay_channel){
            .width = media->venc_attr[c].venc_attr.pic_width,
            .height = media->venc_attr[c].venc_attr.pic_height,
            .cross = settings->osd_cross &&
                     (c <= CA_HISI_SUB_VENC || settings->osd_recording),
            .hfov = (float)APCAM_LENS1_FOV_H};
    }
    int result = ca_overlay_hw_set(&media->overlay, channels, media->venc_count);
    pthread_mutex_unlock(&media->venc_lock);
    pthread_mutex_unlock(&media->lock);
    return result;
}

std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config)
{
    return APC_Media_HiSi::create(config);
}
