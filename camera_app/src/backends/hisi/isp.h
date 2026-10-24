#ifndef CAMERA_APP_HISI_ISP_H
#define CAMERA_APP_HISI_ISP_H

#include "camera_app/config.h"
#include "camera_app/exposure.h"
#include "ot_type.h"

td_s32 ca_hisi_apply_isp_config(const struct ca_config *config, bool live);
int ca_hisi_exposure(unsigned lens, struct ca_exposure *sample);

#endif
