/*
 * Second-process MPP probe, run while camera-app owns the pipeline. Never
 * calls ss_mpi_sys_init or creates, starts or stops anything; takes no VENC
 * stream packets. The only write sets the exposure attribute to the value
 * just read. A VPSS frame is held for as long as it takes to sample it.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ss_mpi_ae.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_mem.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_vpss.h"

#define PIPE 0
#define VPSS_GRP 0
#define VPSS_MAIN_CHN 0

static int report(const char *what, td_s32 ret)
{
    printf("%-34s %s (0x%08x)\n", what, ret == TD_SUCCESS ? "ok" : "FAILED", (unsigned)ret);
    return ret == TD_SUCCESS;
}

static void venc(ot_venc_chn chn)
{
    char what[64];
    ot_venc_chn_attr attr;
    ot_venc_chn_status status;
    memset(&attr, 0, sizeof(attr));
    memset(&status, 0, sizeof(status));
    snprintf(what, sizeof(what), "venc %d get_chn_attr", chn);
    if (report(what, ss_mpi_venc_get_chn_attr(chn, &attr)))
        printf("    type %d %ux%u\n", attr.venc_attr.type, attr.venc_attr.pic_width,
               attr.venc_attr.pic_height);
    snprintf(what, sizeof(what), "venc %d query_status", chn);
    if (report(what, ss_mpi_venc_query_status(chn, &status)))
        printf("    left_stream_frames %u cur_packs %u\n", status.left_stream_frames,
               status.cur_packs);
}

int main(void)
{
    static ot_isp_exp_info info;
    static ot_isp_ae_stats stats;
    ot_mpp_version version;
    ot_isp_pub_attr pub;
    ot_isp_exposure_attr exposure;
    ot_video_frame_info frame;

    printf("pid %d\n", (int)getpid());
    memset(&version, 0, sizeof(version));
    if (report("sys get_version", ss_mpi_sys_get_version(&version)))
        printf("    %s\n", version.version);

    venc(0);
    venc(1);

    memset(&pub, 0, sizeof(pub));
    if (report("isp get_pub_attr", ss_mpi_isp_get_pub_attr(PIPE, &pub)))
        printf("    %ux%u at %.1f fps\n", pub.wnd_rect.width, pub.wnd_rect.height,
               (double)pub.frame_rate);
    if (report("isp query_exposure_info", ss_mpi_isp_query_exposure_info(PIPE, &info)))
        printf("    exp_time %u us, iso %u, ave_lum %u, fps %u\n", info.exp_time, info.iso,
               info.ave_lum, info.fps);
    if (report("isp get_ae_stats", ss_mpi_isp_get_ae_stats(PIPE, &stats)))
        printf("    be_global_avg %u %u %u %u\n", stats.be_global_avg[0],
               stats.be_global_avg[1], stats.be_global_avg[2], stats.be_global_avg[3]);
    memset(&exposure, 0, sizeof(exposure));
    if (report("isp get_exposure_attr", ss_mpi_isp_get_exposure_attr(PIPE, &exposure)))
        report("isp set_exposure_attr (unchanged)", ss_mpi_isp_set_exposure_attr(PIPE, &exposure));

    memset(&frame, 0, sizeof(frame));
    if (report("vpss get_chn_frame (main)",
               ss_mpi_vpss_get_chn_frame(VPSS_GRP, VPSS_MAIN_CHN, &frame, 500))) {
        const ot_video_frame *v = &frame.video_frame;
        td_u32 size = v->stride[0] * 64;
        printf("    %ux%u stride %u pixel_format %d compress %d phys 0x%llx\n", v->width,
               v->height, v->stride[0], v->pixel_format, v->compress_mode,
               (unsigned long long)v->phys_addr[0]);
        const td_u8 *luma = ss_mpi_sys_mmap(v->phys_addr[0], size);
        if (luma) {
            unsigned long sum = 0;
            for (td_u32 i = 0; i < size; i += 97) sum += luma[i];
            printf("    mapped %u bytes, sampled mean %lu\n", size, sum / (size / 97 + 1));
            ss_mpi_sys_munmap(luma, size);
        } else {
            printf("    ss_mpi_sys_mmap FAILED\n");
        }
        report("vpss release_chn_frame", ss_mpi_vpss_release_chn_frame(VPSS_GRP, VPSS_MAIN_CHN, &frame));
    }
    return 0;
}
