/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __EXYNOS_CP_CAL_IF_H
#define __EXYNOS_CP_CAL_IF_H
int cal_cp_init(void);
int cal_cp_status(void);
int cal_cp_reset_assert(void);
int cal_cp_reset_release(void);
int cal_cp_active_clear(void);
int cal_cp_reset_req_clear(void);
#endif
