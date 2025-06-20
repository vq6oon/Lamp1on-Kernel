/*
 * Copyright (c) 2018 XiaoMi, Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
 * Copyright (C) 2019 XiaoMi, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 and
 *  only version 2 as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <asm/traps.h>
#include <mt-plat/charger_class.h>
#include <mt-plat/charger_type.h>
#include <mt-plat/upmu_common.h>
#include <mt-plat/mtk_boot.h>

#include "mtk_charger_intf.h"
#include "mtk_charger_init.h"

/* Mask/Bit helpers */
#define _SMB1351_MASK(BITS, POS) \
	((unsigned char)(((1 << (BITS)) - 1) << (POS)))
#define SMB1351_MASK(LEFT_BIT_POS, RIGHT_BIT_POS) \
		_SMB1351_MASK((LEFT_BIT_POS) - (RIGHT_BIT_POS) + 1, \
				(RIGHT_BIT_POS))

/* Configuration registers */
#define CHG_CURRENT_CTRL_REG			0x0
#define FAST_CHG_CURRENT_MASK			SMB1351_MASK(7, 4)
#define AC_INPUT_CURRENT_LIMIT_MASK		SMB1351_MASK(3, 0)

#define CHG_OTH_CURRENT_CTRL_REG		0x1
#define PRECHG_CURRENT_MASK			SMB1351_MASK(7, 5)
#define ITERM_MASK				SMB1351_MASK(4, 2)
#define USB_2_3_MODE_SEL_BIT			BIT(1)
#define USB_2_3_MODE_SEL_BY_I2C			0
#define USB_2_3_MODE_SEL_BY_PIN			0x2
#define USB_5_1_CMD_POLARITY_BIT		BIT(0)
#define USB_CMD_POLARITY_500_1_100_0		0
#define USB_CMD_POLARITY_500_0_100_1		0x1

#define VARIOUS_FUNC_REG			0x2
#define SUSPEND_MODE_CTRL_BIT			BIT(7)
#define SUSPEND_MODE_CTRL_BY_PIN		0
#define SUSPEND_MODE_CTRL_BY_I2C		0x80
#define BATT_TO_SYS_POWER_CTRL_BIT		BIT(6)
#define MAX_SYS_VOLTAGE				BIT(5)
#define AICL_EN_BIT				BIT(4)
#define AICL_DET_TH_BIT				BIT(3)
#define APSD_EN_BIT				BIT(2)
#define BATT_OV_BIT				BIT(1)
#define VCHG_FUNC_BIT				BIT(0)

#define VFLOAT_REG				0x3
#define PRECHG_TO_FAST_VOLTAGE_CFG_MASK		SMB1351_MASK(7, 6)
#define VFLOAT_MASK				SMB1351_MASK(5, 0)

#define CHG_CTRL_REG				0x4
#define AUTO_RECHG_BIT				BIT(7)
#define AUTO_RECHG_ENABLE			0
#define AUTO_RECHG_DISABLE			0x80
#define ITERM_EN_BIT				BIT(6)
#define ITERM_ENABLE				0
#define ITERM_DISABLE				0x40
#define MAPPED_AC_INPUT_CURRENT_LIMIT_MASK	SMB1351_MASK(5, 4)
#define AUTO_RECHG_TH_BIT			BIT(3)
#define AUTO_RECHG_TH_50MV			0
#define AUTO_RECHG_TH_100MV			0x8
#define AFCV_MASK				SMB1351_MASK(2, 0)

#define CHG_STAT_TIMERS_CTRL_REG		0x5
#define STAT_OUTPUT_POLARITY_BIT		BIT(7)
#define STAT_OUTPUT_MODE_BIT			BIT(6)
#define STAT_OUTPUT_CTRL_BIT			BIT(5)
#define OTH_CHG_IL_BIT				BIT(4)
#define COMPLETE_CHG_TIMEOUT_MASK		SMB1351_MASK(3, 2)
#define PRECHG_TIMEOUT_MASK			SMB1351_MASK(1, 0)

#define CHG_PIN_EN_CTRL_REG			0x6
#define LED_BLINK_FUNC_BIT			BIT(7)
#define EN_PIN_CTRL_MASK			SMB1351_MASK(6, 5)
#define EN_PIN_CTRL_SHIFT                       5
#define EN_BY_I2C_0_DISABLE			0
#define EN_BY_I2C_0_ENABLE			0x20
#define EN_BY_PIN_HIGH_ENABLE			0x40
#define EN_BY_PIN_LOW_ENABLE			0x60
#define USBCS_CTRL_BIT				BIT(4)
#define USBCS_CTRL_BY_I2C			0
#define USBCS_CTRL_BY_PIN			0x10
#define USBCS_INPUT_STATE_BIT			BIT(3)
#define CHG_ERR_BIT				BIT(2)
#define APSD_DONE_BIT				BIT(1)
#define USB_FAIL_BIT				BIT(0)

#define THERM_A_CTRL_REG			0x7
#define MIN_SYS_VOLTAGE_MASK			SMB1351_MASK(7, 6)
#define LOAD_BATT_10MA_FVC_BIT			BIT(5)
#define THERM_MONITOR_BIT			BIT(4)
#define THERM_MONITOR_EN			0
#define SOFT_COLD_TEMP_LIMIT_MASK		SMB1351_MASK(3, 2)
#define SOFT_HOT_TEMP_LIMIT_MASK		SMB1351_MASK(1, 0)

#define WDOG_SAFETY_TIMER_CTRL_REG		0x8
#define AICL_FAIL_OPTION_BIT			BIT(7)
#define AICL_FAIL_TO_SUSPEND			0
#define AICL_FAIL_TO_150_MA			0x80
#define WDOG_TIMEOUT_MASK			SMB1351_MASK(6, 5)
#define WDOG_IRQ_SAFETY_TIMER_MASK		SMB1351_MASK(4, 3)
#define WDOG_IRQ_SAFETY_TIMER_EN_BIT		BIT(2)
#define WDOG_OPTION_BIT				BIT(1)
#define WDOG_TIMER_EN_BIT			BIT(0)

#define OTG_AND_TLIM_CONTROL				0xA
#define SWITCHING_FREQUENCY_MASK			SMB1351_MASK(7, 6)
#define SWITCHING_FREQUENCY_1MHZ			0x40

#define HARD_SOFT_LIMIT_CELL_TEMP_REG		0xB
#define HARD_LIMIT_COLD_TEMP_ALARM_TRIP_MASK	SMB1351_MASK(7, 6)
#define HARD_LIMIT_HOT_TEMP_ALARM_TRIP_MASK	SMB1351_MASK(5, 4)
#define SOFT_LIMIT_COLD_TEMP_ALARM_TRIP_MASK	SMB1351_MASK(3, 2)
#define SOFT_LIMIT_HOT_TEMP_ALARM_TRIP_MASK	SMB1351_MASK(1, 0)

#define FAULT_INT_REG				0xC
#define HOT_COLD_HARD_LIMIT_BIT			BIT(7)
#define HOT_COLD_SOFT_LIMIT_BIT			BIT(6)
#define BATT_UVLO_IN_OTG_BIT			BIT(5)
#define OTG_OC_BIT				BIT(4)
#define INPUT_OVLO_BIT				BIT(3)
#define INPUT_UVLO_BIT				BIT(2)
#define AICL_DONE_FAIL_BIT			BIT(1)
#define INTERNAL_OVER_TEMP_BIT			BIT(0)

#define STATUS_INT_REG				0xD
#define CHG_OR_PRECHG_TIMEOUT_BIT		BIT(7)
#define RID_CHANGE_BIT				BIT(6)
#define BATT_OVP_BIT				BIT(5)
#define FAST_TERM_TAPER_RECHG_INHIBIT_BIT	BIT(4)
#define WDOG_TIMER_BIT				BIT(3)
#define POK_BIT					BIT(2)
#define BATT_MISSING_BIT			BIT(1)
#define BATT_LOW_BIT				BIT(0)

#define VARIOUS_FUNC_2_REG			0xE
#define CHG_HOLD_OFF_TIMER_AFTER_PLUGIN_BIT	BIT(7)
#define CHG_INHIBIT_BIT				BIT(6)
#define FAST_CHG_CC_IN_BATT_SOFT_LIMIT_MODE_BIT	BIT(5)
#define FVCL_IN_BATT_SOFT_LIMIT_MODE_MASK	SMB1351_MASK(4, 3)
#define HARD_TEMP_LIMIT_BEHAVIOR_BIT		BIT(2)
#define PRECHG_TO_FASTCHG_BIT			BIT(1)
#define STAT_PIN_CONFIG_BIT			BIT(0)

#define FLEXCHARGER_REG				0x10
#define AFVC_IRQ_BIT				BIT(7)
#define CHG_CONFIG_MASK				SMB1351_MASK(6, 4)
#define LOW_BATT_VOLTAGE_DET_TH_MASK		SMB1351_MASK(3, 0)
#define CHG_CONFIG                              0x40

#define VARIOUS_FUNC_3_REG			0x11
#define SAFETY_TIMER_EN_MASK			SMB1351_MASK(7, 6)
#define BLOCK_SUSPEND_DURING_VBATT_LOW_BIT	BIT(5)
#define TIMEOUT_SEL_FOR_APSD_BIT		BIT(4)
#define SDP_SUSPEND_BIT				BIT(3)
#define QC_2P1_AUTO_INCREMENT_MODE_BIT		BIT(2)
#define QC_2P1_AUTH_ALGO_BIT			BIT(1)
#define DCD_EN_BIT				BIT(0)

#define HVDCP_BATT_MISSING_CTRL_REG		0x12
#define HVDCP_ADAPTER_SEL_MASK			SMB1351_MASK(7, 6)
#define HVDCP_EN_BIT				BIT(5)
#define HVDCP_AUTO_INCREMENT_LIMIT_BIT		BIT(4)
#define BATT_MISSING_ON_INPUT_PLUGIN_BIT	BIT(3)
#define BATT_MISSING_2P6S_POLLER_BIT		BIT(2)
#define BATT_MISSING_ALGO_BIT			BIT(1)
#define BATT_MISSING_THERM_PIN_SOURCE_BIT	BIT(0)
#define HVDCP_ADAPTER_SEL_9V			0x40

#define PON_OPTIONS_REG				0x13
#define SYSOK_INOK_POLARITY_BIT			BIT(7)
#define SYSOK_OPTIONS_MASK			SMB1351_MASK(6, 4)
#define INPUT_MISSING_POLLER_CONFIG_BIT		BIT(3)
#define VBATT_LOW_DISABLED_OR_RESET_STATE_BIT	BIT(2)
#define QC_2P1_AUTH_ALGO_IRQ_EN_BIT		BIT(0)

#define OTG_MODE_POWER_OPTIONS_REG		0x14
#define ADAPTER_CONFIG_MASK			SMB1351_MASK(7, 6)
#define MAP_HVDCP_BIT				BIT(5)
#define SDP_LOW_BATT_FORCE_USB5_OVER_USB1_BIT	BIT(4)
#define OTG_HICCUP_MODE_BIT			BIT(2)
#define INPUT_CURRENT_LIMIT_MASK		SMB1351_MASK(1, 0)

#define CHARGER_I2C_CTRL_REG			0x15
#define FULLON_MODE_EN_BIT			BIT(7)
#define I2C_HS_MODE_EN_BIT			BIT(6)
#define SYSON_LDO_OUTPUT_SEL_BIT		BIT(5)
#define VBATT_TRACKING_VOLTAGE_DIFF_BIT		BIT(4)
#define DISABLE_AFVC_WHEN_ENTER_TAPER_BIT	BIT(3)
#define VCHG_IINV_BIT				BIT(2)
#define AFVC_OVERRIDE_BIT			BIT(1)
#define SYSOK_PIN_CONFIG_BIT			BIT(0)

#define VERSION_REG                             0x2E
#define VERSION_MASK                            BIT(1)

/* Command registers */
#define CMD_I2C_REG				0x30
#define CMD_RELOAD_BIT				BIT(7)
#define CMD_BQ_CFG_ACCESS_BIT			BIT(6)

#define CMD_INPUT_LIMIT_REG			0x31
#define CMD_OVERRIDE_BIT			BIT(7)
#define CMD_SUSPEND_MODE_BIT			BIT(6)
#define CMD_INPUT_CURRENT_MODE_BIT		BIT(3)
#define CMD_INPUT_CURRENT_MODE_APSD		0
#define CMD_INPUT_CURRENT_MODE_CMD		0x08
#define CMD_USB_2_3_SEL_BIT			BIT(2)
#define CMD_USB_2_MODE				0
#define CMD_USB_3_MODE				0x4
#define CMD_USB_1_5_AC_CTRL_MASK		SMB1351_MASK(1, 0)
#define CMD_USB_100_MODE			0x2
#define CMD_USB_500_MODE			0x0
#define CMD_USB_AC_MODE				0x1

#define CMD_CHG_REG				0x32
#define CMD_DISABLE_THERM_MONITOR_BIT		BIT(4)
#define CMD_TURN_OFF_STAT_PIN_BIT		BIT(3)
#define CMD_PRE_TO_FAST_EN_BIT			BIT(2)
#define CMD_CHG_EN_BIT				BIT(1)
#define CMD_CHG_DISABLE				0
#define CMD_CHG_ENABLE				0x2
#define CMD_OTG_EN_BIT				BIT(0)

#define CMD_DEAD_BATT_REG			0x33
#define CMD_STOP_DEAD_BATT_TIMER_MASK		SMB1351_MASK(7, 0)

#define CMD_HVDCP_REG				0x34
#define CMD_APSD_RE_RUN_BIT			BIT(7)
#define CMD_FORCE_HVDCP_2P0_BIT			BIT(5)
#define CMD_HVDCP_MODE_MASK			SMB1351_MASK(5, 0)
#define CMD_HVDCP_DEC_BIT			BIT(2)
#define CMD_HVDCP_INC_BIT			BIT(0)

/* Status registers */
#define STATUS_0_REG				0x36
#define STATUS_AICL_BIT				BIT(7)
#define STATUS_INPUT_CURRENT_LIMIT_MASK		SMB1351_MASK(6, 5)
#define STATUS_DCIN_INPUT_CURRENT_LIMIT_MASK	SMB1351_MASK(4, 0)

#define STATUS_1_REG				0x37
#define STATUS_INPUT_RANGE_MASK			SMB1351_MASK(7, 4)
#define STATUS_INPUT_USB_BIT			BIT(0)

#define STATUS_2_REG				0x38
#define STATUS_FAST_CHG_BIT			BIT(7)
#define STATUS_HARD_LIMIT_BIT			BIT(6)
#define STATUS_FLOAT_VOLTAGE_MASK		SMB1351_MASK(5, 0)

#define STATUS_3_REG				0x39
#define STATUS_CHG_BIT				BIT(7)
#define STATUS_PRECHG_CURRENT_MASK		SMB1351_MASK(6, 4)
#define STATUS_FAST_CHG_CURRENT_MASK		SMB1351_MASK(3, 0)

#define STATUS_4_REG				0x3A
#define STATUS_OTG_BIT				BIT(7)
#define STATUS_AFVC_BIT				BIT(6)
#define STATUS_DONE_BIT				BIT(5)
#define STATUS_BATT_LESS_THAN_2V_BIT		BIT(4)
#define STATUS_HOLD_OFF_BIT			BIT(3)
#define STATUS_CHG_MASK				SMB1351_MASK(2, 1)
#define STATUS_NO_CHARGING			0
#define STATUS_FAST_CHARGING			0x4
#define STATUS_PRE_CHARGING			0x2
#define STATUS_TAPER_CHARGING			0x6
#define STATUS_CHG_EN_STATUS_BIT		BIT(0)

#define STATUS_5_REG				0x3B
#define STATUS_SOURCE_DETECTED_MASK		SMB1351_MASK(7, 0)
#define STATUS_PORT_CDP				0x80
#define STATUS_PORT_DCP				0x40
#define STATUS_PORT_OTHER			0x20
#define STATUS_PORT_SDP				0x10
#define STATUS_PORT_ACA_A			0x8
#define STATUS_PORT_ACA_B			0x4
#define STATUS_PORT_ACA_C			0x2
#define STATUS_PORT_ACA_DOCK			0x1

#define STATUS_6_REG				0x3C
#define STATUS_DCD_TIMEOUT_BIT			BIT(7)
#define STATUS_DCD_GOOD_DG_BIT			BIT(6)
#define STATUS_OCD_GOOD_DG_BIT			BIT(5)
#define STATUS_RID_ABD_DG_BIT			BIT(4)
#define STATUS_RID_FLOAT_STATE_MACHINE_BIT	BIT(3)
#define STATUS_RID_A_STATE_MACHINE_BIT		BIT(2)
#define STATUS_RID_B_STATE_MACHINE_BIT		BIT(1)
#define STATUS_RID_C_STATE_MACHINE_BIT		BIT(0)

#define STATUS_7_REG				0x3D
#define STATUS_HVDCP_MASK			SMB1351_MASK(7, 0)
#define CHECK_HVDCP_9V_12V		SMB1351_MASK(3, 2)

#define STATUS_8_REG				0x3E
#define STATUS_USNIN_HV_INPUT_SEL_BIT		BIT(5)
#define STATUS_USBIN_LV_UNDER_INPUT_SEL_BIT	BIT(4)
#define STATUS_USBIN_LV_INPUT_SEL_BIT		BIT(3)

/* Revision register */
#define CHG_REVISION_REG			0x3F
#define GUI_REVISION_MASK			SMB1351_MASK(7, 4)
#define DEVICE_REVISION_MASK			SMB1351_MASK(3, 0)

/* IRQ status registers */
#define IRQ_A_REG				0x40
#define IRQ_HOT_HARD_BIT			BIT(6)
#define IRQ_COLD_HARD_BIT			BIT(4)
#define IRQ_HOT_SOFT_BIT			BIT(2)
#define IRQ_COLD_SOFT_BIT			BIT(0)

#define IRQ_B_REG				0x41
#define IRQ_BATT_TERMINAL_REMOVED_BIT		BIT(6)
#define IRQ_BATT_MISSING_BIT			BIT(4)
#define IRQ_LOW_BATT_VOLTAGE_BIT		BIT(2)
#define IRQ_INTERNAL_TEMP_LIMIT_BIT		BIT(0)

#define IRQ_C_REG				0x42
#define IRQ_PRE_TO_FAST_VOLTAGE_BIT		BIT(6)
#define IRQ_RECHG_BIT				BIT(4)
#define IRQ_TAPER_BIT				BIT(2)
#define IRQ_TERM_BIT				BIT(0)

#define IRQ_D_REG				0x43
#define IRQ_BATT_OV_BIT				BIT(6)
#define IRQ_CHG_ERROR_BIT			BIT(4)
#define IRQ_CHG_TIMEOUT_BIT			BIT(2)
#define IRQ_PRECHG_TIMEOUT_BIT			BIT(0)

#define IRQ_E_REG				0x44
#define IRQ_USBIN_OV_BIT			BIT(6)
#define IRQ_USBIN_UV_BIT			BIT(4)
#define IRQ_AFVC_BIT				BIT(2)
#define IRQ_POWER_OK_BIT			BIT(0)

#define IRQ_F_REG				0x45
#define IRQ_OTG_OVER_CURRENT_BIT		BIT(6)
#define IRQ_OTG_FAIL_BIT			BIT(4)
#define IRQ_RID_BIT				BIT(2)
#define IRQ_OTG_OC_RETRY_BIT			BIT(0)

#define IRQ_G_REG				0x46
#define IRQ_SOURCE_DET_BIT			BIT(6)
#define IRQ_AICL_DONE_BIT			BIT(4)
#define IRQ_AICL_FAIL_BIT			BIT(2)
#define IRQ_CHG_INHIBIT_BIT			BIT(0)

#define IRQ_H_REG				0x47
#define IRQ_IC_LIMIT_STATUS_BIT			BIT(5)
#define IRQ_HVDCP_2P1_STATUS_BIT		BIT(4)
#define IRQ_HVDCP_AUTH_DONE_BIT			BIT(2)
#define IRQ_WDOG_TIMEOUT_BIT			BIT(0)

/* constants */
#define USB2_MIN_CURRENT_MA			100
#define USB2_MAX_CURRENT_MA			500
#define USB3_MIN_CURRENT_MA			150
#define USB3_MAX_CURRENT_MA			900
#define SMB1351_IRQ_REG_COUNT			8
#define SMB1351_CHG_PRE_MIN_MA			100
#define SMB1351_CHG_FAST_MIN_MA			1000
#define SMB1351_CHG_FAST_MAX_MA			4500
#define SMB1351_CHG_PRE_SHIFT			5
#define SMB1351_CHG_FAST_SHIFT			4
#define DEFAULT_BATT_CAPACITY			50
#define DEFAULT_BATT_TEMP			250
#define SUSPEND_CURRENT_MA			2

#define CHG_ITERM_70MA				0x1C
#define CHG_ITERM_100MA				0x18
#define CHG_ITERM_200MA				0x0
#define CHG_ITERM_300MA				0x04
#define CHG_ITERM_400MA				0x08
#define CHG_ITERM_500MA				0x0C
#define CHG_ITERM_600MA				0x10
#define CHG_ITERM_700MA				0x14

#define ADAPTER_NONE 0x00
#define USBIN_COLLAPSE_TIME ((unsigned long)100)

static int smb1351_set_hvdcp_dpdm(struct charger_device *chg_dev);
extern int cycle_count;
static unsigned long f_vbusin_uv_time;
static unsigned long s_vbusin_uv_time;
extern bool thermal_is_500;
enum {
	USER	= BIT(0),
	THERMAL = BIT(1),
	CURRENT = BIT(2),
	SOC	= BIT(3),
};

enum chip_version {
	SMB_UNKNOWN = 0,
	SMB1350,
	SMB1351,
	SMB_MAX_TYPE,
};

enum usbsw_state {
	USBSW_CHG = 0,
	USBSW_USB,
};

static enum power_supply_type const smblib_apsd_results[] = {
	POWER_SUPPLY_TYPE_UNKNOWN,
	POWER_SUPPLY_TYPE_USB,
	POWER_SUPPLY_TYPE_USB_CDP,
	POWER_SUPPLY_TYPE_USB_FLOAT,
	POWER_SUPPLY_TYPE_USB_DCP,
};

enum thermal_status_levels {
	TEMP_SHUT_DOWN = 0,
	TEMP_SHUT_DOWN_SMB,
	TEMP_ALERT_LEVEL,
	TEMP_ABOVE_RANGE,
	TEMP_WITHIN_RANGE,
	TEMP_BELOW_RANGE,
};
struct smb1351_desc {
	u32 ichg;		/* uA */
	u32 mivr;		/* uV */
	u32 cv;			/* uV */
	u32 ieoc;		/* uA */
	u32 safety_timer;	/* hour */
	bool en_te;
	bool en_wdt;
	bool en_st;
	const char *chg_dev_name;
	const char *alias_name;
};

/* These default values will be used if there's no property in dts */
static struct smb1351_desc smb1351_default_desc = {
	.ichg = 1800000,	/* uA */
	.mivr = 4500000,	/* uV */
	.cv = 4396000,		/* uV */
	.ieoc = 200000,		/* uA */
	.safety_timer = 12,	/* hour */
	.en_te = true,
	.en_wdt = true,
	.en_st = true,
	.chg_dev_name = "secondary_chg",
	.alias_name = "smb1351",
};

struct smb1351_charger {
	struct i2c_client	*client;
	struct device		*dev;
	struct mt_charger	*mt_chg;
	struct smb1351_desc	*desc;
	struct charger_device	*chg_dev;
	struct charger_properties	chg_props;
	u32			intr_gpio;
	/* u32 en_gpio */
	int			irq;
	bool			bc12_supported;
	bool			bc12_en;
	bool			tcpc_attach;
	enum charger_type	chg_type;
	struct mutex		chgdet_lock;
	bool			chip_enable;
	enum hvdcp_status	hvdcp_type;
	bool			hvdcp_dpdm_status;
	unsigned int		connect_therm_gpio;
	unsigned int		suspend_gpio;
	bool			shutdown_status;
	bool			otg_enable;
	bool			is_connect;
	int			chg_current_set;
	struct charger_consumer *chg_consumer;
	bool			recharge_disabled;
	int			recharge_mv;
	bool			iterm_disabled;
	int			vfloat_mv;
	int			chg_present;
	bool		vbus_disable;
	enum thermal_status_levels 		thermal_status;
	int			connector_temp;
	int			entry_time;
	int			rerun_apsd_count;
	bool			chg_autonomous_mode;
	bool			disable_apsd;
	bool			battery_missing;
	bool			resume_completed;
	bool			irq_waiting;
	struct work_struct	apsd_update_work;
	struct delayed_work	chg_hvdcp_det_work;
	struct delayed_work	hvdcp_500_mode_check_work;
	struct delayed_work	sw_rerun_aicl_work;
	struct delayed_work	enable_hvdcp_work;
	struct delayed_work	check_type_work;
	// struct delayed_work	conn_therm_work;
	struct delayed_work	float_chg_det_work;
	struct delayed_work	delay_init_work;
	enum chip_version       version;

	/* status tracking */
	bool			batt_full;
	bool			batt_hot;
	bool			batt_cold;
	bool			batt_warm;
	bool			batt_cool;

	int			fastchg_current_max_ma;
	bool			parallel_charger;
	bool			bms_controlled_charging;
	bool			usbin_ov;
	bool			force_hvdcp_2p0;

	/* psy */
	struct power_supply	*usb_psy;
	struct power_supply	*bms_psy;
	struct power_supply	*main_psy;
	struct power_supply	*parallel_psy;
	struct power_supply_desc        parallel_psy_d;
	struct power_supply	*wireless_psy;
	struct power_supply_desc        wireless_psy_d;
	struct mutex		irq_complete;

	struct power_supply	*charger_psy;

	struct dentry		*debug_root;
	u32			peek_poke_address;
};

struct smb_irq_info {
	const char		*name;
	int (*smb_irq)(struct smb1351_charger *chip, u8 rt_stat);
	int			high;
	int			low;
};

struct irq_handler_info {
	u8			stat_reg;
	u8			val;
	u8			prev_val;
	struct smb_irq_info	irq_info[4];
};

/* USB input charge current */
static int usb_chg_current[] = {
	500, 685, 1000, 1100, 1200, 1300, 1500, 1600,
	1700, 1800, 2000, 2200, 2500, 3000, 3500, 3940,
};

static int fast_chg_current[] = {
	1000, 1200, 1400, 1600, 1800, 2000, 2200, 2400,
	2600, 2800, 3000, 3400, 3600, 3800, 4000, 4500,
};

static int pre_chg_current[] = {
	100, 120, 200, 300, 400, 500, 600, 700,
};

struct battery_status {
	bool			batt_hot;
	bool			batt_warm;
	bool			batt_cool;
	bool			batt_cold;
	bool			batt_present;
};

enum {
	BATT_HOT = 0,
	BATT_WARM,
	BATT_NORMAL,
	BATT_COOL,
	BATT_COLD,
	BATT_MISSING,
	BATT_STATUS_MAX,
};

enum hvdcp_status hvdcp_type_tmp;
int usb_det_flag;
static int hvdcp_det_count;
static int hvdcp_rerun_aicl_count;
#define HVDCP_RERUN_AICL_COUNT_MAX 3
static void _smb1351_enable_hvdcp_det(struct smb1351_charger *chip, bool enable);

static int smb1351_read_reg(struct smb1351_charger *chip, int reg, u8 *val)
{
	s32 ret;

	pm_stay_awake(chip->dev);
	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (ret < 0) {
		pr_err("i2c read fail: can't read from %02x: %d\n", reg, ret);
		pm_relax(chip->dev);
		return ret;
	} else {
		*val = ret;
	}
	pm_relax(chip->dev);

	//pr_info("Reading 0x%02x=0x%02x\n", reg, *val);
	return 0;
}

static int smb1351_write_reg(struct smb1351_charger *chip, int reg, u8 val)
{
	s32 ret;

	pm_stay_awake(chip->dev);
	ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	if (ret < 0) {
		pr_err("i2c write fail: can't write %02x to %02x: %d\n",
			val, reg, ret);
		pm_relax(chip->dev);
		return ret;
	}
	pm_relax(chip->dev);
	pr_debug("Writing 0x%02x=0x%02x\n", reg, val);
	return 0;
}

static int smb1351_masked_write(struct smb1351_charger *chip, int reg,
							u8 mask, u8 val)
{
	s32 rc;
	u8 temp;

	rc = smb1351_read_reg(chip, reg, &temp);
	if (rc) {
		pr_err("read failed: reg=%03X, rc=%d\n", reg, rc);
		return rc;
	}
	temp &= ~mask;
	temp |= val & mask;
	rc = smb1351_write_reg(chip, reg, temp);
	if (rc) {
		pr_err("write failed: reg=%03X, rc=%d\n", reg, rc);
		return rc;
	}
	return 0;
}

static int smb1351_enable_volatile_writes(struct smb1351_charger *chip)
{
	int rc;

	rc = smb1351_masked_write(chip, CMD_I2C_REG, CMD_BQ_CFG_ACCESS_BIT,
							CMD_BQ_CFG_ACCESS_BIT);
	if (rc)
		pr_err("Couldn't write CMD_BQ_CFG_ACCESS_BIT rc=%d\n", rc);

	return rc;
}

static int smb_chip_get_version(struct smb1351_charger *chip)
{
	u8 ver;
	int rc = 0;

	//if (chip->version == SMB_UNKNOWN) {
		rc = smb1351_read_reg(chip, VERSION_REG, &ver);
		if (rc) {
			pr_err("Couldn't read version rc=%d\n", rc);
			return rc;
		}

		/* If bit 1 is set, it is SMB1350 */
		if (ver & VERSION_MASK)
			chip->version = SMB1350;
		else
			chip->version = SMB1351;
//	}
	pr_err("###%s, i2c_addr:0x%02x, version:%d\n", __func__, chip->client->addr, chip->version);

	return rc;
}

static int smb1351_psy_chg_type_changed(struct smb1351_charger *chip, bool force_update)
{
	int ret = 0;
	union power_supply_propval propval;

	propval.intval = !!(chip->chg_type);
	ret = power_supply_set_property(chip->charger_psy,
					POWER_SUPPLY_PROP_ONLINE,
					&propval);
	if (ret < 0)
		pr_err("%s: psy online failed, ret = %d\n", __func__, ret);
	else
		pr_info("%s: online = %d\n", __func__, !!(chip->chg_type));


	propval.intval = chip->chg_type;
	ret = power_supply_set_property(chip->charger_psy,
					POWER_SUPPLY_PROP_CHARGE_TYPE,
					&propval);
	if (ret < 0)
		pr_err("%s: psy type failed, ret = %d\n", __func__, ret);
	else
		pr_info("%s: chg_type = %d\n", __func__, chip->chg_type);


	if (!force_update)
		return 0;

	if (chip->mt_chg->usb_desc.type == POWER_SUPPLY_TYPE_USB_HVDCP &&
		chip->chg_type == STANDARD_CHARGER)
		return 0;

	if (chip->chg_type > sizeof(smblib_apsd_results))
		chip->mt_chg->usb_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
	else
		chip->mt_chg->usb_desc.type = smblib_apsd_results[chip->chg_type];

	return ret;
}

static int smb1351_set_usbsw_state(struct smb1351_charger *chip, int state)
{
	pr_info("%s: state = %s\n", __func__, state ? "usb" : "chg");

	/* Switch D+D- to AP/SMB1351 */
	if (state == USBSW_CHG)
		Charger_Detect_Init();
	else
		Charger_Detect_Release();

	return 0;
}

static int rerun_apsd(struct smb1351_charger *chip)
{
	int rc;

	pr_err("Reruning APSD\n");

	rc = smb1351_enable_volatile_writes(chip);
	if (rc)
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);

	rc = smb1351_masked_write(chip, CMD_HVDCP_REG, CMD_APSD_RE_RUN_BIT,
						CMD_APSD_RE_RUN_BIT);
	if (rc)
		pr_err("Couldn't re-run APSD algo\n");

	return 0;
}

static int smb1351_apsd_complete_handler(struct smb1351_charger *chip,
						u8 status)
{
	unsigned int sub_time;
	if (status == 64) {
		f_vbusin_uv_time = jiffies;
	}
	if (status == 0) {
		s_vbusin_uv_time = jiffies;
		sub_time = s_vbusin_uv_time - f_vbusin_uv_time;
		jiffies_to_msecs(sub_time);
		if (sub_time < USBIN_COLLAPSE_TIME) {
			_smb1351_enable_hvdcp_det(chip, false);
			pr_err("222f_time:%lu; s_time:%lu; sub:%lu sub_time:%d\n", f_vbusin_uv_time, s_vbusin_uv_time, (s_vbusin_uv_time - f_vbusin_uv_time), sub_time);
		}
	}
//	pr_err("f_time:%lu; s_time:%lu; sub:%lu\n",f_vbusin_uv_time ,s_vbusin_uv_time ,(s_vbusin_uv_time - f_vbusin_uv_time));
	pr_info("%s: status: %d\n", __func__, status);
	schedule_work(&chip->apsd_update_work);
	return 0;
}

static int smb1351_usbin_uv_handler(struct smb1351_charger *chip, u8 status)
{

	pr_info("%s: status: %d\n", __func__, status);

	if (hvdcp_type_tmp == 1) {
		pr_err("smb1351 hvdcp trgger uv disable hvdcp\n");
		_smb1351_enable_hvdcp_det(chip, false);
		rerun_apsd(chip);
		chip->bc12_en = true;
		chip->rerun_apsd_count = 0;
		schedule_work(&chip->apsd_update_work);
	}
	/* use this to detect USB insertion only if !apsd */
	if (chip->disable_apsd) {
		/*
		 * If APSD is disabled, src det interrupt won't trigger.
		 * Hence use usbin_uv for removal and insertion notification
		 */
		if (status == 0) {
			chip->chg_present = true;
			pr_debug("updating usb_psy present=%d\n",
						chip->chg_present);
			/* power_supply_set_supply_type(chip->usb_psy,
						POWER_SUPPLY_TYPE_USB);
			power_supply_set_present(chip->usb_psy,
						chip->chg_present); */
		} else {
			chip->chg_present = false;
			/* power_supply_set_supply_type(chip->usb_psy,
						POWER_SUPPLY_TYPE_UNKNOWN);
			power_supply_set_present(chip->usb_psy, chip->
								chg_present); */
			pr_debug("updating usb_psy present=%d\n",
							chip->chg_present);
		}
		return 0;
	}

	pr_debug("chip->chg_present = %d\n", chip->chg_present);

	return 0;
}

static int smb1351_usbin_ov_handler(struct smb1351_charger *chip, u8 status)
{
	int health;
	int rc = 0;
	u8 reg = 0;

	pr_debug("%s enter\n", __func__);
	rc = smb1351_read_reg(chip, IRQ_E_REG, &reg);
	if (rc)
		pr_err("Couldn't read IRQ_E rc = %d\n", rc);

	if (status != 0) {
		chip->chg_present = false;
		chip->usbin_ov = true;
		/* power_supply_set_supply_type(chip->usb_psy,
						POWER_SUPPLY_TYPE_UNKNOWN);
		power_supply_set_present(chip->usb_psy, chip->chg_present); */
	} else {
		chip->usbin_ov = false;
		if (reg & IRQ_USBIN_UV_BIT)
			pr_debug("Charger unplugged from OV\n");
		else
			smb1351_apsd_complete_handler(chip, 1);
	}

	if (chip->usb_psy) {
		health = status ? POWER_SUPPLY_HEALTH_OVERVOLTAGE
					: POWER_SUPPLY_HEALTH_GOOD;
		/* power_supply_set_health_state(chip->usb_psy, health); */
		pr_debug("chip ov status is %d\n", health);
	}
	pr_debug("chip->chg_present = %d\n", chip->chg_present);

	return 0;
}

static int smb1351_fast_chg_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s enter\n", __func__);
	return 0;
}

static int smb1351_chg_term_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s enter\n", __func__);
	if (!chip->bms_controlled_charging)
		chip->batt_full = !!status;
	return 0;
}

static int smb1351_safety_timeout_handler(struct smb1351_charger *chip,
						u8 status)
{
	pr_debug("%s safety_timeout triggered\n", __func__);
	return 0;
}

static int smb1351_aicl_done_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s aicl_done triggered\n", __func__);
	return 0;
}

static int smb1351_hot_hard_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s status = 0x%02x\n", __func__, status);
	chip->batt_hot = !!status;
	return 0;
}
static int smb1351_cold_hard_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s status = 0x%02x\n", __func__, status);
	chip->batt_cold = !!status;
	return 0;
}
static int smb1351_hot_soft_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s status = 0x%02x\n", __func__, status);
	chip->batt_warm = !!status;
	return 0;
}
static int smb1351_cold_soft_handler(struct smb1351_charger *chip, u8 status)
{
	pr_debug("%s status = 0x%02x\n", __func__, status);
	chip->batt_cool = !!status;
	return 0;
}

static int smb1351_battery_missing_handler(struct smb1351_charger *chip,
						u8 status)
{
	pr_debug("%s \n", __func__);

	if (status)
		chip->battery_missing = true;
	else
		chip->battery_missing = false;

	return 0;
}

static struct irq_handler_info handlers[] = {
	[0] = {
		.stat_reg	= IRQ_A_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "cold_soft",
				.smb_irq = smb1351_cold_soft_handler,
			},
			{	.name	 = "hot_soft",
				.smb_irq = smb1351_hot_soft_handler,
			},
			{	.name	 = "cold_hard",
				.smb_irq = smb1351_cold_hard_handler,
			},
			{	.name	 = "hot_hard",
				.smb_irq = smb1351_hot_hard_handler,
			},
		},
	},
	[1] = {
		.stat_reg	= IRQ_B_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "internal_temp_limit",
			},
			{	.name	 = "vbatt_low",
			},
			{	.name	 = "battery_missing",
				.smb_irq = smb1351_battery_missing_handler,
			},
			{	.name	 = "batt_therm_removed",
			},
		},
	},
	[2] = {
		.stat_reg	= IRQ_C_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "chg_term",
				.smb_irq = smb1351_chg_term_handler,
			},
			{	.name	 = "taper",
			},
			{	.name	 = "recharge",
			},
			{	.name	 = "fast_chg",
				.smb_irq = smb1351_fast_chg_handler,
			},
		},
	},
	[3] = {
		.stat_reg	= IRQ_D_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "prechg_timeout",
			},
			{	.name	 = "safety_timeout",
				.smb_irq = smb1351_safety_timeout_handler,
			},
			{	.name	 = "chg_error",
			},
			{	.name	 = "batt_ov",
			},
		},
	},
	[4] = {
		.stat_reg	= IRQ_E_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "power_ok",
			},
			{	.name	 = "afvc",
			},
			{	.name	 = "usbin_uv",
				.smb_irq = smb1351_usbin_uv_handler,
			},
			{	.name	 = "usbin_ov",
				.smb_irq = smb1351_usbin_ov_handler,
			},
		},
	},
	[5] = {
		.stat_reg	= IRQ_F_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "otg_oc_retry",
			},
			{	.name	 = "rid",
			},
			{	.name	 = "otg_fail",
			},
			{	.name	 = "otg_oc",
			},
		},
	},
	[6] = {
		.stat_reg	= IRQ_G_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "chg_inhibit",
			},
			{	.name	 = "aicl_fail",
			},
			{	.name	 = "aicl_done",
				.smb_irq = smb1351_aicl_done_handler,
			},
			{	.name	 = "apsd_complete",
				.smb_irq = smb1351_apsd_complete_handler,
			},
		},
	},
	[7] = {
		.stat_reg	= IRQ_H_REG,
		.val		= 0,
		.prev_val	= 0,
		.irq_info	= {
			{	.name	 = "wdog_timeout",
			},
			{	.name	 = "hvdcp_auth_done",
			},
		},
	},
};

extern void dump_regs(struct smb1351_charger *chip);
#define RP_22K_CUR_LVL 1500
#define HVDCP_NOTIFY_MS		5000
#define SDP_NOTIFY_MS		1000
#define MAX_RERUN_APSD_COUNT 3
static void apsd_update_work(struct work_struct *work)
{
	struct smb1351_charger *chip = container_of(work, struct smb1351_charger,
						apsd_update_work);

	int rc = 0;
	u8 reg = 0;

	/* charger class */
	mutex_lock(&chip->chgdet_lock);

	if (chip->bc12_supported) {
retry_apsd:
		rc = smb1351_enable_volatile_writes(chip);
		if (rc) {
			pr_err("Couldn't configure volatile writes rc=%d\n", rc);
		}

		rc = smb1351_read_reg(chip, STATUS_5_REG, &reg);
		if (rc) {
			pr_err("Couldn't read STATUS_5 rc = %d\n", rc);
			goto out;
		}
		pr_info("STATUS_5_REG(0x3B)=%x\n", reg);

		switch (reg) {
		case STATUS_PORT_CDP:
			chip->chg_type = CHARGING_HOST;
			break;
		case STATUS_PORT_DCP:
		case STATUS_PORT_OTHER:
			chip->chg_type = STANDARD_CHARGER;
			if (chip->bc12_en) {
				pr_info("bc12 enabled, schedule hvdcp work.\n");
				queue_delayed_work(system_power_efficient_wq, &chip->chg_hvdcp_det_work,
							msecs_to_jiffies(HVDCP_NOTIFY_MS));
			}
			queue_delayed_work(system_power_efficient_wq, &chip->sw_rerun_aicl_work, msecs_to_jiffies(6000));
			break;
		case STATUS_PORT_SDP:
			chip->chg_type = STANDARD_HOST;
			queue_delayed_work(system_power_efficient_wq, &chip->float_chg_det_work, msecs_to_jiffies(SDP_NOTIFY_MS));
			break;
		// case STATUS_PORT_OTHER:
		// 	chip->chg_type = STANDARD_CHARGER;
		// 	break;
		case 0:
			chip->chg_type = CHARGER_UNKNOWN;
			break;
		default:
			chip->chg_type = STANDARD_HOST;
			break;
		}

		if (chip->chg_type == STANDARD_HOST || chip->chg_type == CHARGING_HOST)
			smb1351_set_usbsw_state(chip, USBSW_USB);

		if (chip->chg_type == CHARGER_UNKNOWN &&
				chip->rerun_apsd_count < MAX_RERUN_APSD_COUNT) {
			chip->rerun_apsd_count++;
			rerun_apsd(chip);
			pr_err("rerun_apsd. rerun_apsd_count = %d.\n", chip->rerun_apsd_count);
			goto retry_apsd;
		}

		if (chip->chg_type == CHARGER_UNKNOWN &&
				chip->rerun_apsd_count >= MAX_RERUN_APSD_COUNT) {
			chip->hvdcp_type = HVDCP_NULL;
			hvdcp_type_tmp = 0;
			chip->hvdcp_dpdm_status = 0;
			hvdcp_det_count = 0;
			pr_err("rerun_apsd CHARGER_UNKNOWN\n");
		}

		if (chip->chg_type != CHARGER_UNKNOWN) {
			chip->rerun_apsd_count = 0;
			smb1351_psy_chg_type_changed(chip, true);
			pr_err("chg_type: %d\n", chip->chg_type);
		}
	}
out:
	mutex_unlock(&chip->chgdet_lock);
}
#define VBUS_PLUG_OUT_THRESHOLD 4000

static void smb1351_rerun_aicl(struct smb1351_charger *chip)
{
	int rc;
	rc = smb1351_masked_write(chip, CMD_INPUT_LIMIT_REG, 0x03, 0x00);
	if (rc) {
		pr_err("rerun aicl set 500 fail rc = %d\n", rc);
	}
	msleep(200);
	rc = smb1351_masked_write(chip, CMD_INPUT_LIMIT_REG, 0x03, 0x01);
	if (rc) {
		pr_err("rerun aicl set HC fail rc = %d\n", rc);
	}
	msleep(200);
}

static void smb1351_check_type_work(struct work_struct *work)
{
	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, check_type_work.work);
	u8 reg;
	smb1351_read_reg(chip, STATUS_5_REG, &reg);
	pr_err("STATUS_5_REG : %02x\n", reg);
	if (chip->chg_type == CHARGER_UNKNOWN && reg != 0x0) {
		pr_err("type is CHARGER_UNKNOWN, but 3B have type\n");
		schedule_work(&chip->apsd_update_work);

	}
}

static void smb1351_enable_hvdcp_work(struct work_struct *work)
{

	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, enable_hvdcp_work.work);
	union power_supply_propval val;
	power_supply_get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);

	pr_debug("%s vbus:%d \n", __func__, val.intval);
	if (val.intval < VBUS_PLUG_OUT_THRESHOLD) {
		pr_debug("%s vbus low, return. \n", __func__);
		_smb1351_enable_hvdcp_det(chip, true);
	}

}
static void smb1351_hvdcp_500_mode_check_work(struct work_struct *work)
{
	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, hvdcp_500_mode_check_work.work);
	u8 usb_mode;
	int rc;
	if (thermal_is_500 == true) {
		queue_delayed_work(system_power_efficient_wq, &chip->hvdcp_500_mode_check_work, msecs_to_jiffies(5000));
		return;
	}
	rc = smb1351_read_reg(chip, STATUS_0_REG, &usb_mode);
	if (rc) {
		pr_err("Couldn't read STATUS_0_REG rc = %d\n", rc);
	}
	if (usb_mode & 0x40) {
		if (hvdcp_rerun_aicl_count < HVDCP_RERUN_AICL_COUNT_MAX) {
			hvdcp_rerun_aicl_count++;
			smb1351_rerun_aicl(chip);
			rc = smb1351_read_reg(chip, STATUS_0_REG, &usb_mode);
			pr_err("hvdcp mode usb in 500 reg:%02x\n", usb_mode);
		} else {
			_smb1351_enable_hvdcp_det(chip, false);
			rerun_apsd(chip);
			chip->bc12_en = true;
			chip->rerun_apsd_count = 0;
		}
	} else {
		hvdcp_rerun_aicl_count = 0;
	}
	queue_delayed_work(system_power_efficient_wq, &chip->hvdcp_500_mode_check_work, msecs_to_jiffies(5000));
}

static void smb1351_sw_rerun_aicl_work(struct work_struct *work)
{
	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, sw_rerun_aicl_work.work);
	int rc;
	u8 aicl_result;
	u8 input_current;

	rc = smb1351_read_reg(chip, STATUS_0_REG, &aicl_result);
	if (rc) {
		pr_err("read aicl result rc = %d\n", rc);
	}
	rc = smb1351_read_reg(chip, CHG_CURRENT_CTRL_REG, &input_current);
	if (rc) {
		pr_err("read input result rc = %d\n", rc);
	}

	aicl_result = aicl_result & AC_INPUT_CURRENT_LIMIT_MASK;
	input_current = input_current & AC_INPUT_CURRENT_LIMIT_MASK;
	pr_err("compare aicl input:%02x aicl:%02x\n", input_current, aicl_result);
	if (input_current > aicl_result) {
		smb1351_rerun_aicl(chip);
	}

	queue_delayed_work(system_power_efficient_wq, &chip->sw_rerun_aicl_work, msecs_to_jiffies(120000));
}

static void smb1351_chg_hvdcp_det_work(struct work_struct *work)
{
	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, chg_hvdcp_det_work.work);
	int rc;
	int hvdcp_det_status = 0;
	u8 hvdcp_status = 0, hvdcp_result = 0;
	union power_supply_propval val;

	power_supply_get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);

	pr_debug("%s vbus:%d \n", __func__, val.intval);
	if (val.intval < VBUS_PLUG_OUT_THRESHOLD) {
		pr_debug("%s vbus low, return. \n", __func__);
		return ;
	}

	rc = smb1351_read_reg(chip, STATUS_7_REG, &hvdcp_status);
	if (rc) {
		pr_err("Couldn't read STATUS_7 rc = %d\n", rc);
	}
	hvdcp_rerun_aicl_count = 0;
	if (hvdcp_status & CHECK_HVDCP_9V_12V) {
		chip->is_connect  = true;
		chip->hvdcp_type = HVDCP;
		hvdcp_type_tmp = HVDCP;
		hvdcp_det_status = 1;
		chip->mt_chg->usb_desc.type = POWER_SUPPLY_TYPE_USB_HVDCP;
		power_supply_changed(chip->usb_psy);
		queue_delayed_work(system_power_efficient_wq, &chip->hvdcp_500_mode_check_work, msecs_to_jiffies(5000));
	}
	pr_err("QC charger detected. hvdcp= %x.\n",	hvdcp_status);
	rc = smb1351_read_reg(chip, IRQ_H_REG, &hvdcp_result);
	if (rc) {
		pr_err("Couldn't read IRQ_H_REG rc = %d\n", rc);
	}

	if (hvdcp_result & IRQ_HVDCP_2P1_STATUS_BIT) {
		chip->is_connect  = true;
		chip->hvdcp_type = HVDCP_3;
		hvdcp_type_tmp = HVDCP_3;
		hvdcp_det_status = 1;
		chip->mt_chg->usb_desc.type = POWER_SUPPLY_TYPE_USB_HVDCP_3;
		power_supply_changed(chip->usb_psy);
		smb1351_set_hvdcp_dpdm(chip->chg_dev);
		queue_delayed_work(system_power_efficient_wq, &chip->hvdcp_500_mode_check_work, msecs_to_jiffies(5000));
	}
	pr_err("QC3 hvdcp_result = 0x%x det_count = %d.\n", hvdcp_result, hvdcp_det_count);
	if (hvdcp_det_status == 0 && hvdcp_det_count < 3) {
		hvdcp_det_count++;
		queue_delayed_work(system_power_efficient_wq, &chip->chg_hvdcp_det_work,
							msecs_to_jiffies(HVDCP_NOTIFY_MS));
	}
}

static void smb1351_float_chg_det_work(struct work_struct *work)
{
	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, float_chg_det_work.work);
	struct charger_manager *cm = chip->chg_consumer->cm;
	union power_supply_propval val;

	power_supply_get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (cm->usb_state != 2 && usb_det_flag == 0) {
		pr_err("float detected msleep11 1S\n");
		msleep(1000);
		rerun_apsd(chip);
		usb_det_flag = 1;
	//	return;
	}
	pr_err("%s usb_state: %d. vbus: %d\n", __func__, cm->usb_state, val.intval);
	if (cm->usb_state != 2 && val.intval > VBUS_PLUG_OUT_THRESHOLD && chip->chg_type == STANDARD_HOST) {
		chip->chg_type = NONSTANDARD_CHARGER;
		smb1351_psy_chg_type_changed(chip, false);
		chip->mt_chg->usb_desc.type = POWER_SUPPLY_TYPE_USB_FLOAT;
		power_supply_changed(chip->usb_psy);
	}
}
static int smblib_set_vbus_disable(struct smb1351_charger *chip,
					bool disable)
{
	pr_debug("%s disable: %d\n", __func__, disable);
	if (disable) {
		if (gpio_is_valid(chip->connect_therm_gpio))
			gpio_direction_output(chip->connect_therm_gpio, 1);
		chip->vbus_disable = true;
	} else {
		if (gpio_is_valid(chip->connect_therm_gpio))
			gpio_direction_output(chip->connect_therm_gpio, 0);
		chip->vbus_disable = false;
	}
	return 0;
}

#define THERM_REG_RECHECK_DELAY_200MS		200	/* 1 sec */
#define THERM_REG_RECHECK_DELAY_1S		1000	/* 1 sec */
#define THERM_REG_RECHECK_DELAY_5S		5000	/* 5 sec */
#define THERM_REG_RECHECK_DELAY_8S		8000	/* 8 sec */
#define THERM_REG_RECHECK_DELAY_10S		10000	/* 10 sec */

#define CONNECTOR_THERM_ABOVE		200	/* 20 Dec */
#define CONNECTOR_THERM_HIG		500	/* 	50 Dec */
#define CONNECTOR_THERM_TOO_HIG		700	/* 70 Dec */
#define CONNECTOR_THERM_MAX		1200	/* 120 Dec */

// static int smblib_set_sw_conn_therm_regulation(struct smb1351_charger *chip,
// 						bool enable)
// {
// 	pr_debug("%s enable: %d\n", __func__, enable);

// 	if (enable) {
// 		chip->entry_time = ktime_get();
// 		schedule_delayed_work(&chip->conn_therm_work,
// 				msecs_to_jiffies(THERM_REG_RECHECK_DELAY_1S));

// 	} else {
// 		if (chip->thermal_status != TEMP_ABOVE_RANGE)
// 			cancel_delayed_work(&chip->conn_therm_work);
// 	}

// 	return 0;
// }

// #define CHG_DETECT_CONN_THERM_US		10000000	/* 10sec */
// static void smblib_conn_therm_work(struct work_struct *work)
// {
// 	struct smb1351_charger *chip = container_of(work, struct smb1351_charger,
// 						conn_therm_work.work);
// 	union power_supply_propval val;
// 	int wdog_timeout = THERM_REG_RECHECK_DELAY_10S;
// 	static int thermal_status = TEMP_BELOW_RANGE;
// 	int usb_present;
// 	u64 elapsed_us;
// 	static bool enable_charging = true;

// 	pm_stay_awake(chip->dev);
// 	power_supply_get_property(chip->main_psy,
// 		POWER_SUPPLY_PROP_TEMP_MAX, &val);
// 	chip->connector_temp = val.intval * 10;
// 	pr_debug("%s connector_temp = %d\n", __func__, chip->connector_temp);
// 	if (chip->connector_temp >=  CONNECTOR_THERM_MAX) {
// 		pr_debug("chg->connector_temp:%d is over max, return \n",
// 				chip->connector_temp);
// 		goto out;
// 	} else if ((chip->connector_temp >=  CONNECTOR_THERM_TOO_HIG)) {
// 		pr_debug("chg->connector_temp:%d is too hig\n",
// 				chip->connector_temp);
// 		thermal_status = TEMP_ABOVE_RANGE;
// 		wdog_timeout = THERM_REG_RECHECK_DELAY_1S;
// 	} else if (chip->connector_temp >=  CONNECTOR_THERM_HIG) {
// 		if ((thermal_status == TEMP_ABOVE_RANGE)  &&
// 			(chip->connector_temp > CONNECTOR_THERM_TOO_HIG - 100)) {
// 			pr_debug("chg->connector_temp:%d is warm\n", chip->connector_temp);
// 		} else {
// 			thermal_status = TEMP_ALERT_LEVEL;
// 		}
// 		wdog_timeout = THERM_REG_RECHECK_DELAY_1S;
// 	} else {
// 		if ((thermal_status == TEMP_ABOVE_RANGE)  &&
// 			(chip->connector_temp > CONNECTOR_THERM_TOO_HIG - 100)) {
// 			wdog_timeout = THERM_REG_RECHECK_DELAY_1S;
// 		} else
// 			thermal_status = TEMP_BELOW_RANGE;
// 		wdog_timeout = THERM_REG_RECHECK_DELAY_10S;
// 	}

// 	if (thermal_status != chip->thermal_status) {
// 		chip->thermal_status = thermal_status;
// 		if (thermal_status == TEMP_ABOVE_RANGE) {
// 			enable_charging = false;
// 			pr_debug("connect temp is too hot, disable vbus\n");
// 			charger_manager_enable_charging(chip->chg_consumer,
// 				MAIN_CHARGER, false);
// 			smblib_set_vbus_disable(chip, true);
// 		} else {
// 			pr_debug("connect temp normal recovery vbus\n");
// 			if (enable_charging == false) {
// 				charger_manager_enable_charging(chip->chg_consumer,
// 					MAIN_CHARGER, true);
// 				enable_charging = true;
// 			}
// 			smblib_set_vbus_disable(chip, false);
// 		}
// 	}

// 	elapsed_us = ktime_us_delta(ktime_get(), chip->entry_time);
// 	if (elapsed_us < CHG_DETECT_CONN_THERM_US)
// 		wdog_timeout = THERM_REG_RECHECK_DELAY_200MS;

// out:
// 	pm_relax(chip->dev);
// 	power_supply_get_property(chip->usb_psy,
// 		POWER_SUPPLY_PROP_PRESENT, &val);
// 	usb_present = val.intval;
// 	if (!usb_present && thermal_status == TEMP_BELOW_RANGE) {
// 		pr_debug("usb is disconnet cancel the connect them work\n");
// 		return;
// 	} else
// 		schedule_delayed_work(&chip->conn_therm_work,
// 				msecs_to_jiffies(wdog_timeout));

// 	return;
// }

static int smb1351_set_suspend_to_iic(struct charger_device *chg_dev, bool suspend)
{
	int rc;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure for volatile rc = %d\n", rc);
	}

	rc = smb1351_masked_write(chip,
		VARIOUS_FUNC_REG, SUSPEND_MODE_CTRL_BIT,
		SUSPEND_MODE_CTRL_BY_I2C);
	if (rc)
		pr_err("Couldn't set VARIOUS_FUNC_REG rc=%d\n", rc);

	if (suspend) {
		rc = smb1351_masked_write(chip, CMD_INPUT_LIMIT_REG,
			CMD_SUSPEND_MODE_BIT, CMD_SUSPEND_MODE_BIT);
		if (rc)
			pr_err("Couldn't set CMD_INPUT_LIMIT_REG rc=%d\n", rc);
	} else {
		rc = smb1351_masked_write(chip, CMD_INPUT_LIMIT_REG,
			CMD_SUSPEND_MODE_BIT, 0);
		if (rc)
			pr_err("Couldn't set CMD_INPUT_LIMIT_REG rc=%d\n", rc);
	}
	pr_debug("---set1 suspend en : %d\n", suspend);
	return 0;
}

#define IRQ_LATCHED_MASK	0x02
#define IRQ_STATUS_MASK		0x01
#define BITS_PER_IRQ		2
static irqreturn_t smb1351_chg_stat_handler(int irq, void *dev_id)
{
	struct smb1351_charger *chip = dev_id;

	int i, j;
	u8 triggered;
	u8 changed;
	u8 rt_stat, prev_rt_stat;
	int rc;
	int handler_count = 0;
	union power_supply_propval val;

	mutex_lock(&chip->irq_complete);

	power_supply_get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	pr_info(" %s read vbus : %d. otg_enable:%d. \n",
			__func__, val.intval, chip->otg_enable);
//1409 520 064
// 	if (val.intval < VBUS_PLUG_OUT_THRESHOLD) {
// 		/*set smb_susp pin low*/
// //		gpio_direction_output(chip->suspend_gpio, 0);
// 	    pr_info("vbus is low, set suspend to 0,return. \n");
// 		chip->shutdown_status = true;
// 		mutex_unlock(&chip->irq_complete);
// 		return IRQ_HANDLED;
// 	} else {
		/*set smb_susp pin high*/
	//	gpio_direction_output(chip->suspend_gpio, 1);
		if (chip->otg_enable) {
//			smb1351_set_suspend_to_iic(chip->chg_dev, true);
			mutex_unlock(&chip->irq_complete);
			return IRQ_HANDLED;
		} else {
//			smb1351_set_suspend_to_iic(chip->chg_dev, false);
			chip->shutdown_status = false;
		}
//	}

	chip->irq_waiting = true;
	if (!chip->resume_completed) {
		pr_debug("IRQ triggered before device-resume\n");
		disable_irq_nosync(irq);
		mutex_unlock(&chip->irq_complete);
		return IRQ_HANDLED;
	}
	chip->irq_waiting = false;

	for (i = 0; i < ARRAY_SIZE(handlers); i++) {
		rc = smb1351_read_reg(chip, handlers[i].stat_reg,
						&handlers[i].val);
		if (rc) {
			pr_err("Couldn't read %d rc = %d\n",
					handlers[i].stat_reg, rc);
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(handlers[i].irq_info); j++) {
			triggered = handlers[i].val
			       & (IRQ_LATCHED_MASK << (j * BITS_PER_IRQ));
			rt_stat = handlers[i].val
				& (IRQ_STATUS_MASK << (j * BITS_PER_IRQ));
			prev_rt_stat = handlers[i].prev_val
				& (IRQ_STATUS_MASK << (j * BITS_PER_IRQ));
			changed = prev_rt_stat ^ rt_stat;

			if (triggered || changed)
				rt_stat ? handlers[i].irq_info[j].high++ :
						handlers[i].irq_info[j].low++;

			if ((triggered || changed)
				&& handlers[i].irq_info[j].smb_irq != NULL) {
				handler_count++;
				rc = handlers[i].irq_info[j].smb_irq(chip,
								rt_stat);
				if (rc)
					pr_err("Couldn't handle %d irq for reg 0x%02x rc = %d\n",
						j, handlers[i].stat_reg, rc);
			}
		}
		handlers[i].prev_val = handlers[i].val;
	}

	mutex_unlock(&chip->irq_complete);

	return IRQ_HANDLED;
}

#define LAST_CNFG_REG	0x16
static int show_cnfg_regs(struct seq_file *m, void *data)
{
	struct smb1351_charger *chip = m->private;
	int rc;
	u8 reg;
	u8 addr;

	for (addr = 0; addr <= LAST_CNFG_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (!rc)
			seq_printf(m, "0x%02x = 0x%02x\n", addr, reg);
	}

	return 0;
}

static int cnfg_debugfs_open(struct inode *inode, struct file *file)
{
	struct smb1351_charger *chip = inode->i_private;

	return single_open(file, show_cnfg_regs, chip);
}

static const struct file_operations cnfg_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= cnfg_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#define FIRST_CMD_REG	0x30
#define LAST_CMD_REG	0x34
static int show_cmd_regs(struct seq_file *m, void *data)
{
	struct smb1351_charger *chip = m->private;
	int rc;
	u8 reg;
	u8 addr;

	for (addr = FIRST_CMD_REG; addr <= LAST_CMD_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (!rc)
			seq_printf(m, "0x%02x = 0x%02x\n", addr, reg);
	}

	return 0;
}

static int cmd_debugfs_open(struct inode *inode, struct file *file)
{
	struct smb1351_charger *chip = inode->i_private;

	return single_open(file, show_cmd_regs, chip);
}

static const struct file_operations cmd_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= cmd_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#define FIRST_STATUS_REG	0x36
#define LAST_STATUS_REG		0x3F
static int show_status_regs(struct seq_file *m, void *data)
{
	struct smb1351_charger *chip = m->private;
	int rc;
	u8 reg;
	u8 addr;

	for (addr = FIRST_STATUS_REG; addr <= LAST_STATUS_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (!rc)
			seq_printf(m, "0x%02x = 0x%02x\n", addr, reg);
	}

	return 0;
}

static int status_debugfs_open(struct inode *inode, struct file *file)
{
	struct smb1351_charger *chip = inode->i_private;

	return single_open(file, show_status_regs, chip);
}

static const struct file_operations status_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= status_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int show_irq_count(struct seq_file *m, void *data)
{
	int i, j, total = 0;

	for (i = 0; i < ARRAY_SIZE(handlers); i++)
		for (j = 0; j < 4; j++) {
			seq_printf(m, "%s=%d\t(high=%d low=%d)\n",
						handlers[i].irq_info[j].name,
						handlers[i].irq_info[j].high
						+ handlers[i].irq_info[j].low,
						handlers[i].irq_info[j].high,
						handlers[i].irq_info[j].low);
			total += (handlers[i].irq_info[j].high
					+ handlers[i].irq_info[j].low);
		}

	seq_printf(m, "\n\tTotal = %d\n", total);

	return 0;
}

static int irq_count_debugfs_open(struct inode *inode, struct file *file)
{
	struct smb1351_charger *chip = inode->i_private;

	return single_open(file, show_irq_count, chip);
}

static const struct file_operations irq_count_debugfs_ops = {
	.owner		= THIS_MODULE,
	.open		= irq_count_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int get_reg(void *data, u64 *val)
{
	struct smb1351_charger *chip = data;
	int rc;
	u8 temp;

	rc = smb1351_read_reg(chip, chip->peek_poke_address, &temp);
	if (rc) {
		pr_err("Couldn't read reg %x rc = %d\n",
			chip->peek_poke_address, rc);
		return -EAGAIN;
	}
	*val = temp;
	return 0;
}

static int set_reg(void *data, u64 val)
{
	struct smb1351_charger *chip = data;
	int rc = 0;
	u8 temp = 0;

	temp = (u8) val;
	rc = smb1351_write_reg(chip, chip->peek_poke_address, temp);
	if (rc) {
		pr_err("Couldn't write 0x%02x to 0x%02x rc= %d\n",
			temp, chip->peek_poke_address, rc);
		return -EAGAIN;
	}
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(poke_poke_debug_ops, get_reg, set_reg, "0x%02llx\n");

static int force_irq_set(void *data, u64 val)
{
	struct smb1351_charger *chip = data;

	smb1351_chg_stat_handler(chip->client->irq, data);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(force_irq_ops, NULL, force_irq_set, "0x%02llx\n");

void dump_regs(struct smb1351_charger *chip)
{
	int rc;
	u8 reg;
	u8 addr;

	for (addr = 0; addr <= LAST_CNFG_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (rc)
			pr_err("Couldn't read 0x%02x rc = %d\n", addr, rc);
		else
			pr_err("0x%02x = 0x%02x\n", addr, reg);
	}

	for (addr = FIRST_STATUS_REG; addr <= LAST_STATUS_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (rc)
			pr_err("Couldn't read 0x%02x rc = %d\n", addr, rc);
		else
			pr_err("0x%02x = 0x%02x\n", addr, reg);
	}

	for (addr = FIRST_CMD_REG; addr <= LAST_CMD_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (rc)
			pr_err("Couldn't read 0x%02x rc = %d\n", addr, rc);
		else
			pr_err("0x%02x = 0x%02x\n", addr, reg);
	}

	for (addr = IRQ_A_REG; addr <= IRQ_H_REG; addr++) {
		rc = smb1351_read_reg(chip, addr, &reg);
		if (rc)
			pr_err("Couldn't read 0x%02x rc = %d\n", addr, rc);
		else
			pr_err("0x%02x = 0x%02x\n", addr, reg);
	}
}

static int smb1351_parse_dt(struct smb1351_charger *chip)
{
	struct smb1351_desc *desc = NULL;
	struct device_node *np = chip->dev->of_node;

	if (!np) {
		pr_err("device tree info. missing\n");
		return -EINVAL;
	}

	chip->desc = &smb1351_default_desc;

	desc = devm_kzalloc(chip->dev, sizeof(struct smb1351_desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	memcpy(desc, &smb1351_default_desc, sizeof(struct smb1351_desc));

	/*
	 * For dual charger, one is primary_chg;
	 * another one will be secondary_chg
	 */
	if (of_property_read_string(np, "charger_name",
				&desc->chg_dev_name) < 0)
		dev_err(chip->dev, "%s: no charger name\n", __func__);

	if (of_property_read_string(np, "alias_name", &desc->alias_name) < 0)
		dev_err(chip->dev, "%s: no alias name\n", __func__);

	if (of_property_read_u32(np, "ichg", &desc->ichg) < 0)
		dev_err(chip->dev, "%s: no ichg\n", __func__);

	if (of_property_read_u32(np, "mivr", &desc->mivr) < 0)
		dev_err(chip->dev, "%s: no mivr\n", __func__);

	if (of_property_read_u32(np, "cv", &desc->cv) < 0)
		dev_err(chip->dev, "%s: no cv\n", __func__);

	if (of_property_read_u32(np, "ieoc", &desc->ieoc) < 0)
		dev_err(chip->dev, "%s: no ieoc\n", __func__);

	if (of_property_read_u32(np, "safety_timer", &desc->safety_timer) < 0)
		dev_err(chip->dev, "%s: no safety timer\n", __func__);

	desc->en_te = of_property_read_bool(np, "en_te");
	desc->en_wdt = of_property_read_bool(np, "en_wdt");
	desc->en_st = of_property_read_bool(np, "en_st");

	chip->desc = desc;
	chip->chg_props.alias_name = chip->desc->alias_name;
	dev_info(chip->dev, "%s: chg_name:%s alias:%s\n", __func__,
			chip->desc->chg_dev_name, chip->chg_props.alias_name);

	chip->bc12_supported = of_property_read_bool(np, "qcom,bc12_supported");

	chip->disable_apsd = of_property_read_bool(np, "qcom,disable-apsd");

	chip->force_hvdcp_2p0 = of_property_read_bool(np,
					"qcom,force-hvdcp-2p0");

	chip->iterm_disabled = of_property_read_bool(np,
					"qcom,iterm-disabled");

	if (of_property_read_u32(np, "qcom,recharge-mv",
						&chip->recharge_mv) < 0)
		dev_err(chip->dev, "%s: no recharge-mv\n", __func__);

	chip->recharge_disabled = of_property_read_bool(np,
					"qcom,recharge-disabled");

	chip->suspend_gpio = of_get_named_gpio(np, "qcom,smb1351_susp", 0);
	if ((!gpio_is_valid(chip->suspend_gpio)))
		dev_err(chip->dev, "%s: no suspend_gpio\n", __func__);

	chip->connect_therm_gpio = of_get_named_gpio(np, "mi,connect_therm", 0);
	if ((!gpio_is_valid(chip->connect_therm_gpio)))
		dev_err(chip->dev, "%s: no connect_therm_gpio\n", __func__);

	return 0;
}

static int create_debugfs_entries(struct smb1351_charger *chip)
{
	struct dentry *ent;

	chip->debug_root = debugfs_create_dir("smb1351", NULL);
	if (!chip->debug_root) {
		pr_err("Couldn't create debug dir\n");
	} else {
		ent = debugfs_create_file("config_registers", S_IFREG | S_IRUGO,
					  chip->debug_root, chip,
					  &cnfg_debugfs_ops);
		if (!ent)
			pr_err("Couldn't create cnfg debug file\n");

		ent = debugfs_create_file("status_registers", S_IFREG | S_IRUGO,
					  chip->debug_root, chip,
					  &status_debugfs_ops);
		if (!ent)
			pr_err("Couldn't create status debug file\n");

		ent = debugfs_create_file("cmd_registers", S_IFREG | S_IRUGO,
					  chip->debug_root, chip,
					  &cmd_debugfs_ops);
		if (!ent)
			pr_err("Couldn't create cmd debug file\n");

		ent = debugfs_create_x32("address", S_IFREG | S_IWUSR | S_IRUGO,
					  chip->debug_root,
					  &(chip->peek_poke_address));
		if (!ent)
			pr_err("Couldn't create address debug file\n");

		ent = debugfs_create_file("data", S_IFREG | S_IWUSR | S_IRUGO,
					  chip->debug_root, chip,
					  &poke_poke_debug_ops);
		if (!ent)
			pr_err("Couldn't create data debug file\n");

	}
	return 0;
}
static int smb1351_get_voltage_now(struct smb1351_charger *chip, union power_supply_propval *val)
{
	int rc;
	u8 reg;

	if (chip->shutdown_status == true) {
		val->intval = -1;
		return 0;
	}

	rc = smb1351_read_reg(chip, STATUS_2_REG, &reg);
	if (rc < 0) {
		pr_err("Couldn't read STATUS_2_REG rc=%d\n", rc);
		return rc;
	}


	val->intval = 3500 + (reg & STATUS_FLOAT_VOLTAGE_MASK) * 20;

	return rc;
}

static int smb1351_get_input_current_now(struct smb1351_charger *chip, union power_supply_propval *val)
{
	val->intval = chip->chg_current_set * 1000;
	return 0;
}

static int smb1351_get_current_now(struct smb1351_charger *chip, union power_supply_propval *val)
{
	int rc, i;
	u8 reg;
	bool fast_chg_status;

	if (chip->shutdown_status == true) {
		val->intval = -1;
		return 0;
	}

	rc = smb1351_read_reg(chip, STATUS_3_REG, &reg);
	if (rc < 0) {
		pr_err("Couldn't read STATUS_3_REG rc=%d\n", rc);
		return rc;
	}

	fast_chg_status = reg & STATUS_CHG_BIT;
	if (fast_chg_status) {
		i = reg & STATUS_FAST_CHG_CURRENT_MASK;
		val->intval = fast_chg_current[i] * 1000;
	} else {
		i = (reg & STATUS_PRECHG_CURRENT_MASK) >> 4;
		val->intval = pre_chg_current[2+i] * 1000;
		if (chip->chg_current_set == 0)
		val->intval = 0;
	}
	pr_err("STATUS_3_REG：0x%02x, return current now= %d\n", reg, val->intval);
	return rc;
}

static enum power_supply_property smb1351_parallel_properties[] = {
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VBUS_DISABLE,
};

static int smb1351_parallel_get_property(struct power_supply *psy,
			enum power_supply_property prop,
			union power_supply_propval *val)
{
	struct smb1351_charger *chip = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "smb1351";
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc = smb1351_get_voltage_now(chip, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_NOW:
		rc = smb1351_get_input_current_now(chip, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		rc = smb1351_get_current_now(chip, val);
		break;
	case POWER_SUPPLY_PROP_VBUS_DISABLE:
		val->intval = chip->vbus_disable;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int smb1351_parallel_set_property(struct power_supply *psy,
	enum power_supply_property psp, const union power_supply_propval *val)
{
	struct smb1351_charger *chip = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_VBUS_DISABLE:
		smblib_set_vbus_disable(chip, !!val->intval);
		return 0;
	default:
		return -EINVAL;
	}

	return 0;
}
static int smb1351_parallel_is_writeable(struct power_supply *psy,
				       enum power_supply_property prop)
{
	int rc;

	switch (prop) {
	case POWER_SUPPLY_PROP_VBUS_DISABLE:
		rc = 1;
		break;
	default:
		rc = 0;
		break;
	}
	return rc;
}

static int smb1351_enable_charging(struct charger_device *chg_dev, bool en)
{
	int rc = 0;
	u8 reg = 0, mask = 0;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	pr_err("smb1351 enable status = %d\n", en);
	// if (!chip->chip_enable && en) {
	// 	pr_err("chip is not enable, return.\n");
	// 	return 0;
	// }

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
		return rc;
	}

	/* setup defaults for CHG_PIN_EN_CTRL_REG */
	mask = EN_PIN_CTRL_MASK | USBCS_CTRL_BIT | CHG_ERR_BIT |
			APSD_DONE_BIT | LED_BLINK_FUNC_BIT;
	reg = EN_BY_I2C_0_DISABLE | USBCS_CTRL_BY_I2C | CHG_ERR_BIT |
			APSD_DONE_BIT | LED_BLINK_FUNC_BIT;
	rc = smb1351_masked_write(chip, CHG_PIN_EN_CTRL_REG, mask, reg);
	if (rc) {
		pr_err("Couldn't set CHG_PIN_EN_CTRL_REG rc=%d\n", rc);
		return rc;
	}

	rc = smb1351_masked_write(chip, CMD_CHG_REG, CMD_CHG_EN_BIT,
					en ? CMD_CHG_ENABLE : 0);
	if (rc)
		pr_err("Couldn't disable charging, rc=%d\n", rc);

	return rc;
}

static int smb1351_is_charging_enabled(struct charger_device *chg_dev, bool *en)
{
	int rc = 0;
	u8 reg = 0;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	rc = smb1351_read_reg(chip, CMD_CHG_REG, &reg);
	if (rc) {
		pr_err("Couldn't get chg_reg, rc=%d\n", rc);
		return rc;
	}

	*en = (reg & CMD_CHG_EN_BIT) ? true : false;

	pr_err("charging is enabled %d, CMD_CHG_REG reg: 0x%x. \n", *en, reg);
	return 0;
}

static int smb1351_is_charging_done(struct charger_device *chg_dev, bool *done)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	*done = chip->batt_full;
#ifdef CONFIG_MTK_ENG_BUILD
	pr_err("charging is %s\n", chip->batt_full ? "done" : "not done");
#endif
	return 0;
}

static int smb1351_is_chip_enabled(struct charger_device *chg_dev, bool *en)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	*en = chip->chip_enable;

	pr_err("smb1351_is_chip_enabled : %d\n", *en);
	return 0;
}

static int smb1351_enable_safety_timer(struct charger_device *chg_dev, bool en)
{
	int ret = 0;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	ret = smb1351_enable_volatile_writes(chip);
	if (ret) {
		pr_err("Couldn't configure volatile writes ret=%d\n", ret);
		return ret;
	}

	ret = smb1351_masked_write(chip, VARIOUS_FUNC_3_REG,
					SAFETY_TIMER_EN_MASK,
					en ? 0 : SAFETY_TIMER_EN_MASK);
	if (ret)
		pr_err("Couldn't set safety timer, rc=%d\n", ret);

	return ret;
}

static int smb1351_enable_chip(struct charger_device *chg_dev, bool en)
{
	int rc = 0;
	u8 reg, mask = 0;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	pr_err("enable chip %d\n", en);

	chip->chip_enable = en;
	if (!en)
		return 0;

	rc = smb_chip_get_version(chip);
	if (rc) {
		pr_err("Couldn't get version rc = %d\n", rc);
		return rc;
	}

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure for volatile rc = %d\n", rc);
		return rc;
	}

	/*set Minimum system voltage to 3.15V*/
	rc = smb1351_masked_write(chip, THERM_A_CTRL_REG,
		MIN_SYS_VOLTAGE_MASK, 0);
	if (rc) {
		pr_err("Couldn't set THERM_A_CTRL_REG rc=%d\n", rc);
		return rc;
	}

	/*set suspend mode by iic*/
	rc = smb1351_masked_write(chip, VARIOUS_FUNC_REG,
		SUSPEND_MODE_CTRL_BIT | AICL_EN_BIT,
		SUSPEND_MODE_CTRL_BY_I2C | AICL_EN_BIT);
	if (rc) {
		pr_err("Couldn't set VARIOUS_FUNC_REG rc=%d\n", rc);
		return rc;
	}

	/*set irq mode to static irq*/
	rc = smb1351_masked_write(chip, VARIOUS_FUNC_2_REG,
				STAT_PIN_CONFIG_BIT, STAT_PIN_CONFIG_BIT);
	if (rc) {
		pr_err("Couldn't set VARIOUS_FUNC_2_REG rc=%d\n", rc);
		return rc;
	}

	rc = smb1351_masked_write(chip, OTG_MODE_POWER_OPTIONS_REG,
			ADAPTER_CONFIG_MASK, ADAPTER_CONFIG_MASK);
	if (rc) {
		pr_err("Couldn't set OTG_MODE_POWER_OPTIONS_REG rc=%d\n",  rc);
		return rc;
	}

	/* set chg_config: 5-9 V, as pm660 only support 5-9V */
	reg = CHG_CONFIG;
	rc = smb1351_masked_write(chip, FLEXCHARGER_REG,
			CHG_CONFIG_MASK, reg);
	if (rc) {
		pr_err("Couldn't set FLEXCHARGER_REG rc=%d\n",  rc);
		return rc;
	}

	/* set recharge-threshold and enable auto recharge */
	if (chip->recharge_mv != -EINVAL) {
		reg = AUTO_RECHG_ENABLE;
		if (chip->recharge_mv > 50)
			reg |= AUTO_RECHG_TH_100MV;
		else
			reg |= AUTO_RECHG_TH_50MV;

		rc = smb1351_masked_write(chip, CHG_CTRL_REG,
				AUTO_RECHG_BIT |
				AUTO_RECHG_TH_BIT, reg);
		if (rc) {
			pr_err("Couldn't set rechg-cfg rc = %d\n", rc);
			return rc;
		}
	}

	/* setup defaults for CHG_PIN_EN_CTRL_REG */
	mask = EN_PIN_CTRL_MASK | USBCS_CTRL_BIT | CHG_ERR_BIT |
		APSD_DONE_BIT | LED_BLINK_FUNC_BIT;
	reg = EN_BY_I2C_0_DISABLE | USBCS_CTRL_BY_I2C | CHG_ERR_BIT |
		APSD_DONE_BIT | LED_BLINK_FUNC_BIT;
	rc = smb1351_masked_write(chip, CHG_PIN_EN_CTRL_REG, mask, reg);
	if (rc) {
		pr_err("Couldn't set CHG_PIN_EN_CTRL_REG rc=%d\n", rc);
		return rc;
	}
	pr_err("smb1351_enable_chip end...\n");
	return rc;
}

static int smb1351_dump_register(struct charger_device *chg_dev)
{
	int ret = 0;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	dump_regs(chip);
	return ret;
}

static int smb1351_get_usbchg_current(struct charger_device *chg_dev, u32 *uA)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	u8 reg = 0;
	int rc = 0, i = 0;
#ifdef CONFIG_MTK_ENG_BUILD
	pr_err("get usbchg current.\n");
#endif

	rc = smb1351_read_reg(chip, CHG_CURRENT_CTRL_REG, &reg);
	if (rc) {
		pr_err("Couldn't read STATUS_7 rc = %d\n", rc);
	}

	i = reg & AC_INPUT_CURRENT_LIMIT_MASK;
	*uA = usb_chg_current[i] * 1000;

	return rc;
}

static int smb1351_set_usbchg_current(struct charger_device *chg_dev, u32 uA)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	int i, rc = 0;
	static int prev_current = -1;
	u8 reg = 0, mask = 0;
	u32 current_ma = uA / 1000;

#ifdef CONFIG_MTK_ENG_BUILD
	pr_err("USB current_ma = %d\n", current_ma);
#endif

	if (chip->chg_autonomous_mode) {
		pr_debug("Charger in autonomous mode\n");
		return 0;
	}

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
		return rc;
	}

	if (current_ma > SUSPEND_CURRENT_MA &&
			current_ma < USB2_MIN_CURRENT_MA)
		current_ma = USB2_MIN_CURRENT_MA;

	if (current_ma == USB2_MIN_CURRENT_MA) {
		/* USB 2.0 - 100mA */
		reg = CMD_USB_2_MODE | CMD_USB_100_MODE;
	} else if (current_ma == USB3_MIN_CURRENT_MA) {
		/* USB 3.0 - 150mA */
		reg = CMD_USB_3_MODE | CMD_USB_100_MODE;
	} else if (current_ma == USB2_MAX_CURRENT_MA) {
		/* USB 2.0 - 500mA */
		reg = CMD_USB_2_MODE | CMD_USB_500_MODE;
	} else if (current_ma == USB3_MAX_CURRENT_MA) {
		/* USB 3.0 - 900mA */
		reg = CMD_USB_3_MODE | CMD_USB_500_MODE;
	} else if (current_ma > USB2_MAX_CURRENT_MA) {
		/* HC mode  - if none of the above */
		reg = CMD_USB_AC_MODE;

		for (i = ARRAY_SIZE(usb_chg_current) - 1; i >= 0; i--) {
			if (usb_chg_current[i] <= current_ma)
				break;
		}
		if (i < 0)
			i = 0;
		rc = smb1351_masked_write(chip, CHG_CURRENT_CTRL_REG,
						AC_INPUT_CURRENT_LIMIT_MASK, i);
		if (rc) {
			pr_err("Couldn't set input mA rc=%d\n", rc);
			return rc;
		}
	}
	/* control input current mode by command */
	reg |= CMD_INPUT_CURRENT_MODE_CMD;
	mask = CMD_INPUT_CURRENT_MODE_BIT | CMD_USB_2_3_SEL_BIT |
		CMD_USB_1_5_AC_CTRL_MASK;
	if (prev_current < current_ma) {
		pr_err("input current is not same ,should rerun aicl,prev:%d current:%d\n", prev_current, current_ma);
		smb1351_rerun_aicl(chip);
	}
	prev_current = current_ma;
	rc = smb1351_masked_write(chip, CMD_INPUT_LIMIT_REG, mask, reg);
	if (rc) {
		pr_err("Couldn't set charging mode rc = %d\n", rc);
		return rc;
	}

	return rc;
}

static int smb1351_get_fastchg_current(struct charger_device *chg_dev, u32 *uA)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	bool fast_chg_status;
	u8 reg, i;
	int rc = 0;

#ifdef CONFIG_MTK_ENG_BUILD
	pr_err("get fastchg current\n");
#endif
	rc = smb1351_read_reg(chip, STATUS_3_REG, &reg);
	if (rc < 0) {
		pr_err("Couldn't read STATUS_3_REG rc=%d\n", rc);
		return rc;
	}

	fast_chg_status = reg & STATUS_CHG_BIT;
	if (fast_chg_status) {
		i = reg & STATUS_FAST_CHG_CURRENT_MASK;
		*uA = fast_chg_current[i] * 1000;
	} else {
		i = (reg & STATUS_PRECHG_CURRENT_MASK) >> 4;
		*uA = pre_chg_current[2+i] * 1000;
	}
	return 0;
}

static int smb1351_set_fastchg_current(struct charger_device *chg_dev, u32 uA)
{
	int rc = 0, i = 0;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	bool is_pre_chg = false;

	chip->chg_current_set = uA / 1000;

#ifdef CONFIG_MTK_ENG_BUILD
	pr_err("fastchg current mA=%d \n", chip->chg_current_set);
#endif

	if ((chip->chg_current_set < SMB1351_CHG_PRE_MIN_MA) ||
		(chip->chg_current_set > SMB1351_CHG_FAST_MAX_MA)) {
#ifdef CONFIG_MTK_ENG_BUILD
		pr_err("bad pre_fastchg current mA=%d asked to set\n",
					chip->chg_current_set);
#endif
		return -EINVAL;
	}

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
		return rc;
	}

	/*
	 * fast chg current could not support less than 1000mA
	 * use pre chg to instead for the parallel charging
	 */
	if (chip->chg_current_set < SMB1351_CHG_FAST_MIN_MA) {
		is_pre_chg = true;
		pr_debug("is_pre_chg true, current is %d\n", chip->chg_current_set);
	}

	if (is_pre_chg) {
		/* set prechg current */
		for (i = ARRAY_SIZE(pre_chg_current) - 1; i >= 0; i--) {
			if (pre_chg_current[i] <= chip->chg_current_set)
				break;
		}
		if (i < 0)
			i = 0;
		if (i == 0)
			i = 0x7 << SMB1351_CHG_PRE_SHIFT;
		else if (i == 1)
			i = 0x6 << SMB1351_CHG_PRE_SHIFT;
		else
			i = (i - 2) << SMB1351_CHG_PRE_SHIFT;

		pr_debug("prechg setting %02x\n", i);

		rc = smb1351_masked_write(chip, CHG_OTH_CURRENT_CTRL_REG,
				PRECHG_CURRENT_MASK, i);
		if (rc)
			pr_err("Couldn't write CHG_OTH_CURRENT_CTRL_REG rc=%d\n",
									rc);
		return smb1351_masked_write(chip, VARIOUS_FUNC_2_REG,
				PRECHG_TO_FASTCHG_BIT, PRECHG_TO_FASTCHG_BIT);
	} else {
		/* set fastchg current */
		for (i = ARRAY_SIZE(fast_chg_current) - 1; i >= 0; i--) {
			if (fast_chg_current[i] <= chip->chg_current_set)
				break;
		}
		if (i < 0)
			i = 0;
		i = i << SMB1351_CHG_FAST_SHIFT;
		pr_debug("fastchg limit=%d setting %02x\n",
					chip->fastchg_current_max_ma, i);

		/* make sure pre chg mode is disabled */
		rc = smb1351_masked_write(chip, VARIOUS_FUNC_2_REG,
					PRECHG_TO_FASTCHG_BIT, 0);
		if (rc)
			pr_err("Couldn't write VARIOUS_FUNC_2_REG rc=%d\n", rc);

		return smb1351_masked_write(chip, CHG_CURRENT_CTRL_REG,
					FAST_CHG_CURRENT_MASK, i);
	}

	return rc;
}

#ifdef CONFIG_MTK_ENG_BUILD
static int smb1351_get_min_ichg(struct charger_device *chg_dev, u32 *uA)
{
	*uA = pre_chg_current[2] * 1000;
	pr_info("get min ichg: %d \n", *uA);
	return 0;
}
#endif

static int smb1351_plug_out(struct charger_device *chg_dev)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
//	struct charger_manager *cm = chip->chg_consumer->cm;
//	int rc;
	pr_err("%s \n", __func__);

	chip->hvdcp_type = HVDCP_NULL;
	hvdcp_type_tmp = HVDCP_NULL;
	hvdcp_det_count = 0;
	usb_det_flag = 0;
//	cm->hvdcp_type = HVDCP_NULL;
	chip->hvdcp_dpdm_status = 0;
	chip->chip_enable = 1;
//	cm->wireless_status = WIRELESS_NULL;
	chip->rerun_apsd_count = 0;
//	_smb1351_enable_hvdcp_det(chip, true);
	queue_delayed_work(system_power_efficient_wq, &chip->enable_hvdcp_work, msecs_to_jiffies(3000));
	cancel_delayed_work_sync(&chip->hvdcp_500_mode_check_work);
	/* Disable SW conn therm Regulation */
//	rc = smblib_set_sw_conn_therm_regulation(chip, false);
//	if (rc < 0)
//		pr_err("Couldn't stop SW conn therm rc=%d\n", rc);

	return 0;
}

static int smb1351_plug_in(struct charger_device *chg_dev)
{
//	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
//	int rc;
	pr_err("%s \n", __func__);
	/* Enable SW conn therm Regulation */
	// rc = smblib_set_sw_conn_therm_regulation(chip, true);
//	if (rc < 0)
//		pr_err("Couldn't start SW conn therm rc=%d\n", rc);
	return 0;
}

static int smb1351_get_hvdcp_type(struct charger_device *chg_dev, u32 *type)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	*type = chip->hvdcp_type;
	return 0;
}
static void _smb1351_enable_hvdcp_det(struct smb1351_charger *chip, bool enable)
{
	int rc = 0;

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	if (enable) {
		/* Enable HVDCP */
		rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
			HVDCP_EN_BIT, HVDCP_EN_BIT);
		if (rc) {
			pr_err("Couldn't set HVDCP_BATT_MISSING_CTRL_REG rc=%d\n", rc);
		}
	} else {
		/* Disable HVDCP */
		rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
			HVDCP_EN_BIT, 0);
		if (rc) {
			pr_err("Couldn't set HVDCP_BATT_MISSING_CTRL_REG rc=%d\n", rc);
		}
	}
}
static int smb1351_enable_hvdcp_det(struct charger_device *chg_dev, bool enable)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	pr_info("%s. en = %d\n", __func__, enable);
	_smb1351_enable_hvdcp_det(chip, enable);

	return 0;
}


#define HVDCP3_MAX_COUNT 8
static int smb1351_set_hvdcp_dpdm(struct charger_device *chg_dev)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	int rc;
	int i;
	u8 reg;
	int vbus_diff, sub_count;
	union power_supply_propval val;
	power_supply_get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	vbus_diff = val.intval - 5800;
	vbus_diff = 7000 - 5000;
	sub_count = vbus_diff / 200;
	rc = smb1351_read_reg(chip, VARIOUS_FUNC_3_REG, &reg);
	if (rc) {
		pr_err("Couldn't read CHG_CURRENT_CTRL_REG rc = %d\n", rc);
	}
	pr_err("hvdcp auto increment mode : %s.\n",
			(reg & QC_2P1_AUTO_INCREMENT_MODE_BIT) > 0 ? "enable" : "disable");

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}

	rc = smb1351_masked_write(chip, VARIOUS_FUNC_3_REG,
			QC_2P1_AUTO_INCREMENT_MODE_BIT, 0);

	if (rc)
		pr_err("Couldn't write CMD_HVDCP_REG rc=%d\n", rc);

	pr_err("before sub vbus: %d sub_count: %d\n", val.intval, sub_count);
	// for (i = 0; i <= sub_count; i++) {
	// 	rc = smb1351_masked_write(chip, CMD_HVDCP_REG,
	// 			CMD_HVDCP_DEC_BIT, CMD_HVDCP_DEC_BIT);
	// 	if (rc)
	// 		pr_err("Couldn't write CMD_HVDCP_REG rc=%d\n", rc);
	// 	msleep(80);
	// 	pr_err("QC3 sub count =%d.\n", i);
	// }

	// rc = smb1351_masked_write(chip, VARIOUS_FUNC_3_REG,
	// 		QC_2P1_AUTO_INCREMENT_MODE_BIT, 0x04);
	// msleep(80);
	for (i = 0; i <= sub_count; i++) {
		rc = smb1351_masked_write(chip, CMD_HVDCP_REG,
				CMD_HVDCP_INC_BIT, CMD_HVDCP_INC_BIT);
		if (rc)
			pr_err("Couldn't write CMD_HVDCP_REG rc=%d\n", rc);
		msleep(80);
		pr_err("QC3 count = %d . \n", i);
	}


	chip->hvdcp_dpdm_status = 1;

	return 0;
}
static int smb1351_get_hvdcp_dpdm_status(struct charger_device *chg_dev, bool *status)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);

	if (chip->hvdcp_dpdm_status)
		*status = true;
	else
		*status = false;

	return 0;
}

static int smb1351_check_hv_charging(struct charger_device *chg_dev)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	struct charger_manager *cm = chip->chg_consumer->cm;
	int rc = 0;
	static bool pre_hv_charging_status = true;


	if (pre_hv_charging_status != cm->enable_hv_charging) {
		pr_info("%s: hv charging is %s, pre_status is %d\n", __func__,
			cm->enable_hv_charging ? "enable" : "disable", pre_hv_charging_status);
		pre_hv_charging_status = cm->enable_hv_charging;
	} else {
		return 0;
	}

	if (!cm->enable_hv_charging) {
		if (chip->is_connect) {
			chip->hvdcp_type = HVDCP_NULL;
			chip->hvdcp_dpdm_status = 0;
			chip->chip_enable = 1;
			chip->rerun_apsd_count = 0;
			cm->hvdcp_type = HVDCP_NULL;
			cm->hvdcp_check_count = 0;

			rc = smb1351_enable_volatile_writes(chip);
			if (rc) {
				pr_err("Couldn't configure volatile writes rc=%d\n", rc);
			}

			rc = smb1351_masked_write(chip, CMD_CHG_REG, CMD_CHG_EN_BIT, 0);
			if (rc)
				pr_err("Couldn't disable charging, rc=%d\n", rc);

			/* Disable HVDCP */
			rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
				HVDCP_EN_BIT, 0);
			if (rc) {
				pr_err("Couldn't set HVDCP_BATT_MISSING_CTRL_REG rc=%d\n", rc);
			}
			/* Re-run APSD */
			rerun_apsd(chip);
			chip->is_connect = false;
			pr_info("%s: disable hvdcp\n", __func__);
		}

	} else {
		if (!chip->is_connect && chip->chg_type == STANDARD_CHARGER) {
			rc = smb1351_enable_volatile_writes(chip);
			if (rc) {
				pr_err("Couldn't configure volatile writes rc=%d\n", rc);
			}

			/* Enable HVDCP */
			rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
				HVDCP_EN_BIT, HVDCP_EN_BIT);
			if (rc) {
				pr_err("Couldn't set HVDCP_BATT_MISSING_CTRL_REG rc=%d\n", rc);
			}
			/* Re-run APSD */
			rerun_apsd(chip);

			cm->hvdcp_check_count = 0;
			pr_info("%s: enable hvdcp\n", __func__);
			return 1;
		}
	}

	return 0;
}

bool usb_otg;
static int smb1351_enable_otg(struct charger_device *chg_dev, bool en)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	int rc = 0;
	pr_info("%s. en = %d\n", __func__, en);
	chip->otg_enable = en;
	usb_otg = en;
	if (en) {
		rc = smb1351_masked_write(chip, CMD_CHG_REG, CMD_OTG_EN_BIT,
							CMD_OTG_EN_BIT);
		if (rc)
			pr_err("Couldn't enable  OTG mode rc=%d\n", rc);
	} else {
		rc = smb1351_masked_write(chip, CMD_CHG_REG, CMD_OTG_EN_BIT, 0);
		if (rc)
			pr_err("Couldn't disable OTG mode rc=%d\n", rc);
	}
	return rc;
}

static int smb1351_set_otg_current(struct charger_device *chg_dev, u32 uA)
{
	u8 reg;
	int rc;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	if (uA >= 1800000)
		reg = 0x00;
	else if (uA >= 1000000)
		reg = 0x0C;

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	rc = smb1351_masked_write(chip, 0xA, 0x0C, reg);
	return rc;
}

static int smb1351_get_charger_type(struct charger_device *chg_dev, u32 *type)
{
	u8 reg;
	int rc;
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	rc = smb1351_read_reg(chip, 0x3A, &reg);
	if (rc) {
		pr_err("Couldn't get charger_type  rc=%d\n", rc);
	}
	reg = reg & 0x06;
	*type = reg >> 1;

	return rc;
}

#define MIN_FLOAT_UV		3500000
#define MAX_FLOAT_UV		4460000
#define VFLOAT_STEP_UV		20000

static int smb1351_set_float_voltage(struct charger_device *chg_dev, u32 uV)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	u8 temp;
	int rc = 0;
	int float_max;

	if (cycle_count < 100) {
		float_max = 4460000;
	} else if (cycle_count < 200) {
		float_max = 4440000;
	} else if (cycle_count < 300) {
		float_max = 4420000;
	} else {
		float_max = 4400000;
	}
	if (uV > float_max) {
		uV = float_max;
	}

	pr_err("set float voltage %d cycle_count : %d", uV, cycle_count);

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
		return rc;
	}

	temp = (uV - MIN_FLOAT_UV) / VFLOAT_STEP_UV;
	return smb1351_masked_write(chip, VFLOAT_REG, VFLOAT_MASK, temp);
}

static int smb1351_enable_chg_type_det(struct charger_device *chg_dev, bool en)
{
	struct smb1351_charger *chip = dev_get_drvdata(&chg_dev->dev);
	int i, ret = 0;
	u8 reg, mask = 0;
	const int max_wait_cnt = 200;

	pr_info("%s: en = %d\n", __func__, en);
	/*set smb_susp pin high*/
    if (gpio_is_valid(chip->suspend_gpio))
		gpio_direction_output(chip->suspend_gpio, 1);

	mutex_lock(&chip->chgdet_lock);

	chip->tcpc_attach = en;

	if (!en) {
		chip->chg_type = CHARGER_UNKNOWN;
		chip->hvdcp_type = HVDCP_NULL;
		hvdcp_det_count = 0;
		hvdcp_type_tmp = 0;
		usb_det_flag = 0;
		chip->hvdcp_dpdm_status = 0;
		//_smb1351_enable_hvdcp_det(chip, true);
		queue_delayed_work(system_power_efficient_wq, &chip->enable_hvdcp_work, msecs_to_jiffies(3000));
		chip->mt_chg->usb_desc.type = smblib_apsd_results[chip->chg_type];
		smb1351_psy_chg_type_changed(chip, true);
		smb1351_set_usbsw_state(chip, USBSW_USB);
		chip->bc12_en = false;
		goto out;
	} else {
		cancel_delayed_work_sync(&chip->enable_hvdcp_work);
	}

	if (en && is_meta_mode()) {
		chip->chg_type = STANDARD_HOST;
		return smb1351_psy_chg_type_changed(chip, false);
	}
	for (i = 0; i < max_wait_cnt; i++) {
		if (is_usb_rdy())
			break;
		pr_info("%s: CDP block\n", __func__);
		if (!chip->tcpc_attach) {
			pr_info("%s: plug out", __func__);
			goto out;
		}
		msleep(100);
	}
	if (i == max_wait_cnt)
		pr_err("%s: CDP timeout\n", __func__);
	else
		pr_info("%s: CDP free\n", __func__);

	smb1351_set_usbsw_state(chip, USBSW_CHG);
	msleep(30);

	ret = smb1351_enable_volatile_writes(chip);
	if (ret) {
		pr_err("Couldn't configure volatile writes rc=%d\n", ret);
		goto out;
	}

	/* setup defaults for CHG_PIN_EN_CTRL_REG */
	mask = EN_PIN_CTRL_MASK | USBCS_CTRL_BIT | CHG_ERR_BIT |
		APSD_DONE_BIT | LED_BLINK_FUNC_BIT;
	reg = EN_BY_I2C_0_DISABLE | USBCS_CTRL_BY_I2C | CHG_ERR_BIT |
		APSD_DONE_BIT | LED_BLINK_FUNC_BIT;
	ret = smb1351_masked_write(chip, CHG_PIN_EN_CTRL_REG, mask, reg);
	if (ret) {
		pr_err("Couldn't set CHG_PIN_EN_CTRL_REG ret=%d\n", ret);
		goto out;
	}

	/* Enable HVDCP */
	// ret = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
	// 	HVDCP_ADAPTER_SEL_MASK | HVDCP_EN_BIT,
	// 	HVDCP_ADAPTER_SEL_9V | HVDCP_EN_BIT);
	// if (ret)
	// 		pr_err("Couldn't set HVDCP_BATT_MISSING_CTRL_REG rc=%d\n", ret);

	/* Enable APSD */
	ret = smb1351_masked_write(chip, VARIOUS_FUNC_REG, APSD_EN_BIT,
				APSD_EN_BIT);
	if (ret) {
		pr_err("Couldn't set VARIOUS_FUNC_REG ret=%d\n", ret);
		goto out;
	}

	/* Re-run APSD */
	rerun_apsd(chip);
	chip->bc12_en = true;
	chip->rerun_apsd_count = 0;

out:
	mutex_unlock(&chip->chgdet_lock);
	queue_delayed_work(system_power_efficient_wq, &chip->check_type_work, msecs_to_jiffies(5000));
	pr_info("%s: out.\n", __func__);
	if (en)
		dump_regs(chip);
	return ret;
}

static struct charger_ops smb1351_chg_ops = {
	.enable = smb1351_enable_charging,
	.is_enabled = smb1351_is_charging_enabled,
	.is_charging_done = smb1351_is_charging_done,
	.is_chip_enabled = smb1351_is_chip_enabled,
	.enable_safety_timer = smb1351_enable_safety_timer,
	.enable_chip = smb1351_enable_chip,
	.dump_registers = smb1351_dump_register,
	.get_input_current = smb1351_get_usbchg_current,
	.set_input_current = smb1351_set_usbchg_current,
	.get_charging_current = smb1351_get_fastchg_current,
	.set_charging_current = smb1351_set_fastchg_current,
#ifdef CONFIG_MTK_ENG_BUILD
	.get_min_charging_current = smb1351_get_min_ichg,
#endif
	.set_constant_voltage = smb1351_set_float_voltage,
	.set_suspend = smb1351_set_suspend_to_iic,
	.enable_chg_type_det = smb1351_enable_chg_type_det,
	.plug_out = smb1351_plug_out,
	.get_charger_type = smb1351_get_charger_type,
	.get_hvdcp_type = smb1351_get_hvdcp_type,
	.get_hvdcp_dpdm_status = smb1351_get_hvdcp_dpdm_status,
	.set_hvdcp_dpdm = smb1351_set_hvdcp_dpdm,
	.enable_hvdcp_det = smb1351_enable_hvdcp_det,
	.plug_in = smb1351_plug_in,
	.enable_otg = smb1351_enable_otg,
	.set_otg_current = smb1351_set_otg_current,
	.check_hv_charging = smb1351_check_hv_charging,
};

static void smb1351_init_charger_input_range(struct smb1351_charger *chip)
{
	int rc;

	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	rc = smb1351_masked_write(chip, 0x14, 0xC0, 0x80);
	rc = smb1351_masked_write(chip, 0x10, 0x70, 0x0);
	rc = smb1351_masked_write(chip, 0x12, 0x10, 0x10);

}

static void smb1351_delay_init_work(struct work_struct *work)
{
	int rc;
	struct power_supply_config parallel_psy_cfg = {};
	struct smb1351_charger *chip = container_of(work,
			struct smb1351_charger, delay_init_work.work);

	struct i2c_client *client = chip->client;

	/* Get chg type det power supply */
	chip->charger_psy = power_supply_get_by_name("charger");
	if (!chip->charger_psy) {
		dev_err(chip->dev, "%s: get charger power supply failed\n", __func__);
		goto retry;
	}

	/* Get main power supply */
	chip->main_psy = power_supply_get_by_name("main");
	if (!chip->main_psy) {
		dev_err(chip->dev, "%s: get main power supply failed\n", __func__);
		goto retry;
	}

	/* Get usb power supply */
	chip->usb_psy = power_supply_get_by_name("usb");
	if (!chip->usb_psy) {
		dev_err(chip->dev, "%s: get usb power supply failed\n", __func__);
		goto retry;
	}
	chip->mt_chg = power_supply_get_drvdata(chip->usb_psy);

	chip->parallel_psy_d.name = "parallel";
	chip->parallel_psy_d.type = POWER_SUPPLY_TYPE_PARALLEL;
	chip->parallel_psy_d.get_property = smb1351_parallel_get_property;
	chip->parallel_psy_d.set_property = smb1351_parallel_set_property;
	chip->parallel_psy_d.properties = smb1351_parallel_properties;
	chip->parallel_psy_d.property_is_writeable
				= smb1351_parallel_is_writeable;
	chip->parallel_psy_d.num_properties
				= ARRAY_SIZE(smb1351_parallel_properties);

	parallel_psy_cfg.drv_data = chip;
	parallel_psy_cfg.num_supplicants = 0;
	chip->parallel_psy = devm_power_supply_register(chip->dev,
				&chip->parallel_psy_d,
				&parallel_psy_cfg);

	if (IS_ERR(chip->parallel_psy)) {
		pr_err("Couldn't register parallel psy rc=%ld\n",
				PTR_ERR(chip->parallel_psy));
		return;
	}
	/*set switching frequency to 1MHZ*/
	rc = smb1351_masked_write(chip, OTG_AND_TLIM_CONTROL,
		SWITCHING_FREQUENCY_MASK, SWITCHING_FREQUENCY_1MHZ);
	if (rc) {
		pr_err("Couldn't set OTG_AND_TLIM_CONTROL rc=%d\n", rc);
	}
/*enable QC*/
	_smb1351_enable_hvdcp_det(chip, true);
/*enable QC*/

/*set otg current to 1.8A*/
	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	rc = smb1351_masked_write(chip, 0xA, 0x0C, 0x0);
/*set otg current to 1.8A*/

/*disable rerun aicl*/
	rc = smb1351_enable_volatile_writes(chip);
	rc = smb1351_masked_write(chip, VARIOUS_FUNC_REG, AICL_EN_BIT, 0x10);
/*disable rerun aicl*/

/*disable DCD and set apsd timeout*/
	// rc = smb1351_enable_volatile_writes(chip);
	rc = smb1351_masked_write(chip, VARIOUS_FUNC_3_REG,
					0x11, 0x01);
/*disable DCD and set apsd timeout*/

/*disable QC3.0 auto inctement mode*/
	rc = smb1351_masked_write(chip, VARIOUS_FUNC_3_REG,
			0x06, 0x02);
/*disable QC3.0 auto inctement mode*/

/*set QC2.0 9V*/
	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
			HVDCP_ADAPTER_SEL_MASK, 0x40);
	rerun_apsd(chip);
	pr_err("dhx---fix qc 2.0\n");
/*set QC2.0 9V*/
	smb1351_init_charger_input_range(chip);
	chip->resume_completed = true;
	mutex_init(&chip->irq_complete);

	create_debugfs_entries(chip);

	/* charger class register */
	chip->chg_dev = charger_device_register(chip->desc->chg_dev_name,
		&client->dev, chip, &smb1351_chg_ops, &chip->chg_props);
	if (IS_ERR_OR_NULL(chip->chg_dev)) {
		pr_err("%s, register charger device fail!\n", __func__);
		return;
	}

	/* STAT irq configuration */
	if (client->irq) {
		pr_info("%s: registering IRQ: %d\n", __func__, client->irq);
		rc = devm_request_threaded_irq(&client->dev, client->irq, NULL,
				smb1351_chg_stat_handler,
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"smb1351_chg_stat_irq", chip);
		if (rc) {
			pr_err("Failed STAT irq=%d request rc = %d\n",
				client->irq, rc);
			return;
		}
		enable_irq_wake(client->irq);
	}
	chip->chg_consumer = charger_manager_get_by_name(chip->dev,
			"charger_port1");
	if (!chip->chg_consumer) {
			pr_info("%s: get charger consumer device failed\n",
				__func__);
	}

	chip->vbus_disable = false;
	INIT_WORK(&chip->apsd_update_work, apsd_update_work);
	INIT_DELAYED_WORK(&chip->sw_rerun_aicl_work, smb1351_sw_rerun_aicl_work);
	INIT_DELAYED_WORK(&chip->chg_hvdcp_det_work, smb1351_chg_hvdcp_det_work);
	INIT_DELAYED_WORK(&chip->hvdcp_500_mode_check_work, smb1351_hvdcp_500_mode_check_work);
	INIT_DELAYED_WORK(&chip->enable_hvdcp_work, smb1351_enable_hvdcp_work);
	INIT_DELAYED_WORK(&chip->check_type_work, smb1351_check_type_work);
	INIT_DELAYED_WORK(&chip->float_chg_det_work, smb1351_float_chg_det_work);
	//INIT_DELAYED_WORK(&chip->conn_therm_work, smblib_conn_therm_work);
	smb1351_set_suspend_to_iic(chip->chg_dev, false);
	pr_info("smb1351 parallel successfully probed.\n");

#if 0
	{
		int addr_raw, addr_tmp = 0;
		addr_raw = client->addr;
		for (; addr_tmp < 0xff; addr_tmp++) {
			client->addr = addr_tmp;
			smb_chip_get_version(chip);
		}
		client->addr = addr_raw;
		panic("####read i2c done\n");
	}
#endif
	return;
retry:
	queue_delayed_work(system_power_efficient_wq, &chip->delay_init_work,
			msecs_to_jiffies(100));
}

static int smb1351_charger_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int rc;
	struct smb1351_charger *chip;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		pr_err("Couldn't allocate memory\n");
		return -ENOMEM;
	}

	chip->client = client;
	chip->dev = &client->dev;
	chip->parallel_charger = true;
	chip->chip_enable = true;
	chip->hvdcp_type = HVDCP_NULL;
	chip->hvdcp_dpdm_status = 0;
	chip->shutdown_status = false;
	chip->otg_enable = false;
	chip->is_connect  = false;
	chip->thermal_status = TEMP_BELOW_RANGE;
	chip->rerun_apsd_count = 0;

	mutex_init(&chip->chgdet_lock);
	i2c_set_clientdata(client, chip);

	INIT_DELAYED_WORK(&chip->delay_init_work, smb1351_delay_init_work);

	rc = smb1351_parse_dt(chip);
	if (rc) {
		pr_err("Couldn't parse DT nodes rc=%d\n", rc);
		return rc;
	}

    if (gpio_is_valid(chip->suspend_gpio)) {
		rc = gpio_request(chip->suspend_gpio, "smb_suspend_gpio");
		if (rc) {
			dev_err(&client->dev,
			"%s: unable to request smb suspend gpio [%d]\n",
				__func__,
				chip->suspend_gpio);
		}
		rc = gpio_direction_output(chip->suspend_gpio, 0);
		if (rc) {
			dev_err(&client->dev,
				"%s: unable to set direction for smb suspend gpio [%d]\n",
					__func__,
					chip->suspend_gpio);
		}
	}

    if (gpio_is_valid(chip->connect_therm_gpio)) {
		rc = gpio_request(chip->connect_therm_gpio, "connect_therm_gpio");
		if (rc) {
			dev_err(&client->dev,
			"%s: unable to request smb suspend gpio [%d]\n",
				__func__,
				chip->connect_therm_gpio);
		}
		rc = gpio_direction_output(chip->connect_therm_gpio, 0);
		if (rc) {
			dev_err(&client->dev,
				"%s: unable to set direction for smb suspend gpio [%d]\n",
					__func__,
					chip->connect_therm_gpio);
		}
	}

	queue_delayed_work(system_power_efficient_wq, &chip->delay_init_work,
			msecs_to_jiffies(100));
	return 0;
}

static int smb1351_charger_remove(struct i2c_client *client)
{
	struct smb1351_charger *chip = i2c_get_clientdata(client);
	int rc;

	pr_info("%s: remove\n", __func__);

	_smb1351_enable_hvdcp_det(chip, true);
	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
			HVDCP_ADAPTER_SEL_MASK, 0x00);
	devm_free_irq(&client->dev, client->irq, chip);
	// cancel_delayed_work_sync(&chip->conn_therm_work);
	cancel_delayed_work_sync(&chip->float_chg_det_work);
	cancel_delayed_work_sync(&chip->hvdcp_500_mode_check_work);
	cancel_delayed_work_sync(&chip->chg_hvdcp_det_work);
	cancel_work_sync(&chip->apsd_update_work);
    if (gpio_is_valid(chip->suspend_gpio))
		gpio_free(chip->suspend_gpio);
	mutex_destroy(&chip->irq_complete);
	debugfs_remove_recursive(chip->debug_root);
	return 0;
}
static void smb1351_charger_shutdown(struct i2c_client *client)
{
	struct smb1351_charger *chip = i2c_get_clientdata(client);
	int rc;

	pr_info("%s: shutdown\n", __func__);
	smb1351_enable_otg(chip->chg_dev, false);
	_smb1351_enable_hvdcp_det(chip, true);
	rc = smb1351_enable_volatile_writes(chip);
	if (rc) {
		pr_err("Couldn't configure volatile writes rc=%d\n", rc);
	}
	rc = smb1351_masked_write(chip, HVDCP_BATT_MISSING_CTRL_REG,
			HVDCP_ADAPTER_SEL_MASK, 0x00);
	devm_free_irq(&client->dev, client->irq, chip);
	// cancel_delayed_work_sync(&chip->conn_therm_work);
	cancel_delayed_work_sync(&chip->float_chg_det_work);
	cancel_delayed_work_sync(&chip->chg_hvdcp_det_work);
	cancel_work_sync(&chip->apsd_update_work);

    if (gpio_is_valid(chip->suspend_gpio))
		gpio_free(chip->suspend_gpio);
	mutex_destroy(&chip->irq_complete);
	debugfs_remove_recursive(chip->debug_root);
}

static int smb1351_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct smb1351_charger *chip = i2c_get_clientdata(client);

	// cancel_delayed_work_sync(&chip->conn_therm_work);

	/* no suspend resume activities for parallel charger */
	if (chip->parallel_charger)
		return 0;

	mutex_lock(&chip->irq_complete);
	chip->resume_completed = false;
	mutex_unlock(&chip->irq_complete);

	return 0;
}

static int smb1351_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct smb1351_charger *chip = i2c_get_clientdata(client);

	/* no suspend resume activities for parallel charger */
	if (chip->parallel_charger)
		return 0;

	if (chip->irq_waiting) {
		pr_err_ratelimited("Aborting suspend, an interrupt was detected while suspending\n");
		return -EBUSY;
	}
	return 0;
}

static int smb1351_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct smb1351_charger *chip = i2c_get_clientdata(client);
	union power_supply_propval val;
	int usb_present = 0;

	power_supply_get_property(chip->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &val);
	usb_present = val.intval;
	// if (usb_present)
	// 	schedule_delayed_work(&chip->conn_therm_work,
	// 			msecs_to_jiffies(THERM_REG_RECHECK_DELAY_5S));

	/* no suspend resume activities for parallel charger */
	if (chip->parallel_charger)
		return 0;

	mutex_lock(&chip->irq_complete);
	chip->resume_completed = true;
	if (chip->irq_waiting) {
		mutex_unlock(&chip->irq_complete);
		/* smb1351_chg_stat_handler(client->irq, chip); */
		enable_irq(client->irq);
	} else {
		mutex_unlock(&chip->irq_complete);
	}
	return 0;
}

static const struct dev_pm_ops smb1351_pm_ops = {
	.suspend	= smb1351_suspend,
	.suspend_noirq	= smb1351_suspend_noirq,
	.resume		= smb1351_resume,
};

static struct of_device_id smb1351_match_table[] = {
	{ .compatible = "qcom,smb1351_charger",},
	{ },
};

static const struct i2c_device_id smb1351_charger_id[] = {
	{"smb1351-charger", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, smb1351_charger_id);

static struct i2c_driver smb1351_charger_driver = {
	.driver		= {
		.name		= "smb1351-charger",
		.owner		= THIS_MODULE,
		.of_match_table	= smb1351_match_table,
		.pm		= &smb1351_pm_ops,
	},
	.probe		= smb1351_charger_probe,
	.remove		= smb1351_charger_remove,
	.id_table	= smb1351_charger_id,
	.shutdown	= smb1351_charger_shutdown,
};

module_i2c_driver(smb1351_charger_driver);

MODULE_AUTHOR("BSP@xiaomi.com");
MODULE_DESCRIPTION("smb1351 Charger");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("i2c:smb1351-charger");
