// SPDX-License-Identifier: MIT
/* debug.intelxe.sensors_raw: the data of debug.intelxe.sensors (the render GT's clocks, the package and VRAM
 * temperatures, the card and package energy and average power, the power limits) as one binary sample, for programs
 * that would otherwise parse that text: Level Zero sysman over NEO (user/xemac xemac_sensors(), the zes* frequency,
 * power and temperature modules), monitors. Read only; anyone may read it; reading does not wake the GPU (the same
 * registers debug.intelxe.sensors reads, the same energy meter: a read of either advances the power window).
 *
 *   struct intelxe_sensors s; size_t n = sizeof s;
 *   sysctlbyname(INTELXE_SENSORS_SYSCTL, &s, &n, NULL, 0);   then check s.version and s.size
 *
 * A newer kext may append fields: s.size is the size of the kext's struct, and a caller with a smaller (older) struct
 * gets its prefix (the sysctl copies at most the caller's length). Fields whose INTELXE_SENSORS_* bit is clear in
 * `valid` are 0. Units are the base units of Linux's sysfs/hwmon where the
 * same quantity exists there (MHz, microjoules, milliwatts), temperatures in whole degrees Celsius.
 */
#pragma once
#include <stdint.h>

#define INTELXE_SENSORS_SYSCTL "debug.intelxe.sensors_raw"
#define INTELXE_SENSORS_VERSION 1

/* intelxe_sensors.state: why nothing is valid (0 = sampled) */
#define INTELXE_SENSORS_OK 0u
#define INTELXE_SENSORS_ASLEEP 1u       /* the device is suspended: nothing is read */
#define INTELXE_SENSORS_NOT_MAPPED 2u   /* no BAR0 yet */
#define INTELXE_SENSORS_NO_ANSWER 3u    /* the registers read all ones (the device fell off the bus) */
#define INTELXE_SENSORS_NO_DEVICE 4u

/* intelxe_sensors.valid */
#define INTELXE_SENSORS_FREQ (1u << 0)       /* act/req/rp0/rpn_mhz */
#define INTELXE_SENSORS_TEMP (1u << 1)       /* pkg/vram_temp_c */
#define INTELXE_SENSORS_ENERGY (1u << 2)     /* energy_uj[] (PMT telemetry: BMG) */
#define INTELXE_SENSORS_POWER_CARD (1u << 3) /* power_mw[CARD]: the last full window's average */
#define INTELXE_SENSORS_POWER_PKG (1u << 4)  /* power_mw[PKG] */
#define INTELXE_SENSORS_LIMITS (1u << 5)     /* limit_mw[][], limit_on (the pcode mailbox: has_mbx_power_limits, BMG) */
#define INTELXE_SENSORS_CRIT (1u << 6)       /* crit_milli, crit_is_watts */
#define INTELXE_SENSORS_RANGE (1u << 7)      /* rpe/min/max_mhz: SLPC is running */

#define INTELXE_SENSORS_CARD 0
#define INTELXE_SENSORS_PKG 1

struct intelxe_sensors {
	uint32_t version;      /* INTELXE_SENSORS_VERSION */
	uint32_t size;         /* bytes filled: sizeof(struct intelxe_sensors) of the kext */
	uint32_t state;        /* INTELXE_SENSORS_OK or why not */
	uint32_t valid;        /* INTELXE_SENSORS_* bits */
	uint64_t timestamp_ns; /* when sampled: CLOCK_UPTIME_RAW (mach_absolute_time) in ns */
	/* the render GT (xe: tile0/gt0/freq0): act_freq (CAGF; 0 while the GT is in RC6), cur_freq (the request),
	 * rp0/rpn_freq (the hardware's limits), rpe_freq (efficient), min/max_freq (what SLPC runs with: slpc_min raised to
	 * an slpc_boost lease, slpc_max or RP0) */
	uint32_t act_mhz, req_mhz, rp0_mhz, rpn_mhz;
	uint32_t rpe_mhz, min_mhz, max_mhz;
	int32_t pkg_temp_c, vram_temp_c; /* xe hwmon temp2_input (package) and temp3_input (VRAM), in degrees */
	uint32_t reserved0;
	uint64_t energy_uj[2];  /* [CARD, PKG]: xe hwmon energy1_input / energy2_input, summed across the counters' wraps */
	uint32_t power_mw[2];   /* [CARD, PKG] */
	uint32_t limit_mw[2][2]; /* [CARD, PKG][PL1, PL2]: xe hwmon power1_max / power1_cap, power2_max / power2_cap */
	uint32_t limit_on;       /* bit (channel * 2 + level): that limit is enabled */
	uint32_t crit_milli;     /* card I1 (xe hwmon power1_crit or curr1_crit): milliwatts or milliamperes */
	uint32_t crit_is_watts;
	uint32_t reserved1;
};
