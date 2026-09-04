/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
/*
 * EasyStick CMD53 retention black-box (shared layout).
 *
 * Boot-shim places the CMD53 record and optional CMD52 marker in .rtc_noinit
 * via RTC_NOINIT_ATTR.
 * Kernel writes through the nm-resolved PA only (never a hand-picked hole).
 *
 * Commit contract (torn-safe):
 *   commit = 0
 *   barrier
 *   payload (generation-tagged stages; event_seq++)
 *   barrier
 *   commit = generation ^ event_seq ^ XOR
 *
 * Stage words hold the generation that armed them (not 0/1). A stage is
 * active for dump iff stage_X == generation. begin() does not clear stale
 * fields — it only arms a new generation.
 *
 * v6 / Experiment 0053 post-`mmc_request_done` breadcrumbs plus boundary
 * markers around IDMAC completion, the MMC bottom half, the IRQ return, and
 * the request-end queue handoff:
 *   stage_end  = BB1 (immediately before mmc_request_done)
 *   stage_bb2  = BB2 (immediately after mmc_request_done returns)
 *   stage_bb3  = BB3 (dw_mci_request_end return)
 *   stage_bb4  = BB4 (mmc_wait_for_req returns)
 *   stage_bb5  = BB5 (sdio_memcpy_toio returns)
 *   stage_bb6  = BB6 (esp_write_block returns)
 *
 * FOCUS_ARG (0x97ec0000): Boot B hang transfer — write/fn1/incr/addr 0x1f600/512.
 * Kernel begin() arms only that CMD53 so the sealed generation is the shot.
 *
 * request_end split stages (stage_request_end_*) are generation-tagged
 * sticky bits, not counters. mark_request_end_stage() writes the current
 * generation into one word; dump prints 1 iff that word == generation.
 * begin() does not clear stale words — a new generation simply makes the
 * old tags inactive. So `enter=1 before_next=0 after_next=0 idle=1` means:
 * the last armed FOCUS_ARG CMD53's dw_mci_request_end() entered, did not
 * take the queued-next-request arm, and did take the STATE_IDLE arm.
 * That is the last focused request_end path, not a live host-idle flag
 * and not a count of idle entries. A later non-FOCUS_ARG request does
 * not update these bits. The host may still have run other MRQs after
 * that focused transfer; those are invisible here.
 */
#ifndef EASYSTICK_CMD53_BB_H
#define EASYSTICK_CMD53_BB_H

#include <stddef.h>
#include <stdint.h>

#define EASYSTICK_CMD53_BB_MAGIC		0x45534242u /* 'ESBB' */
#define EASYSTICK_CMD53_BB_VERSION		6u
#define EASYSTICK_CMD53_BB_COMMIT_XOR	0xC0FFEE01u
#define EASYSTICK_CMD53_BB_ARM_FREE	0u
#define EASYSTICK_CMD53_BB_ARMING	1u
#define EASYSTICK_CMD53_BB_ARMED	2u
#define EASYSTICK_CMD53_BB_SELFTEST_GEN	0x5E1F0001u
#define EASYSTICK_CMD53_BB_TORN_GEN	0x70E40001u
/* Boot B / Experiment A hang CMD53 argument (write, 0x1f600, 512 B). */
#define EASYSTICK_CMD53_BB_FOCUS_ARG	0x97ec0000u
#define EASYSTICK_CRASH_CAPSULE_MAGIC	0x45534350u /* 'ESCP' */
#define EASYSTICK_CRASH_CAPSULE_VERSION	1u
#define EASYSTICK_CRASH_CAPSULE_COMMIT_XOR	0xC3A5C0DEu
#define EASYSTICK_CRASH_CAPSULE_REASON_WDT	1u
#define EASYSTICK_CRASH_CAPSULE_REASON_OOPS	2u
#define EASYSTICK_CRASH_CAPSULE_REASON_PANIC	3u
#define EASYSTICK_CRASH_CAPSULE_STACK_BYTES	32u
#define EASYSTICK_CMD52_MARKER_OFFSET		0x120u
#define EASYSTICK_CMD52_MARKER_WORDS		6u
#define EASYSTICK_CMD52_MARKER_EMPTY		0u
#define EASYSTICK_CMD52_MARKER_MAGIC		0x45534d30u /* 'ESM0' */
#define EASYSTICK_CMD52_MARKER_ARMED		0x45534d31u /* 'ESM1' */
#define EASYSTICK_CMD52_MARKER_TOKEN_ENTER	0x45534d32u /* 'ESM2' */
#define EASYSTICK_CMD52_MARKER_AFTER_46	0x45534d33u /* 'ESM3' */
#define EASYSTICK_CMD52_MARKER_BEFORE_47	0x45534d34u /* 'ESM4' */
#define EASYSTICK_CMD52_MARKER_AFTER_47	0x45534d35u /* 'ESM5' */
#define EASYSTICK_BEAT_CPUS			2u
#define EASYSTICK_BEAT_PERIOD_MS		100u
#define EASYSTICK_BEAT_UNUSED_CPU		0xFFFFFFFFu

struct easystick_crash_capsule {
	uint32_t magic;
	uint32_t version;
	uint32_t sequence;
	uint32_t commit;
	uint32_t reason;
	uint32_t cpu;
	uint32_t epc;
	uint32_t ra;
	uint32_t sp;
	uint32_t gp;
	uint32_t tp;
	uint32_t status;
	uint32_t cause;
	uint32_t badaddr;
	uint32_t wdt_config0;
	uint32_t wdt_config1;
	uint32_t wdt_config2;
	uint32_t wdt_int_raw;
	uint32_t stack_len;
	uint8_t stack[EASYSTICK_CRASH_CAPSULE_STACK_BYTES];
};

/*
 * The CMD52 boundary probe is deliberately outside the v6 CMD53 record.
 * Keeping it at BB+0x120 preserves the crash-capsule offset and the existing
 * 0x120-byte producer/consumer contract. The boot-shim linker check verifies
 * that the six-word symbol is physically adjacent to easystick_cmd53_bb.
 */
struct easystick_cmd52_marker {
	uint32_t magic;
	uint32_t armed;
	uint32_t token_enter;
	uint32_t after_46;
	uint32_t before_47;
	uint32_t after_47;
};

/*
 * Instrument C: per-CPU kthread breadcrumb. Lives AFTER the 0x120 BB and the
 * 0x18 CMD52 marker hole so it cannot collide with crash@0x80 or v6 suffix.
 * Four raw words; no converted jiffies.
 */
struct easystick_beat {
	uint32_t seq;
	uint32_t cpu;
	uint32_t beat_jiffies;
	uint32_t last_feed_jiffies;
};

/* LP ROM BSS floor cited by locked IDF P4 layout (fail-closed assert). */
#define EASYSTICK_LP_ROM_BSS_FLOOR	0x5010fa80u

struct easystick_cmd53_bb {
	uint32_t magic;
	uint32_t version;
	uint32_t generation;
	uint32_t event_seq;
	uint32_t commit; /* generation ^ event_seq ^ XOR after each sealed update */
	uint32_t arm_state; /* FREE, ARMING, or ARMED; guards first-target claim */

	uint32_t cmd_arg;

	/* Generation-tagged stages (value == generation when armed). */
	uint32_t stage_request;
	uint32_t stage_cmd_done;
	uint32_t stage_cmd_err;
	uint32_t stage_idmac;
	uint32_t stage_data_over;
	uint32_t stage_data_err;
	uint32_t stage_cto;
	uint32_t stage_dto;
	uint32_t stage_end; /* BB1 */
	uint32_t stage_bb2;
	uint32_t stage_bb3;
	uint32_t stage_bb4;
	uint32_t stage_bb5;
	uint32_t stage_bb6;
	uint32_t bb3_state;   /* host state at request_end return */
	uint32_t bb3_pending; /* pending events at request_end return */

	uint32_t cmd_status;
	uint32_t data_status;
	uint32_t idsts;
	uint32_t pending_cmd;
	uint32_t pending_data;

	int32_t cmd_error;
	int32_t data_error;
	uint32_t bytes_xfered;

	uint32_t reset_reason_hint; /* boot-shim fill on dump only */
	struct easystick_crash_capsule crash;
	/* Post-completion results; written before the matching stage word. */
	int32_t bb4_ret;
	int32_t bb5_ret;
	int32_t bb6_ret;
	/*
	 * v6 boundary stages.  Keep these after crash so the capsule remains
	 * at EASYSTICK_CMD53_BB_PA + 0x80 for the independent WDT patch.
	 */
	uint32_t stage_idmac_complete_enter;
	uint32_t stage_idmac_complete_exit;
	uint32_t stage_bh_enter;
	uint32_t stage_data_complete_enter;
	uint32_t stage_data_complete_exit;
	uint32_t stage_irq_exit;
	/*
	 * v6 request-end split stages.  Keep these after crash so the capsule
	 * remains at EASYSTICK_CMD53_BB_PA + 0x80.
	 */
	uint32_t stage_request_end_enter;
	uint32_t stage_request_end_before_next;
	uint32_t stage_request_end_after_next;
	uint32_t stage_request_end_idle;
};

_Static_assert(offsetof(struct easystick_cmd53_bb, crash) == 0x80u,
	       "crash capsule offset must remain PA+0x80");
_Static_assert(sizeof(struct easystick_cmd53_bb) == 0x120u,
	       "CMD53 BB v6 layout size must remain 0x120");
_Static_assert(sizeof(struct easystick_cmd52_marker) ==
	       EASYSTICK_CMD52_MARKER_WORDS * sizeof(uint32_t),
	       "CMD52 marker must remain six 32-bit words");
_Static_assert(sizeof(struct easystick_beat) == 16u,
	       "beat slot must remain four 32-bit words");
_Static_assert(sizeof(struct easystick_beat) * EASYSTICK_BEAT_CPUS == 32u,
	       "beat array must remain two slots");

/*
 * One RTC_NOINIT object. Separate symbols were reordered by the P4 linker
 * (observed 2026-09-01: beat @ 0x50108080, bb @ 0x501080a0).
 */
struct easystick_rtc_c_window {
	struct easystick_cmd53_bb bb;
	struct easystick_cmd52_marker marker;
	struct easystick_beat beat[EASYSTICK_BEAT_CPUS];
};

_Static_assert(offsetof(struct easystick_rtc_c_window, marker) == 0x120u,
	       "CMD52 marker hole must stay at BB+0x120");
_Static_assert(offsetof(struct easystick_rtc_c_window, beat) == 0x138u,
	       "beat must stay at BB+0x138");
_Static_assert(sizeof(struct easystick_rtc_c_window) == 0x158u,
	       "RTC C window must stay BB + marker hole + two beat slots");

static inline uint32_t easystick_cmd53_bb_commit_word(uint32_t generation,
						       uint32_t event_seq)
{
	return generation ^ event_seq ^ EASYSTICK_CMD53_BB_COMMIT_XOR;
}

static inline int easystick_cmd53_bb_stage_active(uint32_t stage, uint32_t generation)
{
	return stage == generation && generation != 0u;
}

static inline int easystick_cmd53_bb_valid(const volatile struct easystick_cmd53_bb *bb)
{
	uint32_t gen = bb->generation;
	uint32_t seq = bb->event_seq;
	uint32_t commit = bb->commit;

	if (bb->magic != EASYSTICK_CMD53_BB_MAGIC)
		return 0;
	if (bb->version != EASYSTICK_CMD53_BB_VERSION)
		return 0;
	if (seq == 0u)
		return 0;
	if (commit != easystick_cmd53_bb_commit_word(gen, seq))
		return 0;
	if (!easystick_cmd53_bb_stage_active(bb->stage_request, gen))
		return 0;
	return 1;
}

void easystick_cmd53_bb_dump_early(void);
void easystick_cmd53_bb_release_after_dump(void);

#endif /* EASYSTICK_CMD53_BB_H */
