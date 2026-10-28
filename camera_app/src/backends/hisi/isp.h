#ifndef CAMERA_APP_HISI_ISP_H
#define CAMERA_APP_HISI_ISP_H

#include "camera_app/config.h"
#include "camera_app/exposure.h"
#include "camera_app/media.h"
#include "ot_type.h"

td_s32 ca_hisi_apply_isp_config(const struct ca_config *config);
int ca_hisi_exposure(unsigned lens, struct ca_exposure *sample);
int ca_hisi_measure_white_balance(enum ca_wb_water water, struct ca_wb_gains *gains);

#endif
