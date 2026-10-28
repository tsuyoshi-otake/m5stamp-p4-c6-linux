// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#pragma once

#include <limits.h>
#include <stdint.h>

/*
 * Deep Sleep mailbox (#56), in the reserved PSRAM page that also holds the
 * boot update request (boot_ab.h).  Linux writes a sleep request and reboots;
 * the shim puts the chip to sleep and, on the next boot, reports how it woke.
 * PSRAM loses power in Deep Sleep, so the shim keeps the request and the
 * sleep memory in LP RAM while asleep and copies them back for Linux.
 *
 * The kernel driver (patch 0093, esp32p4-sleep.c) has its own copy of these
 * layouts; keep both in step and bump the versions on any change.
 */
#define SLEEP_MAILBOX_PA        0x499bf000u
#define SLEEP_REQUEST_PA        (SLEEP_MAILBOX_PA + 0x100u)
#define WAKE_INFO_PA            (SLEEP_MAILBOX_PA + 0x180u)
#define SLEEP_MEM_PA            (SLEEP_MAILBOX_PA + 0x200u)
#define SLEEP_MEM_BYTES         1024u

#define SLEEP_REQUEST_MAGIC     0x45534453u /* "ESDS" */
#define SLEEP_REQUEST_VERSION   2u /* 2: LP core temperature check (#57) */
#define SLEEP_REQUEST_COMMIT    0x534c4550u /* "SLEP" */
/* Enable the internal pull opposite to each wake level while asleep. */
#define SLEEP_FLAG_PULL         0x1u
/* About 49 days; the LP timer itself counts much longer. */
#define SLEEP_TIMER_MAX_MS      0xffffffffu
/* LP core check period: lp_timer_sleep_duration_us is 32 bits. */
#define SLEEP_LP_PERIOD_MIN_MS  100u
#define SLEEP_LP_PERIOD_MAX_MS  3600000u
/* lp_temp_low_mc / lp_temp_high_mc when that threshold is not used */
#define SLEEP_TEMP_OFF          INT32_MIN

#define WAKE_INFO_MAGIC         0x4553574bu /* "ESWK" */
#define WAKE_INFO_VERSION       2u

/* wake_info.boot */
#define WAKE_BOOT_POWERON       0u
#define WAKE_BOOT_RESET         1u /* any other reset, e.g. Linux reboot */
#define WAKE_BOOT_DEEPSLEEP     2u
#define WAKE_BOOT_INTERRUPTED   3u /* reset while asleep (RESET, USB) */

/* wake_info.causes */
#define WAKE_CAUSE_TIMER        0x1u
#define WAKE_CAUSE_GPIO         0x2u
#define WAKE_CAUSE_TEMP_LOW     0x4u /* the LP core saw temp <= low */
#define WAKE_CAUSE_TEMP_HIGH    0x8u /* the LP core saw temp >= high */

/* wake_info.skipped: why the last request did not put the chip to sleep */
#define SLEEP_SKIP_NONE         0u
#define SLEEP_SKIP_BUTTON       1u /* BOOT was held */
#define SLEEP_SKIP_GPIO_ACTIVE  2u /* a wake pin was already at its level */
#define SLEEP_SKIP_BAD_REQUEST  3u
#define SLEEP_SKIP_BOOT_UPDATE  4u /* a /boot update was pending */
#define SLEEP_SKIP_TEMP_ACTIVE  5u /* the temperature was already past a threshold */

#define SLEEP_MEM_MAGIC         0x45534d4du /* "ESMM" */
#define SLEEP_MEM_PENDING       0x50454e44u /* "PEND": Linux wrote new data */
#define SLEEP_MEM_PUBLISHED     0x5055424cu /* "PUBL": the shim's copy */

struct sleep_request {
	uint32_t magic;
	uint32_t version;
	uint32_t timer_ms;      /* 0: no timer */
	uint32_t gpio_mask;     /* LP GPIOs that wake, bit n = GPIOn */
	uint32_t gpio_high;     /* bit set: wake when high, else when low */
	uint32_t flags;         /* SLEEP_FLAG_* */
	uint32_t wall_sec_lo;   /* Linux clock when asked; 0: not set */
	uint32_t wall_sec_hi;
	uint32_t wall_usec;
	uint32_t seq;           /* echoed in wake_info */
	uint32_t lp_period_ms;  /* LP core check period; 0: LP core not used */
	int32_t lp_temp_low_mc; /* milli-degC; SLEEP_TEMP_OFF: not used */
	int32_t lp_temp_high_mc;
	uint32_t record_crc32;  /* IEEE CRC-32 of magic..lp_temp_high_mc */
	uint32_t commit_word;   /* written last */
} __attribute__((packed));

struct wake_info {
	uint32_t magic;
	uint32_t version;
	uint32_t boot;          /* WAKE_BOOT_* */
	uint32_t causes;        /* WAKE_CAUSE_* */
	uint32_t gpio_status;   /* LP GPIOs that woke the chip */
	uint32_t button;        /* 1: BOOT held when the shim started */
	uint32_t skipped;       /* SLEEP_SKIP_* */
	uint32_t skipped_gpio;  /* SLEEP_SKIP_GPIO_ACTIVE: the active pins */
	uint32_t seq;           /* request of this wake or skip */
	uint32_t slept_ms;
	uint32_t wall_sec_lo;   /* estimated clock at the Linux jump; 0: unknown */
	uint32_t wall_sec_hi;
	uint32_t wall_usec;
	uint32_t lp_checks;     /* LP core temperature readings while asleep */
	int32_t lp_last_mc;     /* milli-degC; meaningful when lp_checks > 0 */
	int32_t lp_min_mc;
	int32_t lp_max_mc;
	uint32_t record_crc32;  /* IEEE CRC-32 of magic..lp_max_mc */
} __attribute__((packed));

struct sleep_mem {
	uint32_t magic;
	uint32_t state;         /* SLEEP_MEM_PENDING or SLEEP_MEM_PUBLISHED */
	uint32_t len;
	uint32_t crc32;         /* IEEE CRC-32 of data[0..len) */
	uint8_t data[SLEEP_MEM_BYTES];
} __attribute__((packed));

_Static_assert(sizeof(struct sleep_request) == 60, "sleep request layout");
_Static_assert(sizeof(struct wake_info) == 72, "wake info layout");
_Static_assert(sizeof(struct sleep_mem) == 16 + SLEEP_MEM_BYTES, "sleep mem layout");
_Static_assert(SLEEP_REQUEST_PA + sizeof(struct sleep_request) <= WAKE_INFO_PA,
	       "sleep request overlaps wake info");
_Static_assert(WAKE_INFO_PA + sizeof(struct wake_info) <= SLEEP_MEM_PA,
	       "wake info overlaps sleep memory");
_Static_assert(SLEEP_MEM_PA + sizeof(struct sleep_mem) <= SLEEP_MAILBOX_PA + 0x1000u,
	       "sleep memory leaves the mailbox page");

/* Board pins (Tab5 would set its own; see #56 "ボード定義"). */
#if !defined(BOARD_SLEEP_C6_POWER_GPIO)
/* Stamp-P4 + C6 AddOn: GPIO42 is the C6 DC-DC enable, which latches on
 * through the C6 rail; a low pulse turns the C6 off until the next pulse. */
#define BOARD_SLEEP_C6_POWER_GPIO   42
#define BOARD_SLEEP_BUTTON_GPIO     35 /* BOOT, active low */
/* LP GPIO0-15 without GPIO9, which is the C6 IO9 boot strap on the AddOn. */
#define BOARD_SLEEP_WAKE_GPIO_MASK  0xfdffu
#endif

void deep_sleep_early(void);
void deep_sleep_publish(void);
