#ifndef CAMERA_APP_HISI_ISP_H
#define CAMERA_APP_HISI_ISP_H

#include "camera_app/config.h"
#include "camera_app/exposure.h"
#include "camera_app/media.h"
#include "ot_type.h"

/* startup: the first apply after the ISP starts, with camera.ini's settings.
 * Iris priority that can't be applied (no DC iris, or the ISP refuses it)
 * then falls back to the normal exposure, as do later applies while
 * camera.ini still asks for it; setting it live fails instead. */
td_s32 ca_hisi_apply_isp_config(const struct ca_config *config, bool startup);
int ca_hisi_exposure(unsigned lens, struct ca_exposure *sample);
/* White balance for one-push; the caller holds the ISP lock. */
td_s32 ca_hisi_apply_white_balance(const struct ca_config *config);
/* the gains in use, in 1/256; green is Gr */
td_s32 ca_hisi_white_balance_gains(td_u16 *red, td_u16 *green, td_u16 *blue);
/* manual gains, green on Gr and Gb, or auto keeping its calibration */
td_s32 ca_hisi_set_white_balance_gains(bool manual, td_u16 red, td_u16 green, td_u16 blue);
/* auto DC iris on PWM0 (iris.h sets up the PWM); only once the ISP runs */
td_s32 ca_hisi_enable_dc_iris(void);
/* Whether the board's DC iris is set up, so applies may use it (iris
 * priority) and keep it in auto; before the first apply. */
void ca_hisi_set_dc_iris(bool available);

#endif
