#ifndef CA_HISI_SDK_COMPAT_H
#define CA_HISI_SDK_COMPAT_H

/* Forced into every file built against the B040 headers. The V2.0.2.0
 * sample_comm sources use pipe PSFM and sensor objects that B040 does not
 * have; PSFM is never enabled here, and only the IMX678 is linked. */
#include "ot_common_vi.h"
#include "ot_sns_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    td_bool psfm_en;
} ot_vi_psfm_attr;

static inline td_s32 ss_mpi_vi_set_pipe_psfm_attr(ot_vi_pipe vi_pipe,
                                                  const ot_vi_psfm_attr *attr)
{
    (void)vi_pipe;
    (void)attr;
    return TD_FAILURE;
}

extern ot_isp_sns_obj g_sns_imx335_obj;
extern ot_isp_sns_obj g_sns_imx662_obj;
extern ot_isp_sns_obj g_sns_imx662_2l_obj;
extern ot_isp_sns_obj g_sns_imx678_obj;
extern ot_isp_sns_obj g_sns_imx678_2l_obj;
extern ot_isp_sns_obj g_sns_sc233hgs_obj;
extern ot_isp_sns_obj g_sns_sc233hgs_2l_obj;
extern ot_isp_sns_obj g_sns_sc4336p_obj;
extern ot_isp_sns_obj g_sns_sc450ai_2l_obj;

#ifdef __cplusplus
}
#endif

#endif
