#!/usr/bin/env python3
"""Emit kernel/esp-hosted retention-BB helper text (PA placeholder).

Shared by 0052/0053/0026 generators so struct layout cannot drift.
"""
from __future__ import annotations

# Keep in lockstep with cmd53-bb/easystick_cmd53_bb.h (VERSION 6).
KERN_HELPER = r'''
/* EasyStick C/0053: CMD53 retention BB (DMA, torn-safe, post-done crumbs). */
#define EASYSTICK_CMD53_BB_PA		0xBBDEAD01u
#define EASYSTICK_CMD53_BB_MAGIC	0x45534242u
#define EASYSTICK_CMD53_BB_VERSION	6u
#define EASYSTICK_CMD53_BB_COMMIT_XOR	0xC0FFEE01u
#define EASYSTICK_CMD53_BB_ARM_FREE	0u
#define EASYSTICK_CMD53_BB_ARMING	1u
#define EASYSTICK_CMD53_BB_ARMED	2u
#define EASYSTICK_CMD53_BB_FOCUS_ARG	0x97ec0000u
#define EASYSTICK_CRASH_CAPSULE_MAGIC	0x45534350u
#define EASYSTICK_CRASH_CAPSULE_VERSION	1u
#define EASYSTICK_CRASH_CAPSULE_COMMIT_XOR	0xC3A5C0DEu
#define EASYSTICK_CRASH_CAPSULE_REASON_WDT	1u
#define EASYSTICK_CRASH_CAPSULE_REASON_OOPS	2u
#define EASYSTICK_CRASH_CAPSULE_REASON_PANIC	3u
#define EASYSTICK_CRASH_CAPSULE_STACK_BYTES	32u

struct easystick_crash_capsule {
	u32 magic;
	u32 version;
	u32 sequence;
	u32 commit;
	u32 reason;
	u32 cpu;
	u32 epc;
	u32 ra;
	u32 sp;
	u32 gp;
	u32 tp;
	u32 status;
	u32 cause;
	u32 badaddr;
	u32 wdt_config0;
	u32 wdt_config1;
	u32 wdt_config2;
	u32 wdt_int_raw;
	u32 stack_len;
	u8 stack[EASYSTICK_CRASH_CAPSULE_STACK_BYTES];
};

struct easystick_cmd53_bb {
	u32 magic;
	u32 version;
	u32 generation;
	u32 event_seq;
	u32 commit;
	u32 arm_state;
	u32 cmd_arg;
	u32 stage_request;
	u32 stage_cmd_done;
	u32 stage_cmd_err;
	u32 stage_idmac;
	u32 stage_data_over;
	u32 stage_data_err;
	u32 stage_cto;
	u32 stage_dto;
	u32 stage_end; /* BB1: pre mmc_request_done */
	u32 stage_bb2; /* BB2: post mmc_request_done */
	u32 stage_bb3; /* BB3: dw_mci_request_end return */
	u32 stage_bb4; /* BB4: mmc_wait_for_req return */
	u32 stage_bb5; /* BB5: sdio_memcpy_toio return */
	u32 stage_bb6; /* BB6: esp_write_block return */
	u32 bb3_state;   /* BB3 snapshot: host state at request_end return */
	u32 bb3_pending; /* BB3 snapshot: pending events at request_end return */
	u32 cmd_status;
	u32 data_status;
	u32 idsts;
	u32 pending_cmd;
	u32 pending_data;
	s32 cmd_error;
	s32 data_error;
	u32 bytes_xfered;
	u32 reset_reason_hint;
	struct easystick_crash_capsule crash;
	s32 bb4_ret;
	s32 bb5_ret;
	s32 bb6_ret;
	/*
	 * v6 boundary stages.  Keep these after crash so the capsule remains
	 * at EASYSTICK_CMD53_BB_PA + 0x80 for the independent WDT patch.
	 */
	u32 stage_idmac_complete_enter;
	u32 stage_idmac_complete_exit;
	u32 stage_bh_enter;
	u32 stage_data_complete_enter;
	u32 stage_data_complete_exit;
	u32 stage_irq_exit;
	/*
	 * v6 request-end split stages.  Keep these after crash so the capsule
	 * remains at EASYSTICK_CMD53_BB_PA + 0x80.
	 */
	u32 stage_request_end_enter;
	u32 stage_request_end_before_next;
	u32 stage_request_end_after_next;
	u32 stage_request_end_idle;
};

static inline volatile struct easystick_cmd53_bb *easystick_cmd53_bb_ptr(void)
{
	return (volatile struct easystick_cmd53_bb *)(uintptr_t)EASYSTICK_CMD53_BB_PA;
}

static void easystick_cmd53_bb_seal(volatile struct easystick_cmd53_bb *bb,
				   u32 gen, u32 seq)
{
	wmb(); /* payload before commit */
	WRITE_ONCE(bb->commit, gen ^ seq ^ EASYSTICK_CMD53_BB_COMMIT_XOR);
}

static void easystick_cmd53_bb_invalidate(volatile struct easystick_cmd53_bb *bb)
{
	WRITE_ONCE(bb->commit, 0);
	wmb(); /* invalidate before any further payload store */
}

/* Pre-BB1 path: stop IRQ crumbs once BB1 (stage_end) is sealed. */
static bool easystick_cmd53_bb_armed(void)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();
	u32 gen = READ_ONCE(bb->generation);

	return READ_ONCE(bb->magic) == EASYSTICK_CMD53_BB_MAGIC &&
	       READ_ONCE(bb->version) == EASYSTICK_CMD53_BB_VERSION &&
	       READ_ONCE(bb->arm_state) == EASYSTICK_CMD53_BB_ARMED &&
	       gen != 0 &&
	       READ_ONCE(bb->stage_request) == gen &&
	       READ_ONCE(bb->stage_end) != gen;
}

static bool easystick_cmd53_bb_cmd_matches(struct mmc_command *cmd)
{
	volatile struct easystick_cmd53_bb *bb;

	if (!cmd || cmd->opcode != SD_IO_RW_EXTENDED)
		return false;
	if (!easystick_cmd53_bb_armed())
		return false;
	bb = easystick_cmd53_bb_ptr();
	return cmd->arg == READ_ONCE(bb->cmd_arg);
}

static void easystick_cmd53_bb_begin(struct mmc_command *cmd)
{
	volatile struct easystick_cmd53_bb *bb;
	u32 gen;
	u32 old_gen;

	if (!cmd || cmd->opcode != SD_IO_RW_EXTENDED)
		return;
	/* Arm only the Boot B hang transfer class. */
	if (cmd->arg != EASYSTICK_CMD53_BB_FOCUS_ARG)
		return;
	bb = easystick_cmd53_bb_ptr();
	old_gen = READ_ONCE(bb->generation);
	/*
	 * Freeze the first target transfer until BB6 or reset.  This is
	 * deliberately based on the shared record, not a per-translation-unit
	 * flag: begin(), BB4, and BB6 live in different object files.
	 */
	if (READ_ONCE(bb->magic) == EASYSTICK_CMD53_BB_MAGIC &&
	    READ_ONCE(bb->version) == EASYSTICK_CMD53_BB_VERSION &&
	    old_gen != 0u &&
	    READ_ONCE(bb->stage_request) == old_gen &&
	    READ_ONCE(bb->stage_bb6) != old_gen)
		return;
	if (READ_ONCE(bb->arm_state) != EASYSTICK_CMD53_BB_ARM_FREE)
		return;
	gen = old_gen + 1u;
	if (!gen)
		gen = 1u;
	if (cmpxchg((u32 *)&bb->arm_state,
		    EASYSTICK_CMD53_BB_ARM_FREE,
		    EASYSTICK_CMD53_BB_ARMING) != EASYSTICK_CMD53_BB_ARM_FREE)
		return;
	easystick_cmd53_bb_invalidate(bb);
	WRITE_ONCE(bb->magic, EASYSTICK_CMD53_BB_MAGIC);
	WRITE_ONCE(bb->version, EASYSTICK_CMD53_BB_VERSION);
	WRITE_ONCE(bb->generation, gen);
	WRITE_ONCE(bb->event_seq, 1);
	WRITE_ONCE(bb->cmd_arg, cmd->arg);
	WRITE_ONCE(bb->stage_request, gen);
	easystick_cmd53_bb_seal(bb, gen, 1);
	wmb();
	WRITE_ONCE(bb->arm_state, EASYSTICK_CMD53_BB_ARMED);
}

static void easystick_cmd53_bb_event(volatile u32 *stage_word,
				    volatile u32 *extra1, u32 v1,
				    volatile u32 *extra2, u32 v2)
{
	volatile struct easystick_cmd53_bb *bb;
	u32 gen;
	u32 seq;

	if (!easystick_cmd53_bb_armed())
		return;
	bb = easystick_cmd53_bb_ptr();
	gen = READ_ONCE(bb->generation);
	seq = READ_ONCE(bb->event_seq) + 1u;
	if (!seq)
		seq = 1u;
	easystick_cmd53_bb_invalidate(bb);
	if (extra1)
		WRITE_ONCE(*extra1, v1);
	if (extra2)
		WRITE_ONCE(*extra2, v2);
	WRITE_ONCE(*stage_word, gen);
	WRITE_ONCE(bb->event_seq, seq);
	easystick_cmd53_bb_seal(bb, gen, seq);
}

static void easystick_cmd53_bb_mark_host_stage(
	struct dw_mci *host, volatile u32 *stage_word)
{
	if (!host || !host->mrq ||
	    !easystick_cmd53_bb_cmd_matches(host->mrq->cmd))
		return;
	easystick_cmd53_bb_event(stage_word, NULL, 0, NULL, 0);
}

static void easystick_cmd53_bb_mark_request_end_stage(
	struct mmc_request *mrq, volatile u32 *stage_word)
{
	if (!mrq || !mrq->cmd ||
	    !easystick_cmd53_bb_cmd_matches(mrq->cmd))
		return;
	easystick_cmd53_bb_event(stage_word, NULL, 0, NULL, 0);
}

static bool easystick_cmd53_bb_post_open_gen(u32 gen)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	return gen != 0u &&
	       READ_ONCE(bb->arm_state) == EASYSTICK_CMD53_BB_ARMED &&
	       READ_ONCE(bb->generation) == gen &&
	       READ_ONCE(bb->stage_request) == gen &&
	       READ_ONCE(bb->stage_end) == gen &&
	       READ_ONCE(bb->stage_bb6) != gen;
}

static void easystick_cmd53_bb_post_stage(u32 gen,
					  volatile u32 *stage_word,
					  volatile s32 *ret_word,
					  s32 ret)
{
	if (!easystick_cmd53_bb_post_open_gen(gen))
		return;
	if (ret_word)
		WRITE_ONCE(*ret_word, ret);
	/*
	 * Post stages are monotonic single-writer words.  Do not invalidate or
	 * reseal the common BB1 commit: BB2..BB6 run on different CPUs.
	 */
	wmb();
	WRITE_ONCE(*stage_word, gen);
}

static void easystick_cmd53_bb_mark_cmd_done(struct mmc_command *cmd)
{
	volatile struct easystick_cmd53_bb *bb;

	if (!easystick_cmd53_bb_cmd_matches(cmd))
		return;
	bb = easystick_cmd53_bb_ptr();
	easystick_cmd53_bb_event(&bb->stage_cmd_done, NULL, 0, NULL, 0);
}

static void easystick_cmd53_bb_mark_cmd_err(struct mmc_command *cmd,
					   u32 status, u32 pending)
{
	volatile struct easystick_cmd53_bb *bb;

	if (!easystick_cmd53_bb_cmd_matches(cmd))
		return;
	bb = easystick_cmd53_bb_ptr();
	easystick_cmd53_bb_event(&bb->stage_cmd_err, &bb->cmd_status, status,
				 &bb->pending_cmd, pending);
}

static void easystick_cmd53_bb_mark_idmac(u32 idsts)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	easystick_cmd53_bb_event(&bb->stage_idmac, &bb->idsts, idsts, NULL, 0);
}

static void easystick_cmd53_bb_mark_data_over(u32 pending)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	easystick_cmd53_bb_event(&bb->stage_data_over, &bb->pending_data, pending,
				 NULL, 0);
}

static void easystick_cmd53_bb_mark_data_err(u32 status, u32 pending)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	easystick_cmd53_bb_event(&bb->stage_data_err, &bb->data_status, status,
				 &bb->pending_data, pending);
}

static void easystick_cmd53_bb_mark_cto(struct mmc_command *cmd)
{
	volatile struct easystick_cmd53_bb *bb;

	if (!easystick_cmd53_bb_cmd_matches(cmd))
		return;
	bb = easystick_cmd53_bb_ptr();
	easystick_cmd53_bb_event(&bb->stage_cto, NULL, 0, NULL, 0);
}

static void easystick_cmd53_bb_mark_dto(void)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	easystick_cmd53_bb_event(&bb->stage_dto, NULL, 0, NULL, 0);
}

/* BB1.  Copy request fields before mmc_request_done() can wake the caller. */
static u32 easystick_cmd53_bb_mark_end_and_get_gen(struct mmc_request *mrq)
{
	volatile struct easystick_cmd53_bb *bb;
	u32 gen;
	u32 seq;
	struct mmc_command *cmd;
	struct mmc_data *data;

	if (!mrq || !easystick_cmd53_bb_cmd_matches(mrq->cmd))
		return 0;
	bb = easystick_cmd53_bb_ptr();
	gen = READ_ONCE(bb->generation);
	seq = READ_ONCE(bb->event_seq) + 1u;
	if (!seq)
		seq = 1u;
	cmd = mrq->cmd;
	data = mrq->data;
	easystick_cmd53_bb_invalidate(bb);
	WRITE_ONCE(bb->cmd_error, cmd->error);
	if (data) {
		WRITE_ONCE(bb->data_error, data->error);
		WRITE_ONCE(bb->bytes_xfered, data->bytes_xfered);
	}
	WRITE_ONCE(bb->stage_end, gen);
	WRITE_ONCE(bb->event_seq, seq);
	easystick_cmd53_bb_seal(bb, gen, seq);
	return gen;
}

/* BB2.  No request pointer is valid or needed after mmc_request_done(). */
static void easystick_cmd53_bb_mark_bb2_gen(u32 gen)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	if (!easystick_cmd53_bb_post_open_gen(gen))
		return;
	easystick_cmd53_bb_post_stage(gen, &bb->stage_bb2, NULL, 0);
}

/* BB3.  Snapshot host state without dereferencing the completed request. */
static void easystick_cmd53_bb_mark_bb3_gen(u32 gen, u32 state, u32 pending)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	if (!easystick_cmd53_bb_post_open_gen(gen))
		return;
	WRITE_ONCE(bb->bb3_state, state);
	WRITE_ONCE(bb->bb3_pending, pending);
	wmb();
	WRITE_ONCE(bb->stage_bb3, gen);
}

'''

# Upper layers (mmc core / sdio / esp-hosted): post-only helpers, no dw_mci types.
POST_ONLY_HELPER = r'''
/* EasyStick 0053: post-mmc_request_done retention crumbs (no printk). */
#define EASYSTICK_CMD53_BB_PA		0xBBDEAD01u
#define EASYSTICK_CMD53_BB_MAGIC	0x45534242u
#define EASYSTICK_CMD53_BB_VERSION	6u
#define EASYSTICK_CMD53_BB_COMMIT_XOR	0xC0FFEE01u
#define EASYSTICK_CMD53_BB_ARM_FREE	0u
#define EASYSTICK_CMD53_BB_ARMING	1u
#define EASYSTICK_CMD53_BB_ARMED	2u
#define EASYSTICK_CRASH_CAPSULE_MAGIC	0x45534350u
#define EASYSTICK_CRASH_CAPSULE_VERSION	1u
#define EASYSTICK_CRASH_CAPSULE_COMMIT_XOR	0xC3A5C0DEu
#define EASYSTICK_CRASH_CAPSULE_REASON_WDT	1u
#define EASYSTICK_CRASH_CAPSULE_REASON_OOPS	2u
#define EASYSTICK_CRASH_CAPSULE_REASON_PANIC	3u
#define EASYSTICK_CRASH_CAPSULE_STACK_BYTES	32u

struct easystick_crash_capsule {
	u32 magic;
	u32 version;
	u32 sequence;
	u32 commit;
	u32 reason;
	u32 cpu;
	u32 epc;
	u32 ra;
	u32 sp;
	u32 gp;
	u32 tp;
	u32 status;
	u32 cause;
	u32 badaddr;
	u32 wdt_config0;
	u32 wdt_config1;
	u32 wdt_config2;
	u32 wdt_int_raw;
	u32 stack_len;
	u8 stack[EASYSTICK_CRASH_CAPSULE_STACK_BYTES];
};

struct easystick_cmd53_bb {
	u32 magic;
	u32 version;
	u32 generation;
	u32 event_seq;
	u32 commit;
	u32 arm_state;
	u32 cmd_arg;
	u32 stage_request;
	u32 stage_cmd_done;
	u32 stage_cmd_err;
	u32 stage_idmac;
	u32 stage_data_over;
	u32 stage_data_err;
	u32 stage_cto;
	u32 stage_dto;
	u32 stage_end;
	u32 stage_bb2;
	u32 stage_bb3;
	u32 stage_bb4;
	u32 stage_bb5;
	u32 stage_bb6;
	u32 bb3_state;
	u32 bb3_pending;
	u32 cmd_status;
	u32 data_status;
	u32 idsts;
	u32 pending_cmd;
	u32 pending_data;
	s32 cmd_error;
	s32 data_error;
	u32 bytes_xfered;
	u32 reset_reason_hint;
	struct easystick_crash_capsule crash;
	s32 bb4_ret;
	s32 bb5_ret;
	s32 bb6_ret;
	/*
	 * v6 boundary stages.  Keep these after crash so the capsule remains
	 * at EASYSTICK_CMD53_BB_PA + 0x80 for the independent WDT patch.
	 */
	u32 stage_idmac_complete_enter;
	u32 stage_idmac_complete_exit;
	u32 stage_bh_enter;
	u32 stage_data_complete_enter;
	u32 stage_data_complete_exit;
	u32 stage_irq_exit;
	/*
	 * v6 request-end split stages.  Keep these after crash so the capsule
	 * remains at EASYSTICK_CMD53_BB_PA + 0x80.
	 */
	u32 stage_request_end_enter;
	u32 stage_request_end_before_next;
	u32 stage_request_end_after_next;
	u32 stage_request_end_idle;
};

static inline volatile struct easystick_cmd53_bb *easystick_cmd53_bb_ptr(void)
{
	return (volatile struct easystick_cmd53_bb *)(uintptr_t)EASYSTICK_CMD53_BB_PA;
}

static void easystick_cmd53_bb_seal(volatile struct easystick_cmd53_bb *bb,
				   u32 gen, u32 seq)
{
	wmb();
	WRITE_ONCE(bb->commit, gen ^ seq ^ EASYSTICK_CMD53_BB_COMMIT_XOR);
}

static void easystick_cmd53_bb_invalidate(volatile struct easystick_cmd53_bb *bb)
{
	WRITE_ONCE(bb->commit, 0);
	wmb();
}

static bool easystick_cmd53_bb_post_open(void)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();
	u32 gen = READ_ONCE(bb->generation);

	return READ_ONCE(bb->magic) == EASYSTICK_CMD53_BB_MAGIC &&
	       READ_ONCE(bb->version) == EASYSTICK_CMD53_BB_VERSION &&
	       READ_ONCE(bb->arm_state) == EASYSTICK_CMD53_BB_ARMED &&
	       gen != 0 &&
	       READ_ONCE(bb->stage_request) == gen &&
	       READ_ONCE(bb->stage_end) == gen &&
	       READ_ONCE(bb->stage_bb6) != gen;
}

static bool easystick_cmd53_bb_post_open_gen(u32 gen)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();

	return gen != 0u &&
	       READ_ONCE(bb->arm_state) == EASYSTICK_CMD53_BB_ARMED &&
	       READ_ONCE(bb->generation) == gen &&
	       READ_ONCE(bb->stage_request) == gen &&
	       READ_ONCE(bb->stage_end) == gen &&
	       READ_ONCE(bb->stage_bb6) != gen;
}

static void easystick_cmd53_bb_post_stage(u32 gen,
					  volatile u32 *stage_word,
					  volatile s32 *ret_word,
					  s32 ret)
{
	if (!easystick_cmd53_bb_post_open_gen(gen))
		return;
	if (ret_word)
		WRITE_ONCE(*ret_word, ret);
	/* Post stages never race by updating the common commit/event_seq pair. */
	wmb();
	WRITE_ONCE(*stage_word, gen);
}

static void easystick_cmd53_bb_mark_bb4(struct mmc_request *mrq)
{
	volatile struct easystick_cmd53_bb *bb;
	u32 gen;
	s32 cmd_e = 0;
	s32 data_e = 0;

	if (!easystick_cmd53_bb_post_open())
		return;
	bb = easystick_cmd53_bb_ptr();
	gen = READ_ONCE(bb->generation);
	if (mrq && mrq->cmd) {
		if (mrq->cmd->arg != READ_ONCE(bb->cmd_arg))
			return;
		cmd_e = mrq->cmd->error;
	}
	if (mrq && mrq->data) {
		data_e = mrq->data->error;
	}
	WRITE_ONCE(bb->bb4_ret, cmd_e ? cmd_e : data_e);
	/*
	 * BB1 already captured these values before completion.  Keep them
	 * unchanged here; only the dedicated BB4 result precedes its stage word.
	 */
	easystick_cmd53_bb_post_stage(gen, &bb->stage_bb4, NULL, 0);
}

static void easystick_cmd53_bb_mark_bb5(int ret)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();
	u32 gen;

	if (!easystick_cmd53_bb_post_open())
		return;
	gen = READ_ONCE(bb->generation);
	if (READ_ONCE(bb->stage_bb4) != gen)
		return;
	easystick_cmd53_bb_post_stage(gen, &bb->stage_bb5, &bb->bb5_ret, ret);
}

static void easystick_cmd53_bb_mark_bb6(int ret)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();
	u32 gen;

	if (!easystick_cmd53_bb_post_open())
		return;
	gen = READ_ONCE(bb->generation);
	if (READ_ONCE(bb->stage_bb5) != gen)
		return;
	easystick_cmd53_bb_post_stage(gen, &bb->stage_bb6, &bb->bb6_ret, ret);
	if (READ_ONCE(bb->stage_bb6) == gen) {
		wmb();
		WRITE_ONCE(bb->arm_state, EASYSTICK_CMD53_BB_ARM_FREE);
	}
}

'''
