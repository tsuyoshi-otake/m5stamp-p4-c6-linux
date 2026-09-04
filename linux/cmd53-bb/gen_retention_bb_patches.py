#!/usr/bin/env python3
"""Regenerate 0052 / 0053 / esp-hosted 0026 retention-BB patches from vendor trees."""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from kern_helper_text import KERN_HELPER, POST_ONLY_HELPER

HERE = Path(__file__).resolve().parent
LINUX_DIR = HERE.parent
FIRMWARE_DIR = LINUX_DIR.parent
REPO_LINUX_VENDOR = FIRMWARE_DIR / "vendor" / "linux"
REPO_ESP_HOSTED = FIRMWARE_DIR / "vendor" / "esp-hosted" / "esp_hosted_ng" / "host"
PATCH_DIR = LINUX_DIR / "kernel-patches"
ESP_PATCH_DIR = LINUX_DIR / "cmd53-bb" / "esp-hosted-patches"


def run_diff(old: Path, new: Path, rel: str) -> str:
    """Unified diff with paths a/rel and b/rel."""
    r = subprocess.run(
        # The generated retention hooks are applied after the optional
        # 0011/0013/0014 dw_mmc diagnostics.  Keep only one context line so
        # unrelated insertions in the same interrupt/timer block do not make
        # an otherwise independent hook reject.
        ["git", "diff", "--no-index", "--unified=1", "--", str(old), str(new)],
        capture_output=True,
        text=True,
    )
    # git diff --no-index returns 1 when files differ
    out = r.stdout
    if not out.strip():
        raise SystemExit(f"empty diff for {rel}")
    lines = []
    for line in out.splitlines(True):
        if line.startswith("diff --git "):
            lines.append(f"diff --git a/{rel} b/{rel}\n")
        elif line.startswith("--- "):
            lines.append(f"--- a/{rel}\n")
        elif line.startswith("+++ "):
            lines.append(f"+++ b/{rel}\n")
        else:
            lines.append(line)
    return "".join(lines)


def apply_dw_mmc(src: Path, dst: Path) -> None:
    text = src.read_text(encoding="utf-8")
    if "easystick_cmd53_bb_begin" in text:
        raise SystemExit("dw_mmc.c already has BB hooks — use clean vendor")

    anchor = "static void __dw_mci_start_request(struct dw_mci *host,\n"
    if anchor not in text:
        raise SystemExit("start_request anchor missing")
    text = text.replace(anchor, KERN_HELPER + "\n" + anchor, 1)

    old = "\thost->dir_status = 0;\n\n\tdata = cmd->data;\n"
    new = "\thost->dir_status = 0;\n\teasystick_cmd53_bb_begin(cmd);\n\n\tdata = cmd->data;\n"
    if old not in text:
        raise SystemExit("dir_status anchor missing")
    text = text.replace(old, new, 1)

    replacements = [
        (
            "\t\t\thost->cmd_status = pending;\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n",
            "\t\t\thost->cmd_status = pending;\n"
            "\t\t\teasystick_cmd53_bb_mark_cmd_err(host->cmd, pending, pending);\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n",
        ),
        (
            "\t\t\tmci_writel(host, RINTSTS, DW_MCI_DATA_ERROR_FLAGS);\n"
            "\t\t\thost->data_status = pending;\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n",
            "\t\t\tmci_writel(host, RINTSTS, DW_MCI_DATA_ERROR_FLAGS);\n"
            "\t\t\thost->data_status = pending;\n"
            "\t\t\teasystick_cmd53_bb_mark_data_err(pending, pending);\n"
            "\t\t\tsmp_wmb(); /* drain writebuffer */\n"
            "\t\t\tset_bit(EVENT_DATA_ERROR, &host->pending_events);\n",
        ),
        (
            "\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\n"
            "\tdw_mci_start_fault_timer(host);\n",
            "\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\teasystick_cmd53_bb_mark_cmd_done(host->cmd);\n"
            "\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\n"
            "\tdw_mci_start_fault_timer(host);\n",
        ),
        (
            "\t\t\tif (!host->data_status)\n"
            "\t\t\t\thost->data_status = pending;\n",
            "\t\t\tif (!host->data_status)\n"
            "\t\t\t\thost->data_status = pending;\n"
            "\t\t\teasystick_cmd53_bb_mark_data_over(pending);\n",
        ),
        (
            "\t\thost->cmd_status = SDMMC_INT_RTO;\n"
            "\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\t\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\t\tbreak;\n",
            "\t\thost->cmd_status = SDMMC_INT_RTO;\n"
            "\t\teasystick_cmd53_bb_mark_cto(host->cmd);\n"
            "\t\tset_bit(EVENT_CMD_COMPLETE, &host->pending_events);\n"
            "\t\tqueue_work(system_bh_wq, &host->bh_work);\n"
            "\t\tbreak;\n",
        ),
    ]
    for old_s, new_s in replacements:
        if old_s not in text:
            raise SystemExit(f"anchor missing:\n{old_s[:80]}...")
        text = text.replace(old_s, new_s, 1)

    # IDMAC TI/RI — appears once for each controller address width.  Place the
    # breadcrumb after the NI clear, where the preceding diagnostic patches do
    # not insert a trace block.  Placing it immediately after the if-opening
    # would make the generated hunk reject after 0011's trace block.
    idmac_clear_anchors = [
        (
            "\t\t\tmci_writel(host, IDSTS64, SDMMC_IDMAC_INT_NI);\n",
            "\t\t\tmci_writel(host, IDSTS64, SDMMC_IDMAC_INT_NI);\n"
            "\t\t\teasystick_cmd53_bb_mark_idmac(pending);\n",
        ),
        (
            "\t\t\tmci_writel(host, IDSTS, SDMMC_IDMAC_INT_NI);\n",
            "\t\t\tmci_writel(host, IDSTS, SDMMC_IDMAC_INT_NI);\n"
            "\t\t\teasystick_cmd53_bb_mark_idmac(pending);\n",
        ),
    ]
    for old_idmac, new_idmac in idmac_clear_anchors:
        if text.count(old_idmac) != 1:
            raise SystemExit("IDMAC NI-clear anchor count is not 1")
        text = text.replace(old_idmac, new_idmac, 1)

    # Boundary markers around IDMAC completion distinguish a cache-sync/DMA
    # completion stall from a later IRQ or bottom-half stall.  Keep both
    # address-width branches covered even though this target currently uses
    # the 32-bit register set.
    old_dma_complete = (
        "\t\t\tif (!test_bit(EVENT_DATA_ERROR, &host->pending_events))\n"
        "\t\t\t\thost->dma_ops->complete((void *)host);\n"
    )
    new_dma_complete = (
        "\t\t\tif (!test_bit(EVENT_DATA_ERROR, &host->pending_events)) {\n"
        "\t\t\t\teasystick_cmd53_bb_mark_host_stage(\n"
        "\t\t\t\t\thost, &easystick_cmd53_bb_ptr()->"
        "stage_idmac_complete_enter);\n"
        "\t\t\t\thost->dma_ops->complete((void *)host);\n"
        "\t\t\t\teasystick_cmd53_bb_mark_host_stage(\n"
        "\t\t\t\t\thost, &easystick_cmd53_bb_ptr()->"
        "stage_idmac_complete_exit);\n"
        "\t\t\t}\n"
    )
    dma_complete_count = text.count(old_dma_complete)
    if dma_complete_count != 2:
        raise SystemExit(
            "IDMAC complete anchor count "
            f"{dma_complete_count}, expected 2"
        )
    text = text.replace(old_dma_complete, new_dma_complete)

    # Mark entry to the MMC bottom half and the data-complete callback.  These
    # are retention writes only: no printk is added to the hot path.
    old_bh = "\tstruct dw_mci *host = from_work(host, t, bh_work);\n"
    new_bh = (
        old_bh
        + "\teasystick_cmd53_bb_mark_host_stage(\n"
        "\t\thost, &easystick_cmd53_bb_ptr()->stage_bh_enter);\n"
    )
    if text.count(old_bh) != 1:
        raise SystemExit("bottom-half entry anchor count is not 1")
    text = text.replace(old_bh, new_bh, 1)

    old_data_complete = "\t\t\terr = dw_mci_data_complete(host, data);\n"
    new_data_complete = (
        "\t\t\teasystick_cmd53_bb_mark_host_stage(\n"
        "\t\t\t\thost, &easystick_cmd53_bb_ptr()->"
        "stage_data_complete_enter);\n"
        "\t\t\terr = dw_mci_data_complete(host, data);\n"
        "\t\t\teasystick_cmd53_bb_mark_host_stage(\n"
        "\t\t\t\thost, &easystick_cmd53_bb_ptr()->"
        "stage_data_complete_exit);\n"
    )
    if text.count(old_data_complete) != 1:
        raise SystemExit("data-complete anchor count is not 1")
    text = text.replace(old_data_complete, new_data_complete, 1)

    # The final return is the last point in the IRQ handler.  Restrict the
    # replacement to that function so unrelated IRQ handlers stay untouched.
    irq_idx = text.find("static irqreturn_t dw_mci_interrupt(")
    if irq_idx < 0:
        raise SystemExit("dw_mci_interrupt missing")
    irq_sub = text[irq_idx:]
    old_irq_return = "\treturn IRQ_HANDLED;\n}\n"
    new_irq_return = (
        "\teasystick_cmd53_bb_mark_host_stage(\n"
        "\t\thost, &easystick_cmd53_bb_ptr()->stage_irq_exit);\n"
        "\treturn IRQ_HANDLED;\n"
        "}\n"
    )
    if irq_sub.count(old_irq_return) != 1:
        raise SystemExit("dw_mci_interrupt return anchor count is not 1")
    text = text[:irq_idx] + irq_sub.replace(old_irq_return, new_irq_return, 1)

    # DTO — optional trace block
    # 0011 inserts its trace bookkeeping immediately after data_status.  Put
    # the DTO marker immediately before queue_work instead, using an anchor
    # that remains adjacent after that earlier diagnostic insertion.
    dto_queue = "\t\tqueue_work(system_bh_wq, &host->bh_work);\n"
    dto_marked_queue = (
        "\t\teasystick_cmd53_bb_mark_dto();\n"
        + dto_queue
    )
    dto_idx = text.find("static void dw_mci_dto_timer(")
    if dto_idx < 0:
        raise SystemExit("dw_mci_dto_timer missing")
    dto_sub = text[dto_idx:]
    if dto_sub.count(dto_queue) != 1:
        raise SystemExit("DTO queue anchor count is not 1")
    text = text[:dto_idx] + dto_sub.replace(dto_queue, dto_marked_queue, 1)

    old_end = (
        "\tspin_unlock(&host->lock);\n"
        "\tmmc_request_done(prev_mmc, mrq);\n"
        "\tspin_lock(&host->lock);\n"
    )
    new_end = (
        "\tbb_gen = easystick_cmd53_bb_mark_end_and_get_gen(mrq);\n"
        "\tspin_unlock(&host->lock);\n"
        "\tmmc_request_done(prev_mmc, mrq);\n"
        "\teasystick_cmd53_bb_mark_bb2_gen(bb_gen);\n"
        "\tspin_lock(&host->lock);\n"
        "\teasystick_cmd53_bb_mark_bb3_gen(bb_gen,\n"
        "\t\t\t\t\t(u32)host->state,\n"
        "\t\t\t\t\t(u32)host->pending_events);\n"
    )
    # Only the request_end function — match with surrounding uniqueness
    # Find dw_mci_request_end's closing sequence
    idx = text.find("static void dw_mci_request_end(")
    if idx < 0:
        raise SystemExit("dw_mci_request_end missing")
    sub = text[idx:]
    old_decl = "\tstruct mmc_host\t*prev_mmc = host->slot->mmc;\n"
    new_decl = (
        old_decl
        + "\tu32 bb_gen;\n"
        + "\teasystick_cmd53_bb_mark_request_end_stage(\n"
        + "\t\tmrq, &easystick_cmd53_bb_ptr()->stage_request_end_enter);\n"
    )
    if old_decl not in sub:
        raise SystemExit("request_end declaration anchor missing")
    sub = sub.replace(old_decl, new_decl, 1)

    old_next = (
        "\t\thost->state = STATE_SENDING_CMD;\n"
        "\t\tdw_mci_start_request(host, slot);\n"
    )
    new_next = (
        "\t\thost->state = STATE_SENDING_CMD;\n"
        "\t\teasystick_cmd53_bb_mark_request_end_stage(\n"
        "\t\t\tmrq, &easystick_cmd53_bb_ptr()->"
        "stage_request_end_before_next);\n"
        "\t\tdw_mci_start_request(host, slot);\n"
        "\t\teasystick_cmd53_bb_mark_request_end_stage(\n"
        "\t\t\tmrq, &easystick_cmd53_bb_ptr()->"
        "stage_request_end_after_next);\n"
    )
    if sub.count(old_next) != 1:
        raise SystemExit("request_end next-request anchor count is not 1")
    sub = sub.replace(old_next, new_next, 1)

    old_idle = (
        "\t\tif (host->state == STATE_SENDING_CMD11)\n"
        "\t\t\thost->state = STATE_WAITING_CMD11_DONE;\n"
        "\t\telse\n"
        "\t\t\thost->state = STATE_IDLE;\n"
    )
    new_idle = (
        old_idle
        + "\t\teasystick_cmd53_bb_mark_request_end_stage(\n"
        + "\t\t\tmrq, &easystick_cmd53_bb_ptr()->stage_request_end_idle);\n"
    )
    if sub.count(old_idle) != 1:
        raise SystemExit("request_end idle anchor count is not 1")
    sub = sub.replace(old_idle, new_idle, 1)

    old_end_with_brace = old_end + "}"
    if sub.count(old_end_with_brace) != 1:
        raise SystemExit("request_end unlock/done anchor count is not 1")
    replacement = new_end + "\n}"
    sub = sub.replace(old_end_with_brace, replacement, 1)
    if "bb_gen = easystick_cmd53_bb_mark_end_and_get_gen(mrq);" not in sub:
        raise SystemExit("request_end BB1 seal was not injected")
    text = text[:idx] + sub

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8", newline="\n")


def apply_core(src: Path, dst: Path) -> None:
    text = src.read_text(encoding="utf-8")
    if "easystick_cmd53_bb_mark_bb4" in text:
        raise SystemExit("core.c already patched")
    # Insert helpers before mmc_wait_for_req
    anchor = "void mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq)\n"
    if anchor not in text:
        raise SystemExit("mmc_wait_for_req anchor missing")
    text = text.replace(anchor, POST_ONLY_HELPER + "\n" + anchor, 1)
    old = (
        "void mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq)\n"
        "{\n"
        "\t__mmc_start_req(host, mrq);\n"
        "\n"
        "\tif (!mrq->cap_cmd_during_tfr)\n"
        "\t\tmmc_wait_for_req_done(host, mrq);\n"
        "}\n"
    )
    new = (
        "void mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq)\n"
        "{\n"
        "\t__mmc_start_req(host, mrq);\n"
        "\n"
        "\tif (!mrq->cap_cmd_during_tfr)\n"
        "\t\tmmc_wait_for_req_done(host, mrq);\n"
        "\teasystick_cmd53_bb_mark_bb4(mrq);\n"
        "}\n"
    )
    if old not in text:
        raise SystemExit("mmc_wait_for_req body anchor missing")
    text = text.replace(old, new, 1)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8", newline="\n")


def apply_sdio_io(src: Path, dst: Path) -> None:
    text = src.read_text(encoding="utf-8")
    if "easystick_cmd53_bb_mark_bb5" in text:
        raise SystemExit("sdio_io.c already patched")
    # Need barrier/WRITE_ONCE — sdio_io already includes mmc headers.
    # Insert a trimmed helper: reuse POST_ONLY but only bb5 call site needs
    # the full helper once. Prefer include of same POST_ONLY_HELPER before
    # sdio_memcpy_toio.
    anchor = (
        "int sdio_memcpy_toio(struct sdio_func *func, unsigned int addr,\n"
        "\tvoid *src, int count)\n"
    )
    if anchor not in text:
        raise SystemExit("sdio_memcpy_toio anchor missing")
    text = text.replace(anchor, POST_ONLY_HELPER + "\n" + anchor, 1)
    old = (
        "int sdio_memcpy_toio(struct sdio_func *func, unsigned int addr,\n"
        "\tvoid *src, int count)\n"
        "{\n"
        "\treturn sdio_io_rw_ext_helper(func, 1, addr, 1, src, count);\n"
        "}\n"
    )
    new = (
        "int sdio_memcpy_toio(struct sdio_func *func, unsigned int addr,\n"
        "\tvoid *src, int count)\n"
        "{\n"
        "\tint ret = sdio_io_rw_ext_helper(func, 1, addr, 1, src, count);\n"
        "\teasystick_cmd53_bb_mark_bb5(ret);\n"
        "\treturn ret;\n"
        "}\n"
    )
    if old not in text:
        raise SystemExit("sdio_memcpy_toio body missing")
    text = text.replace(old, new, 1)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8", newline="\n")


def apply_esp(src: Path, dst: Path) -> None:
    text = src.read_text(encoding="utf-8")
    if "easystick_cmd53_bb_mark_bb6" in text:
        raise SystemExit("esp_sdio_api.c already patched")
    # Insert after includes block — find first function
    # Use POST_ONLY_HELPER but strip mmc_request-dependent mark_bb4 if it
    # causes compile issues — esp_sdio_api.c may not include mmc_host.h.
    # Provide bb6-only helper.
    helper = POST_ONLY_HELPER
    # Remove mark_bb4 (needs struct mmc_request) to avoid incomplete type.
    start = helper.find("static void easystick_cmd53_bb_mark_bb4")
    end = helper.find("static void easystick_cmd53_bb_mark_bb5")
    if start < 0 or end < 0:
        raise SystemExit("POST_ONLY_HELPER shape unexpected")
    helper = helper[:start] + helper[end:]

    # Drop mark_bb5 too — only bb6 needed here.
    start = helper.find("static void easystick_cmd53_bb_mark_bb5")
    end = helper.find("static void easystick_cmd53_bb_mark_bb6")
    helper = helper[:start] + helper[end:]

    old = (
        "int esp_write_block(struct esp_sdio_context *context, u32 reg, u8 *data, u16 size, u8 is_lock_needed)\n"
        "{\n"
        "\tif (size <= 1) {\n"
        "\t\treturn esp_write_byte(context, reg, *data, is_lock_needed);\n"
        "\t} else {\n"
        "\t\treturn esp_write_multi_byte(context, reg, data, size, is_lock_needed);\n"
        "\t}\n"
        "}\n"
    )
    new = (
        "int esp_write_block(struct esp_sdio_context *context, u32 reg, u8 *data, u16 size, u8 is_lock_needed)\n"
        "{\n"
        "\tint ret;\n"
        "\n"
        "\tif (size <= 1)\n"
        "\t\tret = esp_write_byte(context, reg, *data, is_lock_needed);\n"
        "\telse\n"
        "\t\tret = esp_write_multi_byte(context, reg, data, size, is_lock_needed);\n"
        "\teasystick_cmd53_bb_mark_bb6(ret);\n"
        "\treturn ret;\n"
        "}\n"
    )
    if old not in text:
        raise SystemExit("esp_write_block body missing")
    # Place helper just before esp_write_block
    text = text.replace(
        "int esp_write_block(struct esp_sdio_context *context, u32 reg, u8 *data, u16 size, u8 is_lock_needed)\n",
        helper + "\nint esp_write_block(struct esp_sdio_context *context, u32 reg, u8 *data, u16 size, u8 is_lock_needed)\n",
        1,
    )
    text = text.replace(old, new, 1)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    vendor = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO_LINUX_VENDOR
    esp_host = Path(sys.argv[2]) if len(sys.argv) > 2 else REPO_ESP_HOSTED
    dw = vendor / "drivers/mmc/host/dw_mmc.c"
    core = vendor / "drivers/mmc/core/core.c"
    sdio = vendor / "drivers/mmc/core/sdio_io.c"
    esp = esp_host / "sdio/esp_sdio_api.c"
    for p in (dw, core, sdio, esp):
        if not p.is_file():
            raise SystemExit(f"missing {p}")

    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        apply_dw_mmc(dw, t / "dw_mmc.c")
        apply_core(core, t / "core.c")
        apply_sdio_io(sdio, t / "sdio_io.c")
        apply_esp(esp, t / "esp_sdio_api.c")

        p52 = (
            "Subject: [PATCH] mmc: dw_mmc: EasyStick CMD53 retention BB (0052/0053 BB1-3)\n"
            "\n"
            "v6: FOCUS_ARG arm; BB1=end; BB2 post mmc_request_done; BB3 request_end; "
            "DMA/IRQ/bottom-half/request-end boundaries.\n"
            "Placeholder PA 0xBBDEAD01u substituted by build-cmd53-bb.sh.\n"
            "\n"
            + run_diff(dw, t / "dw_mmc.c", "drivers/mmc/host/dw_mmc.c")
        )
        p53 = (
            "Subject: [PATCH] mmc: EasyStick CMD53 retention BB post-done (0053 BB4-5)\n"
            "\n"
            "BB4 after mmc_wait_for_req; BB5 after sdio_memcpy_toio. No printk.\n"
            "Placeholder PA 0xBBDEAD01u substituted by build-cmd53-bb.sh.\n"
            "\n"
            + run_diff(core, t / "core.c", "drivers/mmc/core/core.c")
            + run_diff(sdio, t / "sdio_io.c", "drivers/mmc/core/sdio_io.c")
        )
        p26 = (
            "Subject: [PATCH] esp-hosted: CMD53 retention BB6 after esp_write_block\n"
            "\n"
            "No printk. Placeholder PA 0xBBDEAD01u rendered with CMD53 BB build.\n"
            "\n"
            + run_diff(esp, t / "esp_sdio_api.c", "sdio/esp_sdio_api.c")
        )

        PATCH_DIR.mkdir(parents=True, exist_ok=True)
        ESP_PATCH_DIR.mkdir(parents=True, exist_ok=True)
        (PATCH_DIR / "0052-easystick-dw-mmc-cmd53-retention-bb.patch").write_text(
            p52, encoding="utf-8", newline="\n"
        )
        (PATCH_DIR / "0053-easystick-mmc-cmd53-post-bb.patch").write_text(
            p53, encoding="utf-8", newline="\n"
        )
        (ESP_PATCH_DIR / "0026-easystick-cmd53-retention-bb6.patch").write_text(
            p26, encoding="utf-8", newline="\n"
        )
        print("wrote 0052, 0053, esp-hosted 0026")
    return 0


if __name__ == "__main__":
    # Allow running as script from cmd53-bb/
    sys.path.insert(0, str(HERE))
    raise SystemExit(main())
