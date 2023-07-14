// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/llcc-qcom.h>

#define ACTIVATE                      BIT(0)
#define DEACTIVATE                    BIT(1)
#define ACT_CTRL_OPCODE_ACTIVATE      BIT(0)
#define ACT_CTRL_OPCODE_DEACTIVATE    BIT(1)
#define ACT_CTRL_ACT_TRIG             BIT(0)
#define ACT_CTRL_OPCODE_SHIFT         0x01
#define ATTR1_PROBE_TARGET_WAYS_SHIFT 0x02
#define ATTR1_FIXED_SIZE_SHIFT        0x03
#define ATTR1_PRIORITY_SHIFT          0x04
#define ATTR1_MAX_CAP_SHIFT           0x10
#define ATTR1_MAX_CAP_SHIFT_v31       0x0E
#define ATTR0_RES_WAYS_MASK           GENMASK(15, 0)
#define ATTR0_BONUS_WAYS_MASK         GENMASK(31, 16)
#define ATTR0_BONUS_WAYS_SHIFT        0x10
#define LLCC_STATUS_READ_DELAY        100

#define CACHE_LINE_SIZE_SHIFT         6

#define LLCC_COMMON_STATUS0_V2        0x0003000c
#define LLCC_COMMON_STATUS0_V21       0x0003400c
#define LLCC_COMMON_STATUS0           llcc_regs[LLCC_COMMON_STATUS0_num]

#define MAX_CAP_TO_BYTES(n)           (n * SZ_1K)
#define LLCC_TRP_ACT_CTRLn(n)         (n * SZ_4K)
#define LLCC_TRP_STATUSn(n)           (4 + n * SZ_4K)
#define LLCC_TRP_ATTR0_CFGn(n)        (0x21000 + SZ_8 * n)
#define LLCC_TRP_ATTR1_CFGn(n)        (0x21004 + SZ_8 * n)
#define LLCC_TRP_ATTR2_CFGn(n)        (0x21100 + SZ_4 * n)

#define LLCC_TRP_C_AS_N               0x22890
#define LLCC_TRP_NC_AS_C              0x22894
#define LLCC_FEAC_C_AS_NC_V2          0x35030
#define LLCC_FEAC_C_AS_NC_V21         0x41030
#define LLCC_FEAC_C_AS_NC             llcc_regs[LLCC_FEAC_C_AS_NC_num]
#define LLCC_FEAC_NC_AS_C_V2          0x35034
#define LLCC_FEAC_NC_AS_C_V21         0x41034
#define LLCC_FEAC_NC_AS_C             llcc_regs[LLCC_FEAC_NC_AS_C_num]

#define LLCC_TRP_WRSC_EN              0x21F20
#define LLCC_TRP_WRSC_CACHEABLE_EN    0x21F2C
#define LLCC_TRP_PCB_ACT              0x21F04
#define LLCC_TRP_SCID_DIS_CAP_ALLOC   0x21F00

/**
 * llcc_slice_config - Data associated with the llcc slice
 * @usecase_id: Unique id for the client's use case
 * @slice_id: llcc slice id for each client
 * @max_cap: The maximum capacity of the cache slice provided in KB
 * @priority: Priority of the client used to select victim line for replacement
 * @fixed_size: Boolean indicating if the slice has a fixed capacity
 * @bonus_ways: Bonus ways are additional ways to be used for any slice,
 *		if client ends up using more than reserved cache ways. Bonus
 *		ways are allocated only if they are not reserved for some
 *		other client.
 * @res_ways: Reserved ways for the cache slice, the reserved ways cannot
 *		be used by any other client than the one its assigned to.
 * @cache_mode: Each slice operates as a cache, this controls the mode of the
 *             slice: normal or TCM(Tightly Coupled Memory)
 * @probe_target_ways: Determines what ways to probe for access hit. When
 *                    configured to 1 only bonus and reserved ways are probed.
 *                    When configured to 0 all ways in llcc are probed.
 * @dis_cap_alloc: Disable capacity based allocation for a client
 * @retain_on_pc: If this bit is set and client has maintained active vote
 *               then the ways assigned to this client are not flushed on power
 *               collapse.
 * @activate_on_init: Activate the slice immediately after it is programmed
 * @write_scid_en: Enables write cache support for a given scid.
 * @write_scid_cacheable_en: Enables write cache cacheable support for a
 *                          given scid.(Not supported on V2 or older hardware)
 */
struct llcc_slice_config {
	u32 usecase_id;
	u32 slice_id;
	u32 max_cap;
	u32 priority;
	bool fixed_size;
	u32 bonus_ways;
	u32 res_ways;
	u32 cache_mode;
	u32 probe_target_ways;
	bool dis_cap_alloc;
	bool retain_on_pc;
	bool activate_on_init;
	bool write_scid_en;
	bool write_scid_cacheable_en;
};

static u32 llcc_offsets_v2[] = {
	0x0,
	0x80000,
	0x100000,
	0x180000
};

static u32 llcc_offsets_v21[] = {
	0x0,
	0x400000,
	0x100000,
	0x500000
};

static u32 llcc_offsets_v21_diwali[] = {
	0x0,
	0x100000
};

static u32 llcc_offsets_v31[] = {
	0x0,
	0x100000,
};

enum {
	LLCC_COMMON_STATUS0_num = 0,
	LLCC_FEAC_C_AS_NC_num,
	LLCC_FEAC_NC_AS_C_num,
	LLCC_REGS_MAX,
};

static u32 llcc_regs_v2[LLCC_REGS_MAX] = {
	LLCC_COMMON_STATUS0_V2,
	LLCC_FEAC_C_AS_NC_V2,
	LLCC_FEAC_NC_AS_C_V2,
};

static u32 llcc_regs_v21[LLCC_REGS_MAX] = {
	LLCC_COMMON_STATUS0_V21,
	LLCC_FEAC_C_AS_NC_V21,
	LLCC_FEAC_NC_AS_C_V21,
};

static u32 *llcc_regs = llcc_regs_v2;

struct qcom_llcc_config {
	const struct llcc_slice_config *sct_data;
	int size;
};

static const struct llcc_slice_config sc7180_data[] =  {
	{ LLCC_CPUSS,    1,  256, 1, 0, 0xf, 0x0, 0, 0, 0, 1, 1 },
	{ LLCC_MDM,      8,  128, 1, 0, 0xf, 0x0, 0, 0, 0, 1, 0 },
	{ LLCC_GPUHTW,   11, 128, 1, 0, 0xf, 0x0, 0, 0, 0, 1, 0 },
	{ LLCC_GPU,      12, 128, 1, 0, 0xf, 0x0, 0, 0, 0, 1, 0 },
};

static const struct llcc_slice_config sdm845_data[] =  {
	{ LLCC_CPUSS,    1,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 1 },
	{ LLCC_VIDSC0,   2,  512,  2, 1, 0x0,   0x0f0, 0, 0, 1, 1, 0 },
	{ LLCC_VIDSC1,   3,  512,  2, 1, 0x0,   0x0f0, 0, 0, 1, 1, 0 },
	{ LLCC_ROTATOR,  4,  563,  2, 1, 0x0,   0x00e, 2, 0, 1, 1, 0 },
	{ LLCC_VOICE,    5,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_AUDIO,    6,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_MDMHPGRW, 7,  1024, 2, 0, 0xfc,  0xf00, 0, 0, 1, 1, 0 },
	{ LLCC_MDM,      8,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_CMPT,     10, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_GPUHTW,   11, 512,  1, 1, 0xc,   0x0,   0, 0, 1, 1, 0 },
	{ LLCC_GPU,      12, 2304, 1, 0, 0xff0, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_MMUHWT,   13, 256,  2, 0, 0x0,   0x1,   0, 0, 1, 0, 1 },
	{ LLCC_CMPTDMA,  15, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_DISP,     16, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_VIDFW,    17, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_MDMHPFX,  20, 1024, 2, 1, 0x0,   0xf00, 0, 0, 1, 1, 0 },
	{ LLCC_MDMPNG,   21, 1024, 0, 1, 0x1e,  0x0,   0, 0, 1, 1, 0 },
	{ LLCC_AUDHW,    22, 1024, 1, 1, 0xffc, 0x2,   0, 0, 1, 1, 0 },
};

static const struct llcc_slice_config lahaina_data[] =  {
	{LLCC_CPUSS,     1, 3072, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 1, 0 },
	{LLCC_VIDSC0,    2,  512, 3, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_AUDIO,     6, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 0, 0, 0 },
	{LLCC_MDMHPGRW,  7, 1024, 3, 0, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDM,       8, 3072, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDMHW,     9, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_CMPT,     10, 3072, 1, 1, 0xFFF, 0x0,   0, 0, 0, 0, 0, 0 },
	{LLCC_GPUHTW,   11, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_GPU,      12, 1024, 1, 0, 0xFFF, 0x0,   0, 0, 0, 1, 0, 1 },
	{LLCC_MMUHWT,   13, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 0, 1, 0 },
	{LLCC_CMPTDMA,  15, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_DISP,     16, 3072, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDMPNG,   21, 1024, 0, 1, 0xF,   0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_AUDHW,    22, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_CVP,      28,  512, 3, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDMVPE,   29,  256, 1, 1, 0xF,   0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_APTCM,    30, 1024, 3, 1, 0x0,   0x1,   1, 0, 0, 1, 0, 0 },
	{LLCC_WRTCH,    31,  512, 1, 1, 0xFFF, 0x0,   0, 0, 0, 0, 1, 0 },
	{LLCC_CVPFW,    17,  512, 1, 0, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_CPUSS1,    3, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
};

static const struct llcc_slice_config shima_data[] =  {
	{LLCC_CPUSS,     1, 1536, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 1, 0 },
	{LLCC_AUDIO,     6, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDM,       8,  512, 2, 0, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_CMPT,     10, 1536, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_GPUHTW,   11,  256, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_GPU,      12, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 1 },
	{LLCC_MMUHWT,   13,  256, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 1, 0 },
	{LLCC_DISP,     16, 1536, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDMPNG,   21, 1536, 0, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_AUDHW,    22, 1024, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_MDMVPE,   29,  128, 1, 1, 0xFFF, 0x0,   0, 0, 0, 1, 0, 0 },
	{LLCC_WRTCH,    31,  256, 1, 1, 0xFFF, 0x0,   0, 0, 0, 0, 1, 0 },
};

static const struct llcc_slice_config neo_data[] =  {
	{LLCC_CPUSS,     1,  8192, 1, 0, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
	{LLCC_VIDSC0,    2,   192, 2, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AUDIO,     6,  3072, 3, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPUHTW,   11,     0, 1, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPU,      12,  1536, 2, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 1, 0 },
	{LLCC_MMUHWT,   13,  1024, 1, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_DISP,     16,     0, 2, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_WRTCH,    31,   256, 1, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_CAMEXP0,   4,  4096, 1, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_VIEYE,     7,  7168, 4, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_VIDPTH,    8,  7168, 4, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPUMV,     9,  1024, 2, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_EVALFT,   20,  7168, 5, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_EVARGHT,  21,  7168, 5, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_EVAGAIN,  25,  1024, 2, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AENPU,    30,  1024, 3, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_VIPTH,    29,  1024, 2, 1, 0xFFFFFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
};

static const struct llcc_slice_config waipio_data[] =  {
	{LLCC_CPUSS,     1, 3072, 1, 0, 0xFFFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
	{LLCC_VIDSC0,    2,  512, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AUDIO,     6, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 0, 0, 0 },
	{LLCC_MDMHPGRW,  7, 1024, 3, 0, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMHW,     9, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CMPT,     10, 4096, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPUHTW,   11,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPU,      12, 2048, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 1, 0 },
	{LLCC_MMUHWT,   13,  768, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_DISP,     16, 4096, 2, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMPNG,   21, 1024, 1, 1, 0xF000, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AUDHW,    22, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 0, 0, 0 },
	{LLCC_CVP,      28,  256, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMVPE,   29,   64, 1, 1, 0xF000, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_APTCM,    30, 1024, 3, 1, 0x0,    0xF0,  1, 0, 0, 1, 0, 0, 0 },
	{LLCC_WRTCH,    31,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_CVPFW,    17,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CPUSS1,    3, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CAMEXP0,   4,  256, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CPUMTE,   23,  256, 1, 1, 0x0FFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_CPUHWT,    5,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
	{LLCC_CAMEXP1,  27,  256, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AENPU,     8, 2048, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 0, 0, 0 },
};

static const struct llcc_slice_config cape_data[] =  {
	{LLCC_CPUSS,     1, 3072, 1, 0, 0xFFFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
	{LLCC_VIDSC0,    2,  512, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AUDIO,     6, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 0, 0, 0 },
	{LLCC_MDMHPGRW,  7, 1024, 3, 0, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMHW,     9, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CMPT,     10, 4096, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPUHTW,   11,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPU,      12, 2048, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 1, 0 },
	{LLCC_MMUHWT,   13,  768, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_DISP,     16, 4096, 2, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMPNG,   21, 1024, 0, 1, 0xF000, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AUDHW,    22, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 0, 0, 0 },
	{LLCC_CVP,      28,  256, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMVPE,   29,   64, 1, 1, 0xF000, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_APTCM,    30, 1024, 3, 1, 0x0,    0xF0,  1, 0, 0, 1, 0, 0, 0 },
	{LLCC_WRTCH,    31,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_CVPFW,    17,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CPUSS1,    3, 1024, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CAMEXP0,   4,  256, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_CPUMTE,   23,  256, 1, 1, 0x0FFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_CPUHWT,    5,  512, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
	{LLCC_CAMEXP1,  27,  256, 3, 1, 0xFFFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_AENPU,     8, 2048, 1, 1, 0xFFFF, 0x0,   0, 0, 0, 0, 0, 0, 0 },
};

static const struct llcc_slice_config diwali_data[] =  {
	{LLCC_CPUSS,     1, 1536, 0, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
	{LLCC_VIDSC0,    2,  128, 3, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMHPGRW,  7,  512, 3, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPUHTW,   11,  256, 1, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_GPU,      12,  512, 1, 0, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 1, 0 },
	{LLCC_MMUHWT,   13,  256, 3, 1, 0x0FFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_DISP,     16, 1536, 1, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMPNG,   21, 1024, 0, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_MDMVPE,   29,   64, 3, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 0, 0, 0 },
	{LLCC_WRTCH,    31,  256, 1, 1, 0x0FFF, 0x0,   0, 0, 0, 0, 1, 0, 0 },
	{LLCC_CPUMTE,   23,  256, 1, 1, 0x0FFF, 0x0,   0, 0, 0, 1, 1, 0, 0 },
};

static const struct qcom_llcc_config diwali_cfg = {
	.sct_data       = diwali_data,
	.size           = ARRAY_SIZE(diwali_data),
};

static const struct qcom_llcc_config sc7180_cfg = {
	.sct_data	= sc7180_data,
	.size		= ARRAY_SIZE(sc7180_data),
};

static const struct qcom_llcc_config sdm845_cfg = {
	.sct_data	= sdm845_data,
	.size		= ARRAY_SIZE(sdm845_data),
};

static const struct qcom_llcc_config lahaina_cfg = {
	.sct_data	= lahaina_data,
	.size		= ARRAY_SIZE(lahaina_data),
};

static const struct qcom_llcc_config shima_cfg = {
	.sct_data	= shima_data,
	.size		= ARRAY_SIZE(shima_data),
};

static const struct qcom_llcc_config neo_cfg = {
	.sct_data	= neo_data,
	.size		= ARRAY_SIZE(neo_data),
};

static const struct qcom_llcc_config waipio_cfg = {
	.sct_data	= waipio_data,
	.size		= ARRAY_SIZE(waipio_data),
};

static const struct qcom_llcc_config cape_cfg = {
	.sct_data       = cape_data,
	.size           = ARRAY_SIZE(cape_data),
};

static struct llcc_drv_data *drv_data = (void *) -EPROBE_DEFER;

/**
 * llcc_slice_getd - get llcc slice descriptor
 * @uid: usecase_id for the client
 *
 * A pointer to llcc slice descriptor will be returned on success and
 * and error pointer is returned on failure
 */
struct llcc_slice_desc *llcc_slice_getd(u32 uid)
{
	const struct llcc_slice_config *cfg;
	u32 sz, count;

	if (IS_ERR(drv_data))
		return ERR_CAST(drv_data);

	cfg = drv_data->cfg;
	sz = drv_data->cfg_size;

	for (count = 0; cfg && count < sz; count++, cfg++)
		if (cfg->usecase_id == uid)
			break;

	if (count == sz || !cfg  || IS_ERR_OR_NULL(drv_data->desc))
		return ERR_PTR(-ENODEV);

	return &drv_data->desc[count];
}
EXPORT_SYMBOL_GPL(llcc_slice_getd);

/**
 * llcc_slice_putd - llcc slice descritpor
 * @desc: Pointer to llcc slice descriptor
 */
void llcc_slice_putd(struct llcc_slice_desc *desc)
{
	if (!IS_ERR_OR_NULL(desc))
		WARN(atomic_read(&desc->refcount), " Slice %d is still active\n", desc->slice_id);

}
EXPORT_SYMBOL_GPL(llcc_slice_putd);

static int llcc_update_act_ctrl(u32 sid,
				u32 act_ctrl_reg_val, u32 status)
{
	u32 act_ctrl_reg;
	u32 status_reg;
	u32 slice_status;
	int ret;

	if (IS_ERR(drv_data))
		return PTR_ERR(drv_data);

	act_ctrl_reg = LLCC_TRP_ACT_CTRLn(sid);
	status_reg = LLCC_TRP_STATUSn(sid);

	/* Set the ACTIVE trigger */
	act_ctrl_reg_val |= ACT_CTRL_ACT_TRIG;
	ret = regmap_write(drv_data->bcast_regmap, act_ctrl_reg,
				act_ctrl_reg_val);
	if (ret)
		return ret;

	/* Clear the ACTIVE trigger */
	act_ctrl_reg_val &= ~ACT_CTRL_ACT_TRIG;
	ret = regmap_write(drv_data->bcast_regmap, act_ctrl_reg,
				act_ctrl_reg_val);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(drv_data->bcast_regmap, status_reg,
				      slice_status, !(slice_status & status),
				      0, LLCC_STATUS_READ_DELAY);
	return ret;
}

/**
 * llcc_slice_activate - Activate the llcc slice
 * @desc: Pointer to llcc slice descriptor
 *
 * A value of zero will be returned on success and a negative errno will
 * be returned in error cases
 */
int llcc_slice_activate(struct llcc_slice_desc *desc)
{
	int ret;
	u32 act_ctrl_val;

	if (IS_ERR(drv_data))
		return PTR_ERR(drv_data);

	if (IS_ERR_OR_NULL(desc))
		return -EINVAL;

	mutex_lock(&drv_data->lock);
	if ((atomic_read(&desc->refcount)) >= 1) {
		atomic_inc_return(&desc->refcount);
		mutex_unlock(&drv_data->lock);
		return 0;
	}

	if (test_bit(desc->slice_id, drv_data->bitmap)) {
		mutex_unlock(&drv_data->lock);
		return 0;
	}

	act_ctrl_val = ACT_CTRL_OPCODE_ACTIVATE << ACT_CTRL_OPCODE_SHIFT;

	ret = llcc_update_act_ctrl(desc->slice_id, act_ctrl_val,
				  DEACTIVATE);
	if (ret) {
		mutex_unlock(&drv_data->lock);
		return ret;
	}

	atomic_inc_return(&desc->refcount);
	__set_bit(desc->slice_id, drv_data->bitmap);
	mutex_unlock(&drv_data->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(llcc_slice_activate);

/**
 * llcc_slice_deactivate - Deactivate the llcc slice
 * @desc: Pointer to llcc slice descriptor
 *
 * A value of zero will be returned on success and a negative errno will
 * be returned in error cases
 */
int llcc_slice_deactivate(struct llcc_slice_desc *desc)
{
	u32 act_ctrl_val;
	int ret;

	if (IS_ERR(drv_data))
		return PTR_ERR(drv_data);

	if (IS_ERR_OR_NULL(desc))
		return -EINVAL;

	mutex_lock(&drv_data->lock);
	if ((atomic_read(&desc->refcount)) > 1) {
		atomic_dec_return(&desc->refcount);
		mutex_unlock(&drv_data->lock);
		return 0;
	}

	if (!test_bit(desc->slice_id, drv_data->bitmap)) {
		mutex_unlock(&drv_data->lock);
		return 0;
	}
	act_ctrl_val = ACT_CTRL_OPCODE_DEACTIVATE << ACT_CTRL_OPCODE_SHIFT;

	ret = llcc_update_act_ctrl(desc->slice_id, act_ctrl_val,
				  ACTIVATE);
	if (ret) {
		mutex_unlock(&drv_data->lock);
		return ret;
	}

	atomic_set(&desc->refcount, 0);
	__clear_bit(desc->slice_id, drv_data->bitmap);
	mutex_unlock(&drv_data->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(llcc_slice_deactivate);

/**
 * llcc_get_slice_id - return the slice id
 * @desc: Pointer to llcc slice descriptor
 */
int llcc_get_slice_id(struct llcc_slice_desc *desc)
{
	if (IS_ERR_OR_NULL(desc))
		return -EINVAL;

	return desc->slice_id;
}
EXPORT_SYMBOL_GPL(llcc_get_slice_id);

/**
 * llcc_get_slice_size - return the slice id
 * @desc: Pointer to llcc slice descriptor
 */
size_t llcc_get_slice_size(struct llcc_slice_desc *desc)
{
	if (IS_ERR_OR_NULL(desc))
		return 0;

	return desc->slice_size;
}
EXPORT_SYMBOL_GPL(llcc_get_slice_size);

static int qcom_llcc_cfg_program(struct platform_device *pdev)
{
	int i;
	u32 attr2_cfg;
	u32 attr1_cfg;
	u32 attr0_cfg;
	u32 attr1_val;
	u32 attr0_val;
	u32 attr2_val;
	u32 max_cap_cacheline;
	u32 sz;
	u32 pcb = 0;
	u32 cad = 0;
	u32 wren = 0;
	u32 wrcaen = 0;
	int ret = 0;
	const struct llcc_slice_config *llcc_table;
	struct llcc_slice_desc *desc;
	bool cap_based_alloc_and_pwr_collapse =
		drv_data->cap_based_alloc_and_pwr_collapse;

	sz = drv_data->cfg_size;
	llcc_table = drv_data->cfg;

	for (i = 0; i < sz; i++) {
		drv_data->desc[i].slice_id = llcc_table[i].slice_id;
		drv_data->desc[i].slice_size = llcc_table[i].max_cap;
		atomic_set(&drv_data->desc[i].refcount, 0);
	}

	for (i = 0; i < sz; i++) {
		attr1_cfg = LLCC_TRP_ATTR1_CFGn(llcc_table[i].slice_id);
		attr0_cfg = LLCC_TRP_ATTR0_CFGn(llcc_table[i].slice_id);

		attr1_val = llcc_table[i].cache_mode;
		attr1_val |= llcc_table[i].probe_target_ways <<
				ATTR1_PROBE_TARGET_WAYS_SHIFT;
		attr1_val |= llcc_table[i].fixed_size <<
				ATTR1_FIXED_SIZE_SHIFT;
		attr1_val |= llcc_table[i].priority <<
				ATTR1_PRIORITY_SHIFT;

		max_cap_cacheline = MAX_CAP_TO_BYTES(llcc_table[i].max_cap);

		/* LLCC instances can vary for each target.
		 * The SW writes to broadcast register which gets propagated
		 * to each llcc instace (llcc0,.. llccN).
		 * Since the size of the memory is divided equally amongst the
		 * llcc instances, we need to configure the max cap accordingly.
		 */
		max_cap_cacheline = max_cap_cacheline / drv_data->num_banks;
		max_cap_cacheline >>= CACHE_LINE_SIZE_SHIFT;
		if (drv_data->llcc_ver >= 31) {
			attr1_val |=
				max_cap_cacheline << ATTR1_MAX_CAP_SHIFT_v31;
			attr2_cfg =
				LLCC_TRP_ATTR2_CFGn(llcc_table[i].slice_id);
			attr0_val = llcc_table[i].res_ways;
			attr2_val = llcc_table[i].bonus_ways;
		} else {
			attr1_val |= max_cap_cacheline << ATTR1_MAX_CAP_SHIFT;
			attr0_val =
				llcc_table[i].res_ways & ATTR0_RES_WAYS_MASK;
			attr0_val |=
				llcc_table[i].bonus_ways
					<< ATTR0_BONUS_WAYS_SHIFT;
		}

		ret = regmap_write(drv_data->bcast_regmap, attr1_cfg,
					attr1_val);
		if (ret)
			return ret;
		ret = regmap_write(drv_data->bcast_regmap, attr0_cfg,
					attr0_val);
		if (ret)
			return ret;

		if (drv_data->llcc_ver >= 31) {
			ret = regmap_write(drv_data->bcast_regmap, attr2_cfg,
					attr2_val);
			if (ret)
				return ret;
		}

		if (drv_data->llcc_ver >= 20) {
			wren |= llcc_table[i].write_scid_en <<
						llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
				LLCC_TRP_WRSC_EN, wren);
			if (ret)
				return ret;
		}

		if (drv_data->llcc_ver >= 21) {
			wrcaen |= llcc_table[i].write_scid_cacheable_en <<
						llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
				LLCC_TRP_WRSC_CACHEABLE_EN, wrcaen);
			if (ret)
				return ret;
		}

		if (cap_based_alloc_and_pwr_collapse) {
			cad |= llcc_table[i].dis_cap_alloc <<
				llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
					LLCC_TRP_SCID_DIS_CAP_ALLOC, cad);
			if (ret)
				return ret;

			pcb |= llcc_table[i].retain_on_pc <<
					llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
						LLCC_TRP_PCB_ACT, pcb);
			if (ret)
				return ret;
		}

		if (llcc_table[i].activate_on_init) {
			desc = llcc_slice_getd(llcc_table[i].usecase_id);
			if (PTR_ERR_OR_ZERO(desc)) {
				dev_err(&pdev->dev,
					"Failed to get slice=%d\n", llcc_table[i].slice_id);
				continue;
			}

			ret = llcc_slice_activate(desc);
			if (ret)
				dev_err(&pdev->dev,
					"Failed to activate slice=%d\n", llcc_table[i].slice_id);
		}
	}
	return ret;
}

static int qcom_llcc_remove(struct platform_device *pdev)
{
	/* Set the global pointer to a error code to avoid referencing it */
	drv_data = ERR_PTR(-ENODEV);
	return 0;
}

static struct regmap *qcom_llcc_init_mmio(struct platform_device *pdev,
		const char *name)
{
	void __iomem *base;
	struct regmap_config llcc_regmap_config = {
		.reg_bits = 32,
		.reg_stride = 4,
		.val_bits = 32,
		.fast_io = true,
	};

	base = devm_platform_ioremap_resource_byname(pdev, name);
	if (IS_ERR(base))
		return ERR_CAST(base);

	llcc_regmap_config.name = name;
	return devm_regmap_init_mmio(&pdev->dev, base, &llcc_regmap_config);
}

static int qcom_llcc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret, i;
	struct platform_device *llcc_edac;
	const struct qcom_llcc_config *cfg;
	const struct llcc_slice_config *llcc_cfg;
	void __iomem *ch_reg = NULL;
	u32 sz, ch_reg_sz, ch_reg_off, ch_num;

	drv_data = devm_kzalloc(dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data) {
		ret = -ENOMEM;
		goto err;
	}

	drv_data->regmap = qcom_llcc_init_mmio(pdev, "llcc_base");
	if (IS_ERR(drv_data->regmap)) {
		ret = PTR_ERR(drv_data->regmap);
		goto err;
	}

	drv_data->bcast_regmap =
		qcom_llcc_init_mmio(pdev, "llcc_broadcast_base");
	if (IS_ERR(drv_data->bcast_regmap)) {
		ret = PTR_ERR(drv_data->bcast_regmap);
		goto err;
	}

	if (of_property_match_string(dev->of_node,
				    "compatible", "qcom,llcc-v31") >= 0) {
		drv_data->llcc_ver = 31;
		llcc_regs = llcc_regs_v21;
		drv_data->offsets = llcc_offsets_v31;
		drv_data->num_banks = ARRAY_SIZE(llcc_offsets_v31);
	} else if (of_property_match_string(dev->of_node,
				    "compatible", "qcom,llcc-v21") >= 0) {
		drv_data->llcc_ver = 21;
		llcc_regs = llcc_regs_v21;
		drv_data->offsets = llcc_offsets_v21;
		drv_data->num_banks = ARRAY_SIZE(llcc_offsets_v21);
		if (of_property_match_string(dev->of_node,
				"compatible", "qcom,diwali-llcc") >= 0) {
			drv_data->offsets = llcc_offsets_v21_diwali;
			drv_data->num_banks =
				ARRAY_SIZE(llcc_offsets_v21_diwali);
		}
	} else {
		drv_data->llcc_ver = 20;
		llcc_regs = llcc_regs_v2;
		drv_data->offsets = llcc_offsets_v2;
		drv_data->num_banks = ARRAY_SIZE(llcc_offsets_v2);
	}

	cfg = of_device_get_match_data(&pdev->dev);
	if (!cfg) {
		dev_err(&pdev->dev, "No matching LLCC configuration found\n");
		ret = -ENODEV;
		goto err;
	}

	ch_reg = devm_platform_ioremap_resource_byname(pdev, "multi_ch_reg");
	if (!IS_ERR(ch_reg)) {
		if (of_property_read_u32_index(dev->of_node, "multi-ch-off", 1, &ch_reg_sz)) {
			dev_err(&pdev->dev,
				"Couldn't get size of multi channel feature register\n");
			ret = -ENODEV;
			goto err;
		}

		if (of_property_read_u32(dev->of_node, "multi-ch-off", &ch_reg_off))
			ch_reg_off = 0;

		ch_num = readl_relaxed(ch_reg);
		ch_num = (ch_num >> ch_reg_off) & ((1 << ch_reg_sz) - 1);

		drv_data->cfg_index = ch_num;
		llcc_cfg = cfg[ch_num].sct_data;
		sz = cfg[ch_num].size;

		devm_iounmap(dev, ch_reg);
		ch_reg = NULL;
	} else {
		llcc_cfg = cfg->sct_data;
		sz = cfg->size;
	}

	drv_data->desc = devm_kzalloc(dev, sizeof(struct llcc_slice_desc)*sz, GFP_KERNEL);
	if (IS_ERR_OR_NULL(drv_data->desc)) {
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < sz; i++)
		if (llcc_cfg[i].slice_id > drv_data->max_slices)
			drv_data->max_slices = llcc_cfg[i].slice_id;

	drv_data->cap_based_alloc_and_pwr_collapse =
		of_property_read_bool(pdev->dev.of_node,
				      "cap-based-alloc-and-pwr-collapse");

	drv_data->bitmap = devm_kcalloc(dev,
	BITS_TO_LONGS(drv_data->max_slices), sizeof(unsigned long),
						GFP_KERNEL);
	if (!drv_data->bitmap) {
		ret = -ENOMEM;
		goto err;
	}

	drv_data->cfg = llcc_cfg;
	drv_data->cfg_size = sz;
	mutex_init(&drv_data->lock);
	platform_set_drvdata(pdev, drv_data);

	ret = qcom_llcc_cfg_program(pdev);
	if (ret) {
		pr_err("llcc configuration failed!!\n");
		goto err;
	}

	drv_data->ecc_irq = platform_get_irq(pdev, 0);
	llcc_edac = platform_device_register_data(&pdev->dev,
					"qcom_llcc_edac", -1, drv_data,
					sizeof(*drv_data));
	if (IS_ERR(llcc_edac))
		dev_err(dev, "Failed to register llcc edac driver\n");

	if (of_platform_populate(dev->of_node, NULL, NULL, dev) < 0)
		dev_err(dev, "llcc populate failed!!\n");

	return 0;
err:
	drv_data = ERR_PTR(-ENODEV);
	return ret;
}

static const struct of_device_id qcom_llcc_of_match[] = {
	{ .compatible = "qcom,sc7180-llcc", .data = &sc7180_cfg },
	{ .compatible = "qcom,sdm845-llcc", .data = &sdm845_cfg },
	{ .compatible = "qcom,lahaina-llcc", .data = &lahaina_cfg },
	{ .compatible = "qcom,shima-llcc", .data = &shima_cfg },
	{ .compatible = "qcom,neo-llcc", .data = &neo_cfg },
	{ .compatible = "qcom,waipio-llcc", .data = &waipio_cfg },
	{ .compatible = "qcom,diwali-llcc", .data = &diwali_cfg },
	{ .compatible = "qcom,cape-llcc", .data = &cape_cfg },
	{ }
};

static struct platform_driver qcom_llcc_driver = {
	.driver = {
		.name = "qcom-llcc",
		.of_match_table = qcom_llcc_of_match,
	},
	.probe = qcom_llcc_probe,
	.remove = qcom_llcc_remove,
};
module_platform_driver(qcom_llcc_driver);

MODULE_DESCRIPTION("Qualcomm Last Level Cache Controller");
MODULE_LICENSE("GPL v2");
