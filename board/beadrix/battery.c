/* Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery pack vendor provided charging profile
 */

#include "battery_fuel_gauge.h"
#include "charge_state.h"
#include "common.h"

/*
 * Battery info for all beadrix battery types. Note that the fields
 * start_charging_min/max and charging_min/max are not used for the charger.
 * The effective temperature limits are given by discharging_min/max_c.
 *
 * Fuel Gauge (FG) parameters which are used for determining if the battery
 * is connected, the appropriate ship mode (battery cutoff) command, and the
 * charge/discharge FETs status.
 *
 * Ship mode (battery cutoff) requires 2 writes to the appropriate smart battery
 * register. For some batteries, the charge/discharge FET bits are set when
 * charging/discharging is active, in other types, these bits set mean that
 * charging/discharging is disabled. Therefore, in addition to the mask for
 * these bits, a disconnect value must be specified. Note that for TI fuel
 * gauge, the charge/discharge FET status is found in Operation Status (0x54),
 * but a read of Manufacturer Access (0x00) will return the lower 16 bits of
 * Operation status which contains the FET status bits.
 *
 * The assumption for battery types supported is that the charge/discharge FET
 * status can be read with a sb_read() command and therefore, only the register
 * address, mask, and disconnect value need to be provided.
 */
const struct batt_conf_embed board_battery_info[] = {
	/* GanFeng SG20 Battery Information */
	[BATTERY_GANFENG] = {
		.manuf_name = "Ganfeng",
		.device_name = "SG20",
		.config = {
			.fuel_gauge = {
				.ship_mode = {
					.reg_addr = 0x00,
					.reg_data = { 0x0010, 0x0010 },
				},
				.fet = {
					.reg_addr = 0x43,
					.reg_mask = 0x0003,
					.disconnect_val = 0x0000,
				},
			},
			.batt_info = {
				.voltage_max        = 8700, /* mV */
				.voltage_normal     = 7600, /* mV */
				.voltage_min        = 6000, /* mV */
				.precharge_current  = 256,  /* mA */
				.start_charging_min_c = 0,
				.start_charging_max_c = 50,
				.charging_min_c     = 0,
				.charging_max_c     = 60,
				.discharging_min_c  = -20,
				.discharging_max_c  = 60,
			},
		},
	},
	/* Pow-Tech SG20JL1C2S5900-P1P1 Battery Information */
	[BATTERY_POWTECH_SG20JL1C] = {
		.manuf_name = "POW-TEC",
		.device_name = "SG20",
		.config = {
			.fuel_gauge = {
				.ship_mode = {
					.reg_addr = 0x00,
					.reg_data = { 0x0010, 0x0010 },
				},
				.fet = {
					.reg_addr = 0x54,
					.reg_mask = 0x0006,
					.disconnect_val = 0x0000,
				},
				.flags = FUEL_GAUGE_FLAG_MFGACC,
			},
			.batt_info = {
				.voltage_max        = 8800, /* mV */
				.voltage_normal     = 7700, /* mV */
				.voltage_min        = 6000, /* mV */
				.precharge_current  = 350,  /* mA */
				.start_charging_min_c = 0,
				.start_charging_max_c = 45,
				.charging_min_c     = 0,
				.charging_max_c     = 55,
				.discharging_min_c  = -20,
				.discharging_max_c  = 60,
			},
		},
	},
	/* GanFeng SR41 Battery Information */
	[BATTERY_GFL] = {
		.manuf_name = "GFL",
		.device_name = "MFP-466894",
		.config = {
			.fuel_gauge = {
				.ship_mode = {
					.reg_addr = 0x00,
					.reg_data = { 0x0010, 0x0010 },
				},
				.fet = {
					.reg_addr = 0x54,
					.reg_mask = 0x0006,
					.disconnect_val = 0x0000,
					.cfet_mask = 0x4000,
					.cfet_off_val = 0x4000,
				},
				.flags = FUEL_GAUGE_FLAG_MFGACC,
			},
			.batt_info = {
				.voltage_max        = 8800, /* mV */
				.voltage_normal     = 7700, /* mV */
				.voltage_min        = 6000, /* mV */
				.precharge_current  = 100,  /* mA */
				.start_charging_min_c = 0,
				.start_charging_max_c = 50,
				.charging_min_c     = 0,
				.charging_max_c     = 60,
				.discharging_min_c  = -20,
				.discharging_max_c  = 60,
			},
		},
	},
};
BUILD_ASSERT(ARRAY_SIZE(board_battery_info) == BATTERY_TYPE_COUNT);

const enum battery_type DEFAULT_BATTERY_TYPE = BATTERY_GFL;
