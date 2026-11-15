/*
 * Changes a running camera-app encoder channel from a second process, for
 * RAM-only experiments. Prints the settings before and after each change.
 * Usage: venc_tune CHN show | gop FRAMES | qpdelta N | refresh off |
 *        refresh on ROWS [I_QP] | idr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ss_mpi_venc.h"

static void show(ot_venc_chn chn)
{
    ot_venc_chn_attr attr;
    ot_venc_intra_refresh refresh;
    memset(&attr, 0, sizeof(attr));
    memset(&refresh, 0, sizeof(refresh));
    if (ss_mpi_venc_get_chn_attr(chn, &attr) != TD_SUCCESS) {
        printf("chn %d: get_chn_attr failed\n", chn);
        return;
    }
    td_u32 gop = attr.rc_attr.rc_mode == OT_VENC_RC_MODE_H265_CBR ? attr.rc_attr.h265_cbr.gop
                                                                   : attr.rc_attr.h264_cbr.gop;
    printf("chn %d: rc_mode %d gop %u gop_mode %d ip_qp_delta %d", chn, attr.rc_attr.rc_mode, gop,
           attr.gop_attr.gop_mode, attr.gop_attr.normal_p.ip_qp_delta);
    if (ss_mpi_venc_get_intra_refresh(chn, &refresh) == TD_SUCCESS)
        printf(" refresh %d mode %d rows %u i_qp %u", refresh.enable, refresh.mode,
               refresh.refresh_num, refresh.request_i_qp);
    putchar('\n');
}

static int check(const char *what, td_s32 ret)
{
    printf("%s: %s (0x%08x)\n", what, ret == TD_SUCCESS ? "ok" : "FAILED", (unsigned)ret);
    return ret == TD_SUCCESS ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s CHN show|gop N|qpdelta N|refresh off|refresh on ROWS [I_QP]|idr\n",
                argv[0]);
        return 2;
    }
    ot_venc_chn chn = atoi(argv[1]);
    const char *cmd = argv[2];
    int failed = 0;
    show(chn);
    if (!strcmp(cmd, "gop") || !strcmp(cmd, "qpdelta")) {
        ot_venc_chn_attr attr;
        if (argc < 4 || check("get_chn_attr", ss_mpi_venc_get_chn_attr(chn, &attr))) return 1;
        td_u32 value = (td_u32)atoi(argv[3]);
        if (!strcmp(cmd, "gop")) {
            if (attr.rc_attr.rc_mode == OT_VENC_RC_MODE_H265_CBR)
                attr.rc_attr.h265_cbr.gop = value;
            else
                attr.rc_attr.h264_cbr.gop = value;
        } else {
            attr.gop_attr.normal_p.ip_qp_delta = (td_s32)value;
        }
        failed = check("set_chn_attr", ss_mpi_venc_set_chn_attr(chn, &attr));
    } else if (!strcmp(cmd, "refresh")) {
        ot_venc_intra_refresh refresh;
        memset(&refresh, 0, sizeof(refresh));
        (void)ss_mpi_venc_get_intra_refresh(chn, &refresh);
        refresh.enable = argc > 3 && !strcmp(argv[3], "on") ? TD_TRUE : TD_FALSE;
        refresh.mode = OT_VENC_INTRA_REFRESH_ROW;
        if (refresh.enable) {
            if (argc < 5) return 2;
            refresh.refresh_num = (td_u32)atoi(argv[4]);
            refresh.request_i_qp = argc > 5 ? (td_u32)atoi(argv[5]) : refresh.request_i_qp;
        }
        failed = check("set_intra_refresh", ss_mpi_venc_set_intra_refresh(chn, &refresh));
    } else if (!strcmp(cmd, "idr")) {
        failed = check("request_idr", ss_mpi_venc_request_idr(chn, TD_TRUE));
    } else if (strcmp(cmd, "show")) {
        fprintf(stderr, "unknown command %s\n", cmd);
        return 2;
    }
    if (strcmp(cmd, "show")) show(chn);
    return failed;
}
