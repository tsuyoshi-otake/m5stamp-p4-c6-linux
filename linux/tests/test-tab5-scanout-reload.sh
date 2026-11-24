#!/bin/sh
# Tab5: the panel's scan-out runs without the CPU (#42).  The DW-GDMA
# channel that feeds the DSI bridge used to run in ESP-IDF's link-list
# mode, which stops one frame after the last re-arm; the kernel re-armed
# it from an interrupt, and fbcon drawing boot messages with interrupts
# off for longer than a frame made the panel flash white or cyan.  The
# boot shim now switches the channel to auto-reload before the jump and
# the kernel driver (linux/tab5/0141) only checks it.  This applies the
# driver's patch stack and checks both halves and the DTS node.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
tab5="$repo_root/linux/tab5"
shim="$repo_root/linux/boot-shim/main/tab5_display.c"
dts="$repo_root/linux/dts/esp32p4-tab5.dts"
fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

# 1. The driver patch stack applies: 0095 creates the file, 0133 and
# 0141 change it.  (0095's Makefile/Kconfig hunks need a kernel tree;
# only the new file is taken from it.)
drv="$work/drivers/misc/esp32p4_dsi_scanout.c"
mkdir -p "$work/drivers/misc"
awk '/^\+\+\+ b\/drivers\/misc\/esp32p4_dsi_scanout.c/ { p = 1; next }
	p && /^--- a\// { p = 0 }
	p && /^@@/ { next }
	p { sub(/^\+/, ""); print }' "$tab5/0095-esp32p4-dsi-scanout.patch" > "$drv"
applied=1
for p in 0133-tab5-scanout-lli-guard.patch 0141-tab5-scanout-auto-reload.patch; do
	if ! (cd "$work" && patch -p1 -s --no-backup-if-mismatch < "$tab5/$p") > "$work/patch.log" 2>&1; then
		bad "$p does not apply: $(cat "$work/patch.log")"
		applied=0
	fi
done
[ "$applied" -eq 1 ] && ok "0095 + 0133 + 0141 apply"

# 2. The driver programs auto-reload (src and dst multblk_type 1), drops
# the item's valid/last markers and block-done interrupt, and claims no
# interrupt at all.
if [ "$applied" -eq 1 ]; then
	grep -q '#define DWGDMA_CFG0_RELOAD	0x5u' "$drv" &&
	grep -q '| DWGDMA_CFG0_RELOAD,' "$drv" &&
		ok "driver sets cfg0 to auto-reload" ||
		bad "driver does not set cfg0 multblk_type to auto-reload"
	grep -q 'writel(s->ctl1 & ~(DWGDMA_CTL1_IOC_BLKTFR | DWGDMA_CTL1_LAST |' "$drv" &&
		ok "driver drops IOC/LAST/VALID from ctl1" ||
		bad "driver keeps a block marker in ctl1"
	if grep -qE 'request_irq|platform_get_irq|irqreturn_t' "$drv"; then
		bad "driver still uses an interrupt"
	else
		ok "driver uses no interrupt"
	fi
	# A running channel is never written: probe returns on a
	# free-running channel and waits for a stall before starting.
	probe=$(awk '/^static int dsi_scanout_probe/,/^}/' "$drv")
	first_free=$(printf '%s\n' "$probe" | grep -n 'dsi_scanout_free_running(s)' | head -1 | cut -d: -f1)
	poll=$(printf '%s\n' "$probe" | grep -n 'readl_poll_timeout' | head -1 | cut -d: -f1)
	start=$(printf '%s\n' "$probe" | grep -n 'dsi_scanout_start(s);' | head -1 | cut -d: -f1)
	if [ -n "$first_free" ] && [ -n "$poll" ] && [ -n "$start" ] &&
		[ "$first_free" -lt "$poll" ] && [ "$poll" -lt "$start" ]; then
		ok "probe checks free-run, then waits for a stall, then starts"
	else
		bad "probe order free-run/stall/start: $first_free/$poll/$start"
	fi
	grep -q '\.dev_groups = dsi_scanout_groups,' "$drv" &&
		! grep -q 'device_create_file' "$drv" &&
		ok "sysfs attribute is a driver group" ||
		bad "sysfs attribute is not a driver group"
fi

# 3. The shim switches the stalled channel to auto-reload at the handoff
# and counts frames with interrupts still off.
reload_fn=$(awk '/^static void scanout_start_reload/,/^}/' "$shim")
printf '%s\n' "$reload_fn" | grep -q 'set_src_multi_block_type(gdma, ch,' &&
printf '%s\n' "$reload_fn" | grep -q 'set_dst_multi_block_type(gdma, ch,' &&
[ "$(printf '%s\n' "$reload_fn" | grep -c 'DW_GDMA_BLOCK_TRANSFER_RELOAD')" -eq 2 ] &&
	ok "shim programs src and dst auto-reload" ||
	bad "scanout_start_reload does not set both multi-block types to reload"
printf '%s\n' "$reload_fn" | grep -q 'DWGDMA_CTRL_IOC_BLKTFR' &&
	ok "shim drops the block-done interrupt" ||
	bad "scanout_start_reload keeps the block-done interrupt"
status_fn=$(awk '/^void tab5_display_status/,/^}/' "$shim")
s_start=$(printf '%s\n' "$status_fn" | grep -n 'scanout_start_reload(s_scanout_chan);' | cut -d: -f1)
s_sample=$(printf '%s\n' "$status_fn" | grep -n 'free_frames = gdma_sample_wraps' | cut -d: -f1)
s_mie=$(printf '%s\n' "$status_fn" | grep -n 'RV_SET_CSR(mstatus, MSTATUS_MIE)' | cut -d: -f1)
if [ -n "$s_start" ] && [ -n "$s_sample" ] && [ -n "$s_mie" ] &&
	[ "$s_start" -lt "$s_sample" ] && [ "$s_sample" -lt "$s_mie" ]; then
	ok "handoff starts auto-reload and samples it before MIE comes back"
else
	bad "handoff order start/sample/MIE: $s_start/$s_sample/$s_mie"
fi

# 4. The DTS node claims no interrupt.
node=$(awk '/dsi_scanout: dsi-scanout@50081000 \{/,/\};/' "$dts")
if [ -z "$node" ]; then
	bad "no dsi-scanout node in $dts"
elif printf '%s\n' "$node" | grep -q 'interrupts'; then
	bad "dsi-scanout node still has an interrupt"
else
	ok "dsi-scanout node has no interrupt"
fi

[ "$fails" -eq 0 ] || { echo "FAILED: $fails"; exit 1; }
echo "PASS"
