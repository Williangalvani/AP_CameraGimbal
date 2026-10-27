#include "sdk_compat.h"

/* sample_comm_isp.c can select every sensor the SDK knows, but only the IMX678
 * driver is built. ca_hisi_sensor_type() never selects one of these, whose
 * callbacks are all null. */
ot_isp_sns_obj g_sns_gst412c_obj;
ot_isp_sns_obj g_sns_imx335_obj;
ot_isp_sns_obj g_sns_imx347_slave_obj;
ot_isp_sns_obj g_sns_imx515_obj;
ot_isp_sns_obj g_sns_imx662_obj;
ot_isp_sns_obj g_sns_imx662_2l_obj;
ot_isp_sns_obj g_sns_imx678_2l_obj;
ot_isp_sns_obj g_sns_os04a10_obj;
ot_isp_sns_obj g_sns_os04a10_slave_obj;
ot_isp_sns_obj g_sns_os08a20_obj;
ot_isp_sns_obj g_sns_sc233hgs_obj;
ot_isp_sns_obj g_sns_sc233hgs_2l_obj;
ot_isp_sns_obj g_sns_sc4336p_obj;
ot_isp_sns_obj g_sns_sc450ai_obj;
ot_isp_sns_obj g_sns_sc450ai_2l_obj;
ot_isp_sns_obj g_sns_sc850sl_obj;
