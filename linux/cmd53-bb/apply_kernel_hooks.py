#!/usr/bin/env python3
"""Retired legacy CMD53 hook injector.

Use gen_retention_bb_patches.py instead.  This file contains the obsolete v2
layout and must never mutate a build tree.
"""
from __future__ import annotations

from pathlib import Path
import sys

HELPER = r'''
/* EasyStick C: CMD53 retention BB v3 (DMA, torn-safe, CMD53-attributed). */
#define EASYSTICK_CMD53_BB_PA		0xBBDEAD01u
#define EASYSTICK_CMD53_BB_MAGIC	0x45534242u
#define EASYSTICK_CMD53_BB_VERSION	2u
#define EASYSTICK_CMD53_BB_COMMIT_XOR	0xC0FFEE01u

struct easystick_cmd53_bb {
	u32 magic;
	u32 version;
	u32 generation;
	u32 event_seq;
	u32 commit;
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
	u32 cmd_status;
	u32 data_status;
	u32 idsts;
	u32 pending_cmd;
	u32 pending_data;
	s32 cmd_error;
	s32 data_error;
	u32 bytes_xfered;
	u32 reset_reason_hint;
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

static bool easystick_cmd53_bb_armed(void)
{
	volatile struct easystick_cmd53_bb *bb = easystick_cmd53_bb_ptr();
	u32 gen = READ_ONCE(bb->generation);

	return READ_ONCE(bb->magic) == EASYSTICK_CMD53_BB_MAGIC &&
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

	if (!cmd || cmd->opcode != SD_IO_RW_EXTENDED)
		return;
	bb = easystick_cmd53_bb_ptr();
	gen = READ_ONCE(bb->generation) + 1u;
	if (!gen)
		gen = 1u;
	/* Minimal arm: no per-field clear; stages are generation-tagged. */
	easystick_cmd53_bb_invalidate(bb);
	WRITE_ONCE(bb->magic, EASYSTICK_CMD53_BB_MAGIC);
	WRITE_ONCE(bb->version, EASYSTICK_CMD53_BB_VERSION);
	WRITE_ONCE(bb->generation, gen);
	WRITE_ONCE(bb->event_seq, 1);
	WRITE_ONCE(bb->cmd_arg, cmd->arg);
	WRITE_ONCE(bb->stage_request, gen);
	easystick_cmd53_bb_seal(bb, gen, 1);
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

static void easystick_cmd53_bb_mark_end(struct mmc_request *mrq)
{
	volatile struct easystick_cmd53_bb *bb;
	u32 gen;
	u32 seq;
	struct mmc_command *cmd;
	struct mmc_data *data;

	if (!mrq || !easystick_cmd53_bb_cmd_matches(mrq->cmd))
		return;
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
}

'''


def main() -> int:
    raise SystemExit(
        "CMD53_BB_GATE_FAIL: apply_kernel_hooks.py is retired; "
        "use gen_retention_bb_patches.py"
    )

    root = Path(sys.argv[1])
    c_path = root / "drivers/mmc/host/dw_mmc.c"
    c = c_path.read_text(encoding="utf-8")
    if "easystick_cmd53_bb_begin" in c:
        print("already applied")
        return 0

    anchor = "static void __dw_mci_start_request(struct dw_mci *host,\n"
    if anchor not in c:
        print("start_request anchor missing", file=sys.stderr)
        return 1
    c = c.replace(anchor, HELPER + anchor, 1)

    old = "\thost->dir_status = 0;\n\n\tdata = cmd->data;\n"
    new = "\thost->dir_status = 0;\n\teasystick_cmd53_bb_begin(cmd);\n\n\tdata = cmd->data;\n"
    if old not in c:
        # maybe 0051 already inserted mark_start
        old2 = "\thost->dir_status = 0;\n\tes_mmc_cmd53_mark_start(host, cmd);\n\n\tdata = cmd->data;\n"
        new2 = "\thost->dir_status = 0;\n\teasystick_cmd53_bb_begin(cmd);\n\n\tdata = cmd->data;\n"
        if old2 in c:
            c = c.replace(old2, new2, 1)
        else:
            print("dir_status anchor missing", file=sys.stderr)
            return 1
    else:
        c = c.replace(old, new, 1)

    # CMD error IRQ — after host->cmd_status = pending;
    if "easystick_cmd53_bb_mark_cmd_err(host->cmd, pending, pending);" not in c:
        old_cmd = (
            "\t\t\thost->cmd_status = pending;\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
        )
        new_cmd = (
            "\t\t\thost->cmd_status = pending;\n"
            "\t\t\teasystick_cmd53_bb_mark_cmd_err(host->cmd, pending, pending);\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
        )
        if old_cmd not in c:
            print("CMD err IRQ anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_cmd, new_cmd, 1)

    if "easystick_cmd53_bb_mark_data_err(pending, pending);" not in c:
        old_data = (
            "\t\t\thost->data_status = pending;\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n"
        )
        new_data = (
            "\t\t\thost->data_status = pending;\n"
            "\t\t\teasystick_cmd53_bb_mark_data_err(pending, pending);\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n"
        )
        if old_data not in c:
            # 0011 diagnostics insert a trace->data_error_seen block before RINTSTS
            old_data = (
                "\t\t\tmci_writel(host, RINTSTS, DW_MCI_DATA_ERROR_FLAGS);\n"
                "\t\t\thost->data_status = pending;\n"
                "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
                "\t\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n"
            )
            new_data = (
                "\t\t\tmci_writel(host, RINTSTS, DW_MCI_DATA_ERROR_FLAGS);\n"
                "\t\t\thost->data_status = pending;\n"
                "\t\t\teasystick_cmd53_bb_mark_data_err(pending, pending);\n"
                "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
                "\t\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n"
            )
        if old_data not in c:
            print("DATA err IRQ anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_data, new_data, 1)

    # CMD done interrupt
    if "easystick_cmd53_bb_mark_cmd_done(host->cmd);" not in c:
        old_cdone = (
            "\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\n"
            "\tdw_mci_start_fault_timer(host);\n"
        )
        new_cdone = (
            "\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\teasystick_cmd53_bb_mark_cmd_done(host->cmd);\n"
            "\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\n"
            "\tdw_mci_start_fault_timer(host);\n"
        )
        if old_cdone not in c:
            print("cmd_done anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_cdone, new_cdone, 1)

    # DATA_OVER
    if "easystick_cmd53_bb_mark_data_over(pending);" not in c:
        old_dover = (
            "\t\t\tif (!host->data_status)\n"
            "\t\t\t\thost->data_status = pending;\n"
        )
        new_dover = (
            "\t\t\tif (!host->data_status)\n"
            "\t\t\t\thost->data_status = pending;\n"
            "\t\t\teasystick_cmd53_bb_mark_data_over(pending);\n"
        )
        if old_dover not in c:
            print("DATA_OVER status anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_dover, new_dover, 1)

    # IDMAC TI/RI (64-bit then 32-bit). 0011 may insert trace->{dma_complete,...}.
    if c.count("easystick_cmd53_bb_mark_idmac(pending);") < 1:
        old_id = (
            "\t\tif (pending & (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI)) {\n"
            "\t\t\tif (trace) {\n"
            "\t\t\t\ttrace->dma_complete = 1;\n"
            "\t\t\t\ttrace->own_dma_irq = dw_mci_cmd53_trace_own(host);\n"
            "\t\t\t}\n"
            "\t\t\tmci_writel(host, IDSTS64, SDMMC_IDMAC_INT_TI |\n"
            "\t\t\t\t\t\t\tSDMMC_IDMAC_INT_RI);\n"
        )
        new_id = (
            "\t\tif (pending & (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI)) {\n"
            "\t\t\teasystick_cmd53_bb_mark_idmac(pending);\n"
            "\t\t\tif (trace) {\n"
            "\t\t\t\ttrace->dma_complete = 1;\n"
            "\t\t\t\ttrace->own_dma_irq = dw_mci_cmd53_trace_own(host);\n"
            "\t\t\t}\n"
            "\t\t\tmci_writel(host, IDSTS64, SDMMC_IDMAC_INT_TI |\n"
            "\t\t\t\t\t\t\tSDMMC_IDMAC_INT_RI);\n"
        )
        if old_id not in c:
            print("IDMAC 64-bit anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_id, new_id, 1)
    if c.count("easystick_cmd53_bb_mark_idmac(pending);") < 2:
        old_id32 = (
            "\t\tif (pending & (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI)) {\n"
            "\t\t\tif (trace) {\n"
            "\t\t\t\ttrace->dma_complete = 1;\n"
            "\t\t\t\ttrace->own_dma_irq = dw_mci_cmd53_trace_own(host);\n"
            "\t\t\t}\n"
            "\t\t\tmci_writel(host, IDSTS, SDMMC_IDMAC_INT_TI |\n"
            "\t\t\t\t\t\t\tSDMMC_IDMAC_INT_RI);\n"
        )
        new_id32 = (
            "\t\tif (pending & (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI)) {\n"
            "\t\t\teasystick_cmd53_bb_mark_idmac(pending);\n"
            "\t\t\tif (trace) {\n"
            "\t\t\t\ttrace->dma_complete = 1;\n"
            "\t\t\t\ttrace->own_dma_irq = dw_mci_cmd53_trace_own(host);\n"
            "\t\t\t}\n"
            "\t\t\tmci_writel(host, IDSTS, SDMMC_IDMAC_INT_TI |\n"
            "\t\t\t\t\t\t\tSDMMC_IDMAC_INT_RI);\n"
        )
        if old_id32 not in c:
            print("IDMAC 32-bit anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_id32, new_id32, 1)

    if "easystick_cmd53_bb_mark_cto(host->cmd);" not in c:
        old_cto = (
            "\t\thost->cmd_status = SDMMC_INT_RTO;\n"
            "\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\t\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\t\tbreak;\n"
        )
        new_cto = (
            "\t\thost->cmd_status = SDMMC_INT_RTO;\n"
            "\t\teasystick_cmd53_bb_mark_cto(host->cmd);\n"
            "\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\t\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\t\tbreak;\n"
        )
        if old_cto not in c:
            print("CTO anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_cto, new_cto, 1)

    if "easystick_cmd53_bb_mark_dto();" not in c:
        old_dto = (
            "\t\thost->data_status = SDMMC_INT_DRTO;\n"
            "\t\ttrace = dw_mci_cmd53_trace_active(host);\n"
            "\t\tif (trace)\n"
            "\t\t\ttrace->data_error_seen = 1;\n"
            "\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n"
            "\t\tset_bit(EVENT_DATA_COMPLETE, &host->pending_events);\n"
        )
        new_dto = (
            "\t\thost->data_status = SDMMC_INT_DRTO;\n"
            "\t\teasystick_cmd53_bb_mark_dto();\n"
            "\t\ttrace = dw_mci_cmd53_trace_active(host);\n"
            "\t\tif (trace)\n"
            "\t\t\ttrace->data_error_seen = 1;\n"
            "\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n"
            "\t\tset_bit(EVENT_DATA_COMPLETE, &host->pending_events);\n"
        )
        if old_dto not in c:
            print("DTO anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_dto, new_dto, 1)

    if "easystick_cmd53_bb_mark_end(mrq);" not in c:
        old_end = (
            "\tspin_unlock(&host->lock);\n"
            "\tmmc_request_done(prev_mmc, mrq);\n"
            "\tspin_lock(&host->lock);\n"
        )
        new_end = (
            "\tspin_unlock(&host->lock);\n"
            "\teasystick_cmd53_bb_mark_end(mrq);\n"
            "\tmmc_request_done(prev_mmc, mrq);\n"
            "\tspin_lock(&host->lock);\n"
        )
        if old_end not in c:
            print("request_end anchor missing", file=sys.stderr)
            return 1
        c = c.replace(old_end, new_end, 1)

    for need in (
        "easystick_cmd53_bb_begin(cmd);",
        "easystick_cmd53_bb_mark_cmd_err(host->cmd, pending, pending);",
        "easystick_cmd53_bb_mark_data_err(pending, pending);",
        "easystick_cmd53_bb_mark_cmd_done(host->cmd);",
        "easystick_cmd53_bb_mark_data_over(pending);",
        "easystick_cmd53_bb_mark_idmac(pending);",
        "easystick_cmd53_bb_mark_cto(host->cmd);",
        "easystick_cmd53_bb_mark_dto();",
        "easystick_cmd53_bb_mark_end(mrq);",
        "easystick_cmd53_bb_cmd_matches(",
    ):
        if need not in c:
            print(f"missing call site: {need}", file=sys.stderr)
            return 1

    assert c.count("{") == c.count("}")
    assert "EASYSTICK_CMD53_BB_PA" in c
    c_path.write_text(c, encoding="utf-8")
    print("dw_mmc.c OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
